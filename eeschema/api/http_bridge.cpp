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

#include <api/http_bridge.h>
#include <api/api_handler_sch.h>
#include <sch_edit_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_line.h>
#include <sch_label.h>
#include <sch_junction.h>
#include <sch_no_connect.h>
#include <sch_sheet.h>
#include <sch_field.h>
#include <sch_commit.h>
#include <sch_pin.h>
#include <connection_graph.h>
#include <lib_symbol.h>

// undo/redo + ToolManager glue (referenced via m_frame->GetToolManager()->RunAction)
#include <tool/tool_manager.h>
#include <tool/actions.h>

#include <nlohmann/json.hpp>

#include <wx/log.h>

using json = nlohmann::json;


HTTP_BRIDGE::HTTP_BRIDGE( SCH_EDIT_FRAME* aFrame, int aPort ) :
        wxEvtHandler(),
        m_frame( aFrame ),
        m_port( aPort )
{
}


HTTP_BRIDGE::~HTTP_BRIDGE()
{
    Stop();
}


bool HTTP_BRIDGE::Start()
{
    if( m_running.load() )
        return true;

    wxIPV4address addr;
    addr.Service( m_port );
    addr.Hostname( wxT( "127.0.0.1" ) );

    m_server = std::make_unique<wxSocketServer>( addr, wxSOCKET_REUSEADDR );

    if( !m_server->IsOk() )
    {
        wxLogError( wxT( "HTTP Bridge: Failed to start on port %d" ), m_port );
        m_server.reset();
        return false;
    }

    m_server->SetEventHandler( *this );
    m_server->SetNotify( wxSOCKET_CONNECTION_FLAG );
    m_server->Notify( true );

    Bind( wxEVT_SOCKET, &HTTP_BRIDGE::onSocketEvent, this );

    m_running.store( true );
    wxLogMessage( wxT( "HTTP Bridge: Listening on http://127.0.0.1:%d" ), m_port );
    return true;
}


void HTTP_BRIDGE::Stop()
{
    if( !m_running.load() )
        return;

    m_running.store( false );

    if( m_server )
    {
        m_server->Notify( false );
        m_server.reset();
    }

    wxLogMessage( wxT( "HTTP Bridge: Stopped" ) );
}


void HTTP_BRIDGE::onSocketEvent( wxSocketEvent& aEvent )
{
    if( aEvent.GetSocketEvent() != wxSOCKET_CONNECTION )
        return;

    wxSocketBase* client = m_server->Accept( false );

    if( client )
    {
        client->SetFlags( wxSOCKET_WAITALL | wxSOCKET_BLOCK );
        handleClient( client );
        client->Destroy();
    }
}


void HTTP_BRIDGE::handleClient( wxSocketBase* aClient )
{
    // Read the full HTTP request
    char buf[8192];
    memset( buf, 0, sizeof( buf ) );

    aClient->Read( buf, sizeof( buf ) - 1 );

    if( aClient->Error() )
        return;

    std::string request( buf, aClient->LastCount() );

    // Parse HTTP request line
    size_t lineEnd = request.find( "\r\n" );

    if( lineEnd == std::string::npos )
        return;

    std::string requestLine = request.substr( 0, lineEnd );

    // Parse method and path
    size_t sp1 = requestLine.find( ' ' );
    size_t sp2 = requestLine.find( ' ', sp1 + 1 );

    if( sp1 == std::string::npos || sp2 == std::string::npos )
        return;

    std::string method = requestLine.substr( 0, sp1 );
    std::string path = requestLine.substr( sp1 + 1, sp2 - sp1 - 1 );

    // Extract body (after double CRLF)
    std::string body;
    size_t bodyStart = request.find( "\r\n\r\n" );

    if( bodyStart != std::string::npos )
        body = request.substr( bodyStart + 4 );

    // Handle CORS preflight
    if( method == "OPTIONS" )
    {
        std::string response = "HTTP/1.1 204 No Content\r\n"
                               + corsHeaders()
                               + "Content-Length: 0\r\n\r\n";
        aClient->Write( response.c_str(), response.size() );
        return;
    }

    // Process the request
    std::string responseBody = processRequest( method, path, body );
    std::string response = httpResponse( 200, responseBody );
    aClient->Write( response.c_str(), response.size() );
}


std::string HTTP_BRIDGE::processRequest( const std::string& aMethod,
                                          const std::string& aPath,
                                          const std::string& aBody )
{
    if( !m_frame || !m_frame->Schematic().IsValid() )
        return json( { { "error", "No schematic open" } } ).dump();

    SCHEMATIC& schematic = m_frame->Schematic();
    SCH_SCREEN* screen = schematic.RootScreen();

    if( !screen )
        return json( { { "error", "No screen available" } } ).dump();

    // ── GET endpoints ──────────────────────────────────────────────

    if( aMethod == "GET" && aPath == "/api/schematic/info" )
    {
        json result;
        // BuildSheetList() was renamed; the new name returns the same SCH_SHEET_LIST.
        result["sheet_count"] = (int)schematic.BuildUnorderedSheetList().size();
        // PAGE_INFO::GetType() now returns the PAGE_SIZE_TYPE enum; use the
        // string accessor that KiCad added for display purposes.
        result["paper_size"] = screen->GetPageSettings().GetTypeAsString().ToStdString();

        const TITLE_BLOCK& tb = screen->GetTitleBlock();
        result["title_block"] = {
            { "title", tb.GetTitle().ToStdString() },
            { "date", tb.GetDate().ToStdString() },
            { "revision", tb.GetRevision().ToStdString() },
            { "company", tb.GetCompany().ToStdString() }
        };

        return result.dump();
    }

    if( aMethod == "GET" && aPath == "/api/schematic/components" )
    {
        json components = json::array();

        for( SCH_ITEM* item : screen->Items() )
        {
            if( item->Type() == SCH_SYMBOL_T )
            {
                SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
                json comp;
                comp["id"] = sym->m_Uuid.AsStdString();
                comp["reference"] = sym->GetRef( &schematic.CurrentSheet() ).ToStdString();
                comp["value"] = sym->GetField( FIELD_T::VALUE )->GetText().ToStdString();
                // UTF8 implicitly converts to const std::string&; no ToStdString().
                comp["lib_id"] = (std::string) sym->GetLibId().Format();
                comp["position"] = {
                    { "x", sym->GetPosition().x },
                    { "y", sym->GetPosition().y }
                };
                components.push_back( comp );
            }
        }

        return json( { { "components", components } } ).dump();
    }

    if( aMethod == "GET" && aPath == "/api/schematic/nets" )
    {
        json nets = json::array();
        CONNECTION_GRAPH* connGraph = schematic.ConnectionGraph();

        if( connGraph )
        {
            auto netMap = connGraph->GetNetMap();

            for( const auto& [netKey, subgraphs] : netMap )
            {
                json net;
                net["name"] = netKey.Name.ToStdString();
                net["connected_pins"] = json::array();

                for( const auto* subgraph : subgraphs )
                {
                    for( SCH_ITEM* connItem : subgraph->GetItems() )
                    {
                        if( connItem->Type() == SCH_PIN_T )
                        {
                            SCH_PIN* pin = static_cast<SCH_PIN*>( connItem );
                            // GetParentSymbol() returns SYMBOL* base class now; only
                            // SCH_SYMBOL (placed in the schematic) is interesting here.
                            SCH_SYMBOL* parentSym = dynamic_cast<SCH_SYMBOL*>( pin->GetParentSymbol() );

                            if( parentSym )
                            {
                                std::string pinRef =
                                    parentSym->GetRef( &schematic.CurrentSheet() ).ToStdString()
                                    + ":" + pin->GetNumber().ToStdString();
                                net["connected_pins"].push_back( pinRef );
                            }
                        }
                    }
                }

                nets.push_back( net );
            }
        }

        return json( { { "nets", nets } } ).dump();
    }

    if( aMethod == "GET" && aPath == "/api/schematic/power-rails" )
    {
        json rails = json::array();
        CONNECTION_GRAPH* connGraph = schematic.ConnectionGraph();

        if( connGraph )
        {
            auto netMap = connGraph->GetNetMap();

            for( const auto& [netKey, subgraphs] : netMap )
            {
                for( const auto* subgraph : subgraphs )
                {
                    // Power-net detection moved off SCH_CONNECTION onto SCH_PIN.
                    if( const SCH_ITEM* driver = subgraph->GetDriver();
                        driver && driver->Type() == SCH_PIN_T )
                    {
                        const SCH_PIN* drvPin = static_cast<const SCH_PIN*>( driver );

                        if( drvPin->IsPower() )
                        {
                            rails.push_back( netKey.Name.ToStdString() );
                            break;
                        }
                    }
                }
            }
        }

        return json( { { "power_rails", rails } } ).dump();
    }

    if( aMethod == "GET" && aPath == "/api/schematic/wires" )
    {
        json wires = json::array();

        for( SCH_ITEM* item : screen->Items() )
        {
            if( item->Type() == SCH_LINE_T )
            {
                SCH_LINE* line = static_cast<SCH_LINE*>( item );

                if( line->GetLayer() == LAYER_WIRE )
                {
                    wires.push_back( {
                        { "id", line->m_Uuid.AsStdString() },
                        { "start", { { "x", line->GetStartPoint().x },
                                     { "y", line->GetStartPoint().y } } },
                        { "end", { { "x", line->GetEndPoint().x },
                                   { "y", line->GetEndPoint().y } } }
                    } );
                }
            }
        }

        return json( { { "wires", wires } } ).dump();
    }

    if( aMethod == "GET" && aPath == "/api/schematic/labels" )
    {
        json localLabels = json::array();
        json globalLabels = json::array();
        json hierLabels = json::array();

        for( SCH_ITEM* item : screen->Items() )
        {
            if( item->Type() == SCH_LABEL_T )
            {
                SCH_LABEL* label = static_cast<SCH_LABEL*>( item );
                localLabels.push_back( {
                    { "name", label->GetText().ToStdString() },
                    { "position", { { "x", label->GetPosition().x },
                                    { "y", label->GetPosition().y } } }
                } );
            }
            else if( item->Type() == SCH_GLOBAL_LABEL_T )
            {
                SCH_GLOBALLABEL* label = static_cast<SCH_GLOBALLABEL*>( item );
                globalLabels.push_back( {
                    { "name", label->GetText().ToStdString() },
                    { "position", { { "x", label->GetPosition().x },
                                    { "y", label->GetPosition().y } } }
                } );
            }
            else if( item->Type() == SCH_HIER_LABEL_T )
            {
                SCH_HIERLABEL* label = static_cast<SCH_HIERLABEL*>( item );
                hierLabels.push_back( {
                    { "name", label->GetText().ToStdString() },
                    { "position", { { "x", label->GetPosition().x },
                                    { "y", label->GetPosition().y } } }
                } );
            }
        }

        return json( {
            { "local_labels", localLabels },
            { "global_labels", globalLabels },
            { "hierarchical_labels", hierLabels }
        } ).dump();
    }

    if( aMethod == "GET" && aPath == "/api/schematic/bounding-box" )
    {
        BOX2I bbox;
        bool init = false;

        for( SCH_ITEM* item : screen->Items() )
        {
            if( !init )
            {
                bbox = item->GetBoundingBox();
                init = true;
            }
            else
            {
                bbox.Merge( item->GetBoundingBox() );
            }
        }

        return json( {
            { "min", { { "x", bbox.GetOrigin().x }, { "y", bbox.GetOrigin().y } } },
            { "max", { { "x", bbox.GetEnd().x }, { "y", bbox.GetEnd().y } } }
        } ).dump();
    }

    if( aMethod == "GET" && aPath == "/api/schematic/next-free-position" )
    {
        BOX2I bbox;
        bool init = false;
        int gridNm = 2540000;  // 2.54mm

        for( SCH_ITEM* item : screen->Items() )
        {
            if( !init )
            {
                bbox = item->GetBoundingBox();
                init = true;
            }
            else
            {
                bbox.Merge( item->GetBoundingBox() );
            }
        }

        int freeX, freeY;

        if( init )
        {
            freeX = ( ( bbox.GetRight() + gridNm * 10 ) / gridNm ) * gridNm;
            freeY = ( bbox.GetOrigin().y / gridNm ) * gridNm;
        }
        else
        {
            freeX = gridNm * 40;
            freeY = gridNm * 30;
        }

        return json( { { "position", { { "x", freeX }, { "y", freeY } } } } ).dump();
    }

    // ── POST endpoints ─────────────────────────────────────────────

    if( aMethod == "POST" && aPath == "/api/schematic/place-symbol" )
    {
        try
        {
            json req = json::parse( aBody );
            std::string libIdStr = req.value( "lib_id", "" );
            std::string ref = req.value( "reference", "" );
            std::string val = req.value( "value", "" );
            int posX = req.value( "x", 0 );
            int posY = req.value( "y", 0 );

            LIB_ID libId;
            libId.Parse( wxString::FromUTF8( libIdStr ) );

            LIB_SYMBOL* libSym = m_frame->GetLibSymbol( libId );

            if( !libSym )
                return json( { { "error", "Symbol not found: " + libIdStr } } ).dump();

            SCH_COMMIT commit( m_frame );
            SCH_SYMBOL* symbol = new SCH_SYMBOL( *libSym, libId,
                                                  &schematic.CurrentSheet(), 0 );
            symbol->SetPosition( VECTOR2I( posX, posY ) );

            if( !ref.empty() )
                symbol->SetRef( &schematic.CurrentSheet(), wxString::FromUTF8( ref ) );

            if( !val.empty() )
                symbol->GetField( FIELD_T::VALUE )->SetText( wxString::FromUTF8( val ) );

            commit.Add( symbol, screen );
            commit.Push( _( "HTTP Bridge: Place symbol" ) );

            m_frame->GetCanvas()->Refresh();
            m_frame->RecalculateConnections( nullptr, NO_CLEANUP );

            return json( { { "status", "ok" }, { "id", symbol->m_Uuid.AsStdString() } } ).dump();
        }
        catch( const std::exception& e )
        {
            return json( { { "error", e.what() } } ).dump();
        }
    }

    if( aMethod == "POST" && aPath == "/api/schematic/draw-wire" )
    {
        try
        {
            json req = json::parse( aBody );
            int startX = req.value( "start_x", 0 );
            int startY = req.value( "start_y", 0 );
            int endX = req.value( "end_x", 0 );
            int endY = req.value( "end_y", 0 );

            SCH_COMMIT commit( m_frame );
            SCH_LINE* wire = new SCH_LINE( VECTOR2I( startX, startY ), LAYER_WIRE );
            wire->SetEndPoint( VECTOR2I( endX, endY ) );
            commit.Add( wire, screen );
            commit.Push( _( "HTTP Bridge: Draw wire" ) );

            m_frame->GetCanvas()->Refresh();
            m_frame->RecalculateConnections( nullptr, NO_CLEANUP );

            return json( { { "status", "ok" }, { "id", wire->m_Uuid.AsStdString() } } ).dump();
        }
        catch( const std::exception& e )
        {
            return json( { { "error", e.what() } } ).dump();
        }
    }

    if( aMethod == "POST" && aPath == "/api/schematic/add-label" )
    {
        try
        {
            json req = json::parse( aBody );
            std::string name = req.value( "name", "" );
            int posX = req.value( "x", 0 );
            int posY = req.value( "y", 0 );
            std::string type = req.value( "type", "local" );

            SCH_COMMIT commit( m_frame );
            SCH_LABEL_BASE* label = nullptr;

            if( type == "global" )
                label = new SCH_GLOBALLABEL( VECTOR2I( posX, posY ), wxString::FromUTF8( name ) );
            else if( type == "hierarchical" )
                label = new SCH_HIERLABEL( VECTOR2I( posX, posY ), wxString::FromUTF8( name ) );
            else
                label = new SCH_LABEL( VECTOR2I( posX, posY ), wxString::FromUTF8( name ) );

            commit.Add( label, screen );
            commit.Push( _( "HTTP Bridge: Add label" ) );

            m_frame->GetCanvas()->Refresh();

            return json( { { "status", "ok" }, { "id", label->m_Uuid.AsStdString() } } ).dump();
        }
        catch( const std::exception& e )
        {
            return json( { { "error", e.what() } } ).dump();
        }
    }

    if( aMethod == "POST" && aPath == "/api/schematic/add-junction" )
    {
        try
        {
            json req = json::parse( aBody );
            int posX = req.value( "x", 0 );
            int posY = req.value( "y", 0 );

            SCH_COMMIT commit( m_frame );
            SCH_JUNCTION* junction = new SCH_JUNCTION( VECTOR2I( posX, posY ) );
            commit.Add( junction, screen );
            commit.Push( _( "HTTP Bridge: Add junction" ) );

            m_frame->GetCanvas()->Refresh();

            return json( { { "status", "ok" }, { "id", junction->m_Uuid.AsStdString() } } ).dump();
        }
        catch( const std::exception& e )
        {
            return json( { { "error", e.what() } } ).dump();
        }
    }

    if( aMethod == "POST" && aPath == "/api/schematic/refresh" )
    {
        m_frame->GetCanvas()->Refresh();
        m_frame->RefreshNetNavigator();
        m_frame->RecalculateConnections( nullptr, NO_CLEANUP );
        return json( { { "status", "ok" } } ).dump();
    }

    if( aMethod == "POST" && aPath == "/api/schematic/undo" )
    {
        m_frame->GetToolManager()->RunAction( ACTIONS::undo );
        return json( { { "status", "ok" } } ).dump();
    }

    if( aMethod == "POST" && aPath == "/api/schematic/redo" )
    {
        m_frame->GetToolManager()->RunAction( ACTIONS::redo );
        return json( { { "status", "ok" } } ).dump();
    }

    // Unknown endpoint
    return json( { { "error", "Unknown endpoint: " + aMethod + " " + aPath } } ).dump();
}


std::string HTTP_BRIDGE::httpResponse( int aStatusCode, const std::string& aBody,
                                        const std::string& aContentType )
{
    std::string statusText;

    switch( aStatusCode )
    {
    case 200: statusText = "OK"; break;
    case 204: statusText = "No Content"; break;
    case 400: statusText = "Bad Request"; break;
    case 404: statusText = "Not Found"; break;
    case 500: statusText = "Internal Server Error"; break;
    default: statusText = "Unknown"; break;
    }

    return "HTTP/1.1 " + std::to_string( aStatusCode ) + " " + statusText + "\r\n"
           + corsHeaders()
           + "Content-Type: " + aContentType + "\r\n"
           + "Content-Length: " + std::to_string( aBody.size() ) + "\r\n"
           + "Connection: close\r\n"
           + "\r\n"
           + aBody;
}


std::string HTTP_BRIDGE::corsHeaders() const
{
    return "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           "Access-Control-Allow-Headers: Content-Type\r\n";
}
