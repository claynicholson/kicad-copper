/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2026 Copper Dev, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <copper/copper_client.h>
#include <copper/copper_auth.h>

#include <kicad_curl/kicad_curl_easy.h>
#include <curl/curl.h>     // CURLOPT_WRITEFUNCTION / curl_easy_setopt — not transitively exposed by kicad_curl_easy.h on all platforms
#include <nlohmann/json.hpp>

#include <wx/log.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>


namespace COPPER
{

bool DebugEnabled()
{
    static const bool enabled = []()
    {
        const char* v = std::getenv( "COPPER_DEBUG" );
        return v && *v && std::strcmp( v, "0" ) != 0;
    }();

    return enabled;
}


std::string DebugLogPath()
{
    const char* tmp = std::getenv( "TEMP" );

    if( !tmp )
        tmp = std::getenv( "TMP" );

    if( !tmp )
        tmp = std::getenv( "TMPDIR" );

    std::string dir = tmp ? tmp : ".";
    return dir + "/copper_debug.log";
}


void DebugLog( const std::string& aLine )
{
    if( !DebugEnabled() )
        return;

    // Workers and the UI thread both log; serialize appends.
    static std::mutex mtx;
    std::lock_guard<std::mutex> lock( mtx );

    std::ofstream f( DebugLogPath(), std::ios::app );

    if( !f )
        return;

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t( now );
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      now.time_since_epoch() ).count() % 1000;
    std::tm tmBuf;
#ifdef _WIN32
    localtime_s( &tmBuf, &t );
#else
    localtime_r( &t, &tmBuf );
#endif
    f << std::put_time( &tmBuf, "%Y-%m-%d %H:%M:%S" ) << '.'
      << std::setw( 3 ) << std::setfill( '0' ) << ms << "  " << aLine << '\n';
}


/// Clip long payloads so the log stays readable.
static std::string debugTruncate( const std::string& aText, size_t aMax = 2000 )
{
    if( aText.size() <= aMax )
        return aText;

    return aText.substr( 0, aMax ) + "... [" + std::to_string( aText.size() ) + " bytes total]";
}

CLIENT::CLIENT( AUTH* aAuth ) :
    m_auth( aAuth )
{
}


CLIENT::~CLIENT()
{
    Cancel();

    if( m_workerThread && m_workerThread->joinable() )
        m_workerThread->join();
}


void CLIENT::Chat( const std::string& aPrompt, const SchematicContext& aCtx,
                   ResponseCallback aOnResponse, ErrorCallback aOnError )
{
    nlohmann::json body = buildRequestBody( aPrompt, aCtx );
    body["intent"] = "chat";
    doPost( "/api/v1/chat", body, aOnResponse, aOnError );
}


void CLIENT::Recommend( const std::string& aPrompt, const SchematicContext& aCtx,
                        ResponseCallback aOnResponse, ErrorCallback aOnError )
{
    nlohmann::json body = buildRequestBody( aPrompt, aCtx );
    body["intent"] = "recommend";
    doPost( "/api/v1/recommend", body, aOnResponse, aOnError );
}


void CLIENT::Generate( const std::string& aPrompt, const SchematicContext& aCtx,
                       SSECallback aOnEvent, ErrorCallback aOnError )
{
    nlohmann::json body = buildRequestBody( aPrompt, aCtx );
    body["intent"] = "generate";
    doStreamPost( "/api/v1/generate", body, aOnEvent, aOnError );
}


void CLIENT::Plan( const std::string& aPrompt, const SchematicContext& aCtx,
                   ResponseCallback aOnResponse, ErrorCallback aOnError )
{
    nlohmann::json body = buildRequestBody( aPrompt, aCtx );
    body["intent"] = "plan";
    doPost( "/api/v1/plan", body, aOnResponse, aOnError );
}


void CLIENT::Cancel()
{
    m_cancelled.store( true );
}


nlohmann::json CLIENT::buildRequestBody( const std::string& aPrompt,
                                         const SchematicContext& aCtx ) const
{
    nlohmann::json body;
    body["prompt"] = aPrompt;
    body["context"] = aCtx.toJson();
    return body;
}


void CLIENT::doPost( const std::string& aEndpoint, const nlohmann::json& aBody,
                     ResponseCallback aOnResponse, ErrorCallback aOnError )
{
    // Wait for any previous request to finish
    if( m_workerThread && m_workerThread->joinable() )
        m_workerThread->join();

    m_busy.store( true );
    m_cancelled.store( false );

    m_workerThread = std::make_unique<std::thread>(
        [this, aEndpoint, aBody, aOnResponse, aOnError]()
        {
            // Auth is optional — the hosted backend accepts anonymous
            // requests. Refresh opportunistically so a token is attached
            // when one is available.
            if( !m_auth->IsAuthenticated() )
                m_auth->RefreshToken();

            KICAD_CURL_EASY curl;

            std::string url = m_auth->GetApiUrl() + aEndpoint;
            curl.SetURL( url );
            curl.SetUserAgent( "KiCad-Copper/1.0" );
            curl.SetHeader( "Content-Type", "application/json" );
            std::string token = m_auth->GetAccessToken();

            if( !token.empty() )
                curl.SetHeader( "Authorization", "Bearer " + token );
            curl.SetPostFields( aBody.dump() );

            DebugLog( "POST " + url + " auth=" + ( token.empty() ? "none" : "bearer" )
                      + " body=" + debugTruncate( aBody.dump() ) );

            int rc = curl.Perform();

            if( m_cancelled.load() )
            {
                DebugLog( "POST " + url + " cancelled" );
                m_busy.store( false );
                return;
            }

            if( rc != 0 )
            {
                DebugLog( "POST " + url + " curl error rc=" + std::to_string( rc )
                          + " (" + curl.GetErrorText( rc ) + ")" );
                m_busy.store( false );

                if( aOnError )
                    aOnError( "Request failed: " + curl.GetErrorText( rc ) );

                return;
            }

            int status = curl.GetResponseStatusCode();

            if( status != 200 )
            {
                DebugLog( "POST " + url + " HTTP " + std::to_string( status )
                          + " body=" + debugTruncate( curl.GetBuffer() ) );
                m_busy.store( false );

                if( aOnError )
                    aOnError( "HTTP " + std::to_string( status ) );

                return;
            }

            try
            {
                nlohmann::json resp = nlohmann::json::parse( curl.GetBuffer() );
                CopperResponse result = CopperResponse::fromJson( resp );
                DebugLog( "POST " + url + " HTTP 200 body="
                          + debugTruncate( curl.GetBuffer() ) );
                m_busy.store( false );

                if( aOnResponse )
                    aOnResponse( result );
            }
            catch( const nlohmann::json::exception& e )
            {
                DebugLog( "POST " + url + " JSON parse error: " + e.what()
                          + " body=" + debugTruncate( curl.GetBuffer() ) );
                m_busy.store( false );

                if( aOnError )
                    aOnError( std::string( "JSON parse error: " ) + e.what() );
            }
        } );
}


/// Helper struct for curl write callback data during SSE streaming
struct SSEWriteData
{
    SSECallback     onEvent;
    std::string     buffer;       // Accumulates partial lines
    std::string     currentEvent; // Current event type
    std::string     currentData;  // Current data payload
    std::atomic<bool>* cancelled;
};


/// curl write callback that parses SSE stream in real-time
static size_t sseWriteCallback( char* aPtr, size_t aSize, size_t aNmemb, void* aUserData )
{
    size_t totalBytes = aSize * aNmemb;
    SSEWriteData* ctx = static_cast<SSEWriteData*>( aUserData );

    if( ctx->cancelled && ctx->cancelled->load() )
        return 0;  // Abort the transfer

    ctx->buffer.append( aPtr, totalBytes );

    // Process complete lines from the buffer
    size_t pos;

    while( ( pos = ctx->buffer.find( '\n' ) ) != std::string::npos )
    {
        std::string line = ctx->buffer.substr( 0, pos );
        ctx->buffer.erase( 0, pos + 1 );

        // Remove trailing \r if present
        if( !line.empty() && line.back() == '\r' )
            line.pop_back();

        if( line.empty() )
        {
            // Empty line = end of event; dispatch if we have data
            if( !ctx->currentData.empty() )
            {
                SSEEvent evt;
                evt.event = ctx->currentEvent.empty() ? "message" : ctx->currentEvent;
                evt.data = ctx->currentData;

                DebugLog( "SSE event=" + evt.event + " data=" + debugTruncate( evt.data, 500 ) );

                if( ctx->onEvent )
                    ctx->onEvent( evt );

                ctx->currentEvent.clear();
                ctx->currentData.clear();
            }
        }
        else if( line.rfind( "event:", 0 ) == 0 )
        {
            // "event: <type>"
            ctx->currentEvent = line.substr( 6 );

            // Trim leading space
            if( !ctx->currentEvent.empty() && ctx->currentEvent[0] == ' ' )
                ctx->currentEvent.erase( 0, 1 );
        }
        else if( line.rfind( "data:", 0 ) == 0 )
        {
            // "data: <payload>"
            std::string data = line.substr( 5 );

            if( !data.empty() && data[0] == ' ' )
                data.erase( 0, 1 );

            if( !ctx->currentData.empty() )
                ctx->currentData += "\n";

            ctx->currentData += data;
        }
        // Ignore comment lines (starting with ':') and unknown fields
    }

    return totalBytes;
}


void CLIENT::doStreamPost( const std::string& aEndpoint, const nlohmann::json& aBody,
                           SSECallback aOnEvent, ErrorCallback aOnError )
{
    // Wait for any previous request to finish
    if( m_workerThread && m_workerThread->joinable() )
        m_workerThread->join();

    m_busy.store( true );
    m_cancelled.store( false );

    m_workerThread = std::make_unique<std::thread>(
        [this, aEndpoint, aBody, aOnEvent, aOnError]()
        {
            // Auth is optional — the hosted backend accepts anonymous
            // requests. Refresh opportunistically so a token is attached
            // when one is available.
            if( !m_auth->IsAuthenticated() )
                m_auth->RefreshToken();

            KICAD_CURL_EASY curl;

            std::string url = m_auth->GetApiUrl() + aEndpoint;
            curl.SetURL( url );
            curl.SetUserAgent( "KiCad-Copper/1.0" );
            curl.SetHeader( "Content-Type", "application/json" );
            curl.SetHeader( "Accept", "text/event-stream" );
            std::string token = m_auth->GetAccessToken();

            if( !token.empty() )
                curl.SetHeader( "Authorization", "Bearer " + token );
            curl.SetPostFields( aBody.dump() );

            // Set up SSE streaming via the curl write callback
            SSEWriteData writeData;
            writeData.onEvent = aOnEvent;
            writeData.cancelled = &m_cancelled;

            CURL* handle = curl.GetCurl();

            // We need to use the raw curl handle to set the write callback,
            // since KICAD_CURL_EASY doesn't expose this directly
            curl_easy_setopt( handle, CURLOPT_WRITEFUNCTION, sseWriteCallback );
            curl_easy_setopt( handle, CURLOPT_WRITEDATA, &writeData );

            DebugLog( "POST(stream) " + url + " auth=" + ( token.empty() ? "none" : "bearer" )
                      + " body=" + debugTruncate( aBody.dump() ) );

            int rc = curl.Perform();

            if( m_cancelled.load() )
            {
                DebugLog( "POST(stream) " + url + " cancelled" );
                m_busy.store( false );
                return;
            }

            if( rc != 0 )
            {
                DebugLog( "POST(stream) " + url + " curl error rc=" + std::to_string( rc )
                          + " (" + curl.GetErrorText( rc ) + ")" );
                m_busy.store( false );

                if( aOnError )
                    aOnError( "Stream request failed: " + curl.GetErrorText( rc ) );

                return;
            }

            int status = curl.GetResponseStatusCode();

            if( status != 200 )
            {
                // The error body went through the SSE callback; whatever is
                // still buffered there is the closest thing to a payload.
                DebugLog( "POST(stream) " + url + " HTTP " + std::to_string( status )
                          + " residual=" + debugTruncate( writeData.buffer, 500 ) );
                m_busy.store( false );

                if( aOnError )
                    aOnError( "HTTP " + std::to_string( status ) );

                return;
            }

            DebugLog( "POST(stream) " + url + " HTTP 200 stream complete" );
            m_busy.store( false );
        } );
}

}  // namespace COPPER
