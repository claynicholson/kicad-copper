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

#ifndef KICAD_HTTP_BRIDGE_H
#define KICAD_HTTP_BRIDGE_H

#include <memory>
#include <thread>
#include <atomic>
#include <wx/socket.h>
#include <wx/event.h>

class SCH_EDIT_FRAME;
class API_HANDLER_SCH;


/**
 * HTTP_BRIDGE — Lightweight HTTP server exposing the schematic API to third-party tools.
 *
 * Runs a simple HTTP server on localhost (default port 9742) that accepts JSON
 * requests and delegates to the in-process API_HANDLER_SCH.
 *
 * Endpoints:
 *   GET  /api/schematic/info              → GetSchematicInfo
 *   GET  /api/schematic/components        → GetComponents
 *   GET  /api/schematic/nets              → GetNets
 *   GET  /api/schematic/power-rails       → GetPowerRails
 *   GET  /api/schematic/wires             → GetWires
 *   GET  /api/schematic/labels            → GetLabels
 *   GET  /api/schematic/bounding-box      → GetBoundingBox
 *   GET  /api/schematic/next-free-position → GetNextFreePosition
 *   POST /api/schematic/place-symbol      → PlaceSymbol
 *   POST /api/schematic/draw-wire         → DrawWire
 *   POST /api/schematic/add-label         → AddLabel
 *   POST /api/schematic/add-junction      → AddJunction
 *   POST /api/schematic/refresh           → RefreshView
 *   POST /api/schematic/undo              → Undo
 *   POST /api/schematic/redo              → Redo
 */
class HTTP_BRIDGE : public wxEvtHandler
{
public:
    HTTP_BRIDGE( SCH_EDIT_FRAME* aFrame, int aPort = 9742 );
    ~HTTP_BRIDGE() override;

    /// Start the HTTP server
    bool Start();

    /// Stop the HTTP server
    void Stop();

    /// Check if the server is running
    bool IsRunning() const { return m_running.load(); }

    /// Get the port number
    int GetPort() const { return m_port; }

private:
    void onSocketEvent( wxSocketEvent& aEvent );
    void handleClient( wxSocketBase* aClient );

    /// Parse an HTTP request and return the response
    std::string processRequest( const std::string& aMethod, const std::string& aPath,
                                const std::string& aBody );

    /// Build standard HTTP response
    std::string httpResponse( int aStatusCode, const std::string& aBody,
                              const std::string& aContentType = "application/json" );

    /// Build CORS headers
    std::string corsHeaders() const;

    SCH_EDIT_FRAME*                   m_frame;
    int                               m_port;
    std::atomic<bool>                 m_running{ false };
    std::unique_ptr<wxSocketServer>   m_server;
};


#endif // KICAD_HTTP_BRIDGE_H
