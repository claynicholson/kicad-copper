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

#include <copper/copper_auth.h>

#include <kicad_curl/kicad_curl_easy.h>
#include <oauth/oauth_pkce.h>
#include <nlohmann/json.hpp>

#include <wx/utils.h>
#include <wx/log.h>

#include <chrono>


wxDEFINE_EVENT( COPPER_EVT_AUTH_SUCCESS, wxCommandEvent );
wxDEFINE_EVENT( COPPER_EVT_AUTH_FAILURE, wxCommandEvent );


namespace COPPER
{

const wxString AUTH::PROVIDER_ID   = wxS( "copper" );
const wxString AUTH::ACCOUNT_ID    = wxS( "default" );
const wxString AUTH::CLIENT_ID     = wxS( "kicad-copper" );
const wxString AUTH::CALLBACK_PATH = wxS( "/callback" );


AUTH::AUTH( const std::string& aApiUrl ) :
    m_apiUrl( aApiUrl )
{
}


AUTH::~AUTH()
{
    CancelLogin();
}


void AUTH::StartLogin( wxEvtHandler* aEventSink )
{
    m_eventSink = aEventSink;

    // Generate PKCE parameters
    m_session.code_verifier = OAUTH_PKCE::GenerateCodeVerifier();
    m_session.state = OAUTH_PKCE::GenerateState();
    m_session.client_id = CLIENT_ID;
    m_session.scope = wxS( "schematic" );
    m_session.authorization_endpoint =
            wxString::FromUTF8( m_apiUrl ) + wxS( "/auth/authorize" );

    // Create and start the loopback server for the OAuth callback
    m_loopbackServer = std::make_unique<OAUTH_LOOPBACK_SERVER>(
            this, CALLBACK_PATH, m_session.state );

    if( !m_loopbackServer->Start() )
    {
        wxLogError( "Copper: failed to start OAuth callback server" );

        if( m_eventSink )
        {
            wxCommandEvent* evt = new wxCommandEvent( COPPER_EVT_AUTH_FAILURE );
            evt->SetString( wxS( "Failed to start callback server" ) );
            wxQueueEvent( m_eventSink, evt );
        }

        m_loopbackServer.reset();
        return;
    }

    // Set the redirect URI from the loopback server's actual port
    m_session.redirect_uri = m_loopbackServer->GetRedirectUri();

    // Build the full authorization URL (includes PKCE challenge)
    wxString authUrl = m_session.BuildAuthorizationUrl();

    // Listen for the loopback result
    Bind( EVT_OAUTH_LOOPBACK_RESULT, &AUTH::onLoopbackResult, this );

    // Open the browser
    wxLaunchDefaultBrowser( authUrl );
}


void AUTH::CancelLogin()
{
    Unbind( EVT_OAUTH_LOOPBACK_RESULT, &AUTH::onLoopbackResult, this );
    m_loopbackServer.reset();
    m_eventSink = nullptr;
}


bool AUTH::LoadSavedTokens()
{
    auto tokens = m_tokenStore.LoadTokens( PROVIDER_ID, ACCOUNT_ID );

    if( !tokens.has_value() )
        return false;

    m_tokens = tokens.value();

    if( m_tokens.access_token.IsEmpty() )
        return false;

    fetchUserProfile();
    return true;
}


bool AUTH::RefreshToken()
{
    if( m_tokens.refresh_token.IsEmpty() )
        return false;

    KICAD_CURL_EASY curl;

    std::string url = m_apiUrl + "/auth/token";
    curl.SetURL( url );
    curl.SetUserAgent( "KiCad-Copper/1.0" );
    curl.SetHeader( "Content-Type", "application/json" );

    nlohmann::json body;
    body["grant_type"] = "refresh_token";
    body["refresh_token"] = std::string( m_tokens.refresh_token.ToUTF8() );
    body["client_id"] = std::string( CLIENT_ID.ToUTF8() );
    curl.SetPostFields( body.dump() );

    int rc = curl.Perform();

    if( rc != 0 )
    {
        wxLogWarning( "Copper: token refresh request failed: %s",
                       curl.GetErrorText( rc ) );
        return false;
    }

    int status = curl.GetResponseStatusCode();

    if( status != 200 )
    {
        wxLogWarning( "Copper: token refresh returned status %d", status );
        return false;
    }

    try
    {
        nlohmann::json resp = nlohmann::json::parse( curl.GetBuffer() );

        m_tokens.access_token = wxString::FromUTF8( resp.value( "access_token", "" ) );

        if( resp.contains( "refresh_token" ) )
            m_tokens.refresh_token = wxString::FromUTF8( resp.value( "refresh_token", "" ) );

        int expiresIn = resp.value( "expires_in", 3600 );
        auto now = std::chrono::system_clock::now();
        m_tokens.expires_at = std::chrono::duration_cast<std::chrono::seconds>(
                                  now.time_since_epoch() ).count() + expiresIn;

        saveTokens();
        return !m_tokens.access_token.IsEmpty();
    }
    catch( const nlohmann::json::exception& e )
    {
        wxLogWarning( "Copper: failed to parse refresh response: %s", e.what() );
        return false;
    }
}


void AUTH::Logout()
{
    m_tokens = OAUTH_TOKEN_SET();
    m_userEmail.clear();
    m_tokenStore.DeleteTokens( PROVIDER_ID, ACCOUNT_ID );
}


bool AUTH::IsAuthenticated() const
{
    if( m_tokens.access_token.IsEmpty() )
        return false;

    // If expires_at is 0, we haven't checked yet -- assume valid
    if( m_tokens.expires_at == 0 )
        return true;

    auto now = std::chrono::system_clock::now();
    int64_t nowSecs = std::chrono::duration_cast<std::chrono::seconds>(
                          now.time_since_epoch() ).count();

    return nowSecs < m_tokens.expires_at;
}


std::string AUTH::GetAccessToken() const
{
    return std::string( m_tokens.access_token.ToUTF8() );
}


std::string AUTH::GetRefreshToken() const
{
    return std::string( m_tokens.refresh_token.ToUTF8() );
}


void AUTH::onLoopbackResult( wxCommandEvent& aEvent )
{
    Unbind( EVT_OAUTH_LOOPBACK_RESULT, &AUTH::onLoopbackResult, this );

    bool success = aEvent.GetInt() != 0;
    wxString code = aEvent.GetString();

    m_loopbackServer.reset();

    if( success && !code.IsEmpty() && exchangeCodeForTokens( std::string( code.ToUTF8() ) ) )
    {
        fetchUserProfile();
        saveTokens();

        if( m_eventSink )
        {
            wxCommandEvent* evt = new wxCommandEvent( COPPER_EVT_AUTH_SUCCESS );
            evt->SetString( wxString::FromUTF8( m_userEmail ) );
            wxQueueEvent( m_eventSink, evt );
        }
    }
    else
    {
        if( m_eventSink )
        {
            wxCommandEvent* evt = new wxCommandEvent( COPPER_EVT_AUTH_FAILURE );
            evt->SetString( success ? wxS( "Token exchange failed" )
                                    : wxS( "Authorization failed or timed out" ) );
            wxQueueEvent( m_eventSink, evt );
        }
    }

    m_eventSink = nullptr;
}


bool AUTH::exchangeCodeForTokens( const std::string& aCode )
{
    KICAD_CURL_EASY curl;

    std::string url = m_apiUrl + "/auth/token";
    curl.SetURL( url );
    curl.SetUserAgent( "KiCad-Copper/1.0" );
    curl.SetHeader( "Content-Type", "application/json" );

    nlohmann::json body;
    body["grant_type"] = "authorization_code";
    body["code"] = aCode;
    body["redirect_uri"] = std::string( m_session.redirect_uri.ToUTF8() );
    body["client_id"] = std::string( CLIENT_ID.ToUTF8() );
    body["code_verifier"] = std::string( m_session.code_verifier.ToUTF8() );
    curl.SetPostFields( body.dump() );

    int rc = curl.Perform();

    if( rc != 0 )
    {
        wxLogWarning( "Copper: token exchange request failed: %s",
                       curl.GetErrorText( rc ) );
        return false;
    }

    int status = curl.GetResponseStatusCode();

    if( status != 200 )
    {
        wxLogWarning( "Copper: token exchange returned status %d", status );
        return false;
    }

    try
    {
        nlohmann::json resp = nlohmann::json::parse( curl.GetBuffer() );

        m_tokens.access_token = wxString::FromUTF8( resp.value( "access_token", "" ) );
        m_tokens.refresh_token = wxString::FromUTF8( resp.value( "refresh_token", "" ) );

        if( resp.contains( "id_token" ) )
            m_tokens.id_token = wxString::FromUTF8( resp.value( "id_token", "" ) );

        int expiresIn = resp.value( "expires_in", 3600 );
        auto now = std::chrono::system_clock::now();
        m_tokens.expires_at = std::chrono::duration_cast<std::chrono::seconds>(
                                  now.time_since_epoch() ).count() + expiresIn;

        return !m_tokens.access_token.IsEmpty();
    }
    catch( const nlohmann::json::exception& e )
    {
        wxLogWarning( "Copper: failed to parse token response: %s", e.what() );
        return false;
    }
}


void AUTH::saveTokens()
{
    m_tokenStore.StoreTokens( PROVIDER_ID, ACCOUNT_ID, m_tokens );
}


void AUTH::fetchUserProfile()
{
    if( m_tokens.access_token.IsEmpty() )
        return;

    KICAD_CURL_EASY curl;

    std::string url = m_apiUrl + "/auth/me";
    curl.SetURL( url );
    curl.SetUserAgent( "KiCad-Copper/1.0" );
    curl.SetHeader( "Authorization",
                    "Bearer " + std::string( m_tokens.access_token.ToUTF8() ) );

    int rc = curl.Perform();

    if( rc != 0 )
        return;

    int status = curl.GetResponseStatusCode();

    if( status != 200 )
        return;

    try
    {
        nlohmann::json resp = nlohmann::json::parse( curl.GetBuffer() );
        m_userEmail = resp.value( "email", "" );
    }
    catch( const nlohmann::json::exception& )
    {
        // Silently ignore parse errors for profile fetch
    }
}

}  // namespace COPPER
