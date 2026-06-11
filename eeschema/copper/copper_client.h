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

#ifndef KICAD_COPPER_CLIENT_H
#define KICAD_COPPER_CLIENT_H

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <copper/copper_types.h>

namespace COPPER
{

class AUTH;

/**
 * Debug mode (set env COPPER_DEBUG=1): traces every request, response,
 * SSE event and transport error to <temp>/copper_debug.log.
 */
bool DebugEnabled();

/// Full path of the debug log file (valid whether or not debug is enabled).
std::string DebugLogPath();

/// Append one timestamped line to the debug log. No-op unless DebugEnabled().
void DebugLog( const std::string& aLine );

/**
 * HTTP client for communicating with the Copper cloud API.
 * All heavy I/O runs on a background thread; results are posted
 * back to the UI thread via wxQueueEvent.
 */
class CLIENT
{
public:
    CLIENT( AUTH* aAuth );
    ~CLIENT();

    /// Send a chat message (non-streaming JSON response)
    void Chat( const std::string& aPrompt, const SchematicContext& aCtx,
               ResponseCallback aOnResponse, ErrorCallback aOnError );

    /// Send a recommend request (non-streaming JSON response)
    void Recommend( const std::string& aPrompt, const SchematicContext& aCtx,
                    ResponseCallback aOnResponse, ErrorCallback aOnError );

    /// Send a generate request (SSE streaming response)
    void Generate( const std::string& aPrompt, const SchematicContext& aCtx,
                   SSECallback aOnEvent, ErrorCallback aOnError );

    /// Send a plan request (non-streaming JSON response)
    void Plan( const std::string& aPrompt, const SchematicContext& aCtx,
               ResponseCallback aOnResponse, ErrorCallback aOnError );

    /// Cancel any in-progress request
    void Cancel();

    /// Check if a request is in progress
    bool IsBusy() const { return m_busy.load(); }

private:
    /// Execute a non-streaming POST request on a background thread
    void doPost( const std::string& aEndpoint, const nlohmann::json& aBody,
                 ResponseCallback aOnResponse, ErrorCallback aOnError );

    /// Execute a streaming POST request on a background thread
    void doStreamPost( const std::string& aEndpoint, const nlohmann::json& aBody,
                       SSECallback aOnEvent, ErrorCallback aOnError );

    /// Build the common request body
    nlohmann::json buildRequestBody( const std::string& aPrompt,
                                     const SchematicContext& aCtx ) const;

    AUTH*                        m_auth;
    std::unique_ptr<std::thread> m_workerThread;
    std::atomic<bool>            m_busy{ false };
    std::atomic<bool>            m_cancelled{ false };
};

}  // namespace COPPER

#endif // KICAD_COPPER_CLIENT_H
