/*
 * This program source code file is part of KiCad Copper, a free EDA CAD application.
 *
 * Copyright The KiCad Copper Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

// kicad_curl headers must come before wx headers on Windows
#include <kicad_curl/kicad_curl.h>
#include <kicad_curl/kicad_curl_easy.h>

#include "copper_panel.h"

#include <sch_edit_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <sch_symbol.h>
#include <sch_line.h>
#include <sch_label.h>
#include <sch_junction.h>
#include <sch_sheet.h>
#include <sch_commit.h>
#include <lib_symbol.h>
#include <layer_ids.h>
#include <tool/tool_manager.h>

#include <nlohmann/json.hpp>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/richtext/richtextctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/dir.h>

#include <fstream>
#include <sstream>


// ---------------------------------------------------------------------------
// Colors matching Copper theme (styles.py)
// ---------------------------------------------------------------------------
static const wxColour BG_PRIMARY( 22, 22, 26 );
static const wxColour BG_SECONDARY( 32, 32, 38 );
static const wxColour BG_INPUT( 38, 38, 46 );
static const wxColour TEXT_PRIMARY( 230, 230, 235 );
static const wxColour TEXT_SECONDARY( 140, 140, 150 );
static const wxColour ACCENT( 217, 158, 64 );
static const wxColour USER_BUBBLE( 45, 55, 75 );
static const wxColour AI_BUBBLE( 38, 38, 45 );
static const wxColour ERROR_COLOR( 244, 67, 54 );
static const wxColour SUCCESS_COLOR( 76, 175, 80 );
static const wxColour BORDER_COLOR( 55, 55, 65 );

// Intent detection keyword sets
static const std::vector<wxString> GENERATE_KEYWORDS = {
    wxT("design"), wxT("build"), wxT("create"), wxT("implement"),
    wxT("make"), wxT("generate"), wxT("add")
};
static const std::vector<wxString> RECOMMEND_KEYWORDS = {
    wxT("find"), wxT("recommend"), wxT("suggest"), wxT("search"),
    wxT("what part"), wxT("which component")
};
static const std::vector<wxString> EXPLAIN_KEYWORDS = {
    wxT("explain"), wxT("what does"), wxT("how does"), wxT("describe"),
    wxT("tell me about")
};
static const std::vector<wxString> VERIFY_KEYWORDS = {
    wxT("check"), wxT("verify"), wxT("sanity"), wxT("validate"), wxT("review")
};


COPPER_PANEL::COPPER_PANEL( SCH_EDIT_FRAME* aParent ) :
        wxPanel( aParent ),
        m_frame( aParent ),
        m_conversation( nullptr ),
        m_input( nullptr ),
        m_sendButton( nullptr ),
        m_statusText( nullptr ),
        m_cloudUrl( "http://localhost:8000" ),
        m_busy( false )
{
    SetBackgroundColour( BG_PRIMARY );
    buildUI();
    loadConfig();
}


COPPER_PANEL::~COPPER_PANEL()
{
    if( m_workerThread.joinable() )
        m_workerThread.detach();
}


void COPPER_PANEL::Activate()
{
    if( m_apiKey.empty() )
        promptForApiKey();
}


void COPPER_PANEL::buildUI()
{
    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    // Header
    wxStaticText* header = new wxStaticText( this, wxID_ANY, wxT( "Copper AI" ) );
    header->SetForegroundColour( ACCENT );
    wxFont headerFont = header->GetFont();
    headerFont.SetPointSize( 14 );
    headerFont.SetWeight( wxFONTWEIGHT_BOLD );
    header->SetFont( headerFont );
    mainSizer->Add( header, 0, wxALL | wxALIGN_LEFT, 8 );

    // Vendor selector
    wxBoxSizer* vendorSizer = new wxBoxSizer( wxHORIZONTAL );
    wxStaticText* vendorLabel = new wxStaticText( this, wxID_ANY, wxT( "Vendor:" ) );
    vendorLabel->SetForegroundColour( TEXT_SECONDARY );
    vendorSizer->Add( vendorLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4 );

    wxArrayString vendorChoices;
    vendorChoices.Add( wxT( "All Vendors" ) );
    vendorChoices.Add( wxT( "LCSC / JLCPCB" ) );
    vendorChoices.Add( wxT( "Mouser" ) );
    vendorChoices.Add( wxT( "DigiKey" ) );
    m_vendorChoice = new wxChoice( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, vendorChoices );
    m_vendorChoice->SetSelection( 0 );
    vendorSizer->Add( m_vendorChoice, 1, wxALIGN_CENTER_VERTICAL );

    mainSizer->Add( vendorSizer, 0, wxEXPAND | wxLEFT | wxRIGHT, 8 );

    // Separator
    wxPanel* sep = new wxPanel( this, wxID_ANY, wxDefaultPosition, wxSize( -1, 1 ) );
    sep->SetBackgroundColour( BORDER_COLOR );
    mainSizer->Add( sep, 0, wxEXPAND | wxALL, 8 );

    // Conversation area
    m_conversation = new wxRichTextCtrl( this, wxID_ANY, wxEmptyString,
                                          wxDefaultPosition, wxDefaultSize,
                                          wxRE_MULTILINE | wxRE_READONLY | wxBORDER_NONE );
    m_conversation->SetBackgroundColour( BG_PRIMARY );
    m_conversation->SetForegroundColour( TEXT_PRIMARY );
    mainSizer->Add( m_conversation, 1, wxEXPAND | wxALL, 4 );

    // ASCII art banner
    m_conversation->SetDefaultStyle( wxRichTextAttr() );
    m_conversation->BeginTextColour( ACCENT );
    wxFont monoFont( 8, wxFONTFAMILY_TELETYPE, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_BOLD );
    m_conversation->BeginFont( monoFont );
    m_conversation->WriteText(
        wxT( "  ____                            \n" )
        wxT( " / ___|___  _ __  _ __   ___ _ __ \n" )
        wxT( "| |   / _ \\| '_ \\| '_ \\ / _ \\ '__|\n" )
        wxT( "| |__| (_) | |_) | |_) |  __/ |   \n" )
        wxT( " \\____\\___/| .__/| .__/ \\___|_|   \n" )
        wxT( "           |_|   |_|               \n" ) );
    m_conversation->EndFont();
    m_conversation->EndTextColour();

    // Welcome message
    m_conversation->BeginTextColour( TEXT_SECONDARY );
    m_conversation->WriteText( wxT( "\nAsk me to design circuits, explain schematics, "
                                     "find components, or verify your designs.\n\n"
                                     "Try: \"Design a USB-powered RP2040 dev board\"\n" ) );
    m_conversation->EndTextColour();

    // Input area
    wxBoxSizer* inputSizer = new wxBoxSizer( wxHORIZONTAL );

    m_input = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                               wxSize( -1, 32 ), wxTE_PROCESS_ENTER | wxBORDER_SIMPLE );
    m_input->SetBackgroundColour( BG_INPUT );
    m_input->SetForegroundColour( TEXT_PRIMARY );
    m_input->SetHint( wxT( "Ask Copper anything..." ) );
    inputSizer->Add( m_input, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 4 );

    m_sendButton = new wxButton( this, wxID_ANY, wxT( "Send" ), wxDefaultPosition,
                                  wxSize( 60, 32 ) );
    m_sendButton->SetBackgroundColour( ACCENT );
    m_sendButton->SetForegroundColour( wxColour( 20, 20, 20 ) );
    inputSizer->Add( m_sendButton, 0, wxALIGN_CENTER_VERTICAL );

    mainSizer->Add( inputSizer, 0, wxEXPAND | wxALL, 8 );

    // Status bar
    m_statusText = new wxStaticText( this, wxID_ANY, wxT( "Ready" ) );
    m_statusText->SetForegroundColour( TEXT_SECONDARY );
    wxFont statusFont = m_statusText->GetFont();
    statusFont.SetPointSize( 8 );
    m_statusText->SetFont( statusFont );
    mainSizer->Add( m_statusText, 0, wxLEFT | wxRIGHT | wxBOTTOM, 8 );

    SetSizer( mainSizer );

    // Bind events
    m_sendButton->Bind( wxEVT_BUTTON, &COPPER_PANEL::onSendMessage, this );
    m_input->Bind( wxEVT_TEXT_ENTER, &COPPER_PANEL::onSendMessage, this );
    m_input->Bind( wxEVT_KEY_DOWN, &COPPER_PANEL::onInputKeyDown, this );
}


void COPPER_PANEL::onInputKeyDown( wxKeyEvent& aEvent )
{
    aEvent.Skip();
}


void COPPER_PANEL::onSendMessage( wxCommandEvent& aEvent )
{
    wxString text = m_input->GetValue().Trim().Trim( false );

    if( text.IsEmpty() || m_busy )
        return;

    if( m_apiKey.empty() )
    {
        promptForApiKey();

        if( m_apiKey.empty() )
            return;
    }

    m_input->SetValue( wxEmptyString );
    appendUserMessage( text );

    wxString intent = detectIntent( text );

    if( intent == wxT( "generate" ) )
        doGenerate( text );
    else if( intent == wxT( "recommend" ) )
        doRecommend( text );
    else if( intent == wxT( "explain" ) )
        doExplain();
    else if( intent == wxT( "verify" ) )
        doVerify();
    else
        doChat( text );
}


void COPPER_PANEL::appendUserMessage( const wxString& aText )
{
    m_conversation->SetDefaultStyle( wxRichTextAttr() );
    m_conversation->BeginTextColour( ACCENT );
    m_conversation->BeginBold();
    m_conversation->WriteText( wxT( "You: " ) );
    m_conversation->EndBold();
    m_conversation->EndTextColour();

    m_conversation->BeginTextColour( TEXT_PRIMARY );
    m_conversation->WriteText( aText + wxT( "\n\n" ) );
    m_conversation->EndTextColour();

    scrollToBottom();
}


void COPPER_PANEL::appendAssistantMessage( const wxString& aText )
{
    m_conversation->SetDefaultStyle( wxRichTextAttr() );
    m_conversation->BeginTextColour( wxColour( 120, 180, 240 ) );
    m_conversation->BeginBold();
    m_conversation->WriteText( wxT( "Copper: " ) );
    m_conversation->EndBold();
    m_conversation->EndTextColour();

    m_conversation->BeginTextColour( TEXT_PRIMARY );
    m_conversation->WriteText( aText + wxT( "\n\n" ) );
    m_conversation->EndTextColour();

    scrollToBottom();
}


void COPPER_PANEL::appendErrorMessage( const wxString& aText )
{
    m_conversation->SetDefaultStyle( wxRichTextAttr() );
    m_conversation->BeginTextColour( ERROR_COLOR );
    m_conversation->WriteText( wxT( "Error: " ) + aText + wxT( "\n\n" ) );
    m_conversation->EndTextColour();

    scrollToBottom();
}


void COPPER_PANEL::appendStatusMessage( const wxString& aText )
{
    m_conversation->SetDefaultStyle( wxRichTextAttr() );
    m_conversation->BeginTextColour( TEXT_SECONDARY );
    m_conversation->BeginItalic();
    m_conversation->WriteText( aText + wxT( "\n" ) );
    m_conversation->EndItalic();
    m_conversation->EndTextColour();

    scrollToBottom();
}


void COPPER_PANEL::clearConversation()
{
    m_conversation->Clear();
    m_history.clear();
}


void COPPER_PANEL::scrollToBottom()
{
    m_conversation->ShowPosition( m_conversation->GetLastPosition() );
}


void COPPER_PANEL::setStatus( const wxString& aText )
{
    m_statusText->SetLabel( aText );
}


void COPPER_PANEL::setBusy( bool aBusy )
{
    m_busy = aBusy;
    m_sendButton->Enable( !aBusy );
    m_input->Enable( !aBusy );

    if( aBusy )
        setStatus( wxT( "Thinking..." ) );
    else
        setStatus( wxT( "Ready" ) );
}


wxString COPPER_PANEL::detectIntent( const wxString& aPrompt )
{
    wxString lower = aPrompt.Lower();

    for( const auto& kw : GENERATE_KEYWORDS )
    {
        if( lower.Contains( kw ) )
            return wxT( "generate" );
    }

    for( const auto& kw : RECOMMEND_KEYWORDS )
    {
        if( lower.Contains( kw ) )
            return wxT( "recommend" );
    }

    for( const auto& kw : EXPLAIN_KEYWORDS )
    {
        if( lower.Contains( kw ) )
            return wxT( "explain" );
    }

    for( const auto& kw : VERIFY_KEYWORDS )
    {
        if( lower.Contains( kw ) )
            return wxT( "verify" );
    }

    return wxT( "chat" );
}


std::string COPPER_PANEL::getVendorFilter()
{
    int sel = m_vendorChoice->GetSelection();

    switch( sel )
    {
    case 1: return "lcsc";
    case 2: return "mouser";
    case 3: return "digikey";
    default: return "";
    }
}


// ---------------------------------------------------------------------------
// Schematic context extraction
// ---------------------------------------------------------------------------

nlohmann::json COPPER_PANEL::buildProjectContext()
{
    nlohmann::json ctx;

    SCHEMATIC& schematic = m_frame->Schematic();
    SCH_SCREEN* screen = schematic.RootScreen();
    SCH_SHEET_PATH rootPath;
    rootPath.push_back( &schematic.Root() );

    ctx["project_path"] = std::string( m_frame->Prj().GetProjectPath().ToUTF8() );
    ctx["project_name"] = std::string( m_frame->Prj().GetProjectName().ToUTF8() );

    // Build sheet with components matching ExistingComponent schema
    nlohmann::json sheet;
    sheet["filename"] = std::string( m_frame->GetCurrentFileName().ToUTF8() );

    nlohmann::json components = nlohmann::json::array();

    for( SCH_ITEM* item : screen->Items() )
    {
        if( item->Type() != SCH_SYMBOL_T )
            continue;

        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
        nlohmann::json comp;

        comp["reference"] = std::string( symbol->GetRef( &rootPath, false ).ToUTF8() );
        comp["value"] = std::string( symbol->GetValue( false, &rootPath, false ).ToUTF8() );

        // Split lib_id into symbol_lib and symbol_name
        LIB_ID libId = symbol->GetLibId();
        comp["symbol_lib"] = std::string( libId.GetLibNickname().wx_str() );
        comp["symbol_name"] = std::string( libId.GetLibItemName().wx_str() );

        VECTOR2I pos = symbol->GetPosition();
        nlohmann::json posJson;
        posJson["x"] = pos.x / 25400.0;
        posJson["y"] = pos.y / 25400.0;
        posJson["rotation"] = static_cast<double>( symbol->GetOrientation() * 90 );
        comp["position"] = posJson;

        wxString footprint = symbol->GetFootprintFieldText( true, &rootPath, false );
        comp["footprint"] = std::string( footprint.ToUTF8() );
        comp["properties"] = nlohmann::json::object();

        components.push_back( comp );
    }

    sheet["components"] = components;

    // Nets
    nlohmann::json nets = nlohmann::json::array();

    for( SCH_ITEM* item : screen->Items() )
    {
        if( item->Type() == SCH_LABEL_T || item->Type() == SCH_GLOBAL_LABEL_T )
        {
            SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( item );
            nlohmann::json net;
            net["name"] = std::string( label->GetText().ToUTF8() );
            net["component_pins"] = nlohmann::json::array();
            nets.push_back( net );
        }
    }

    sheet["nets"] = nets;

    // Power rails
    nlohmann::json powerRails = nlohmann::json::array();

    for( SCH_ITEM* item : screen->Items() )
    {
        if( item->Type() == SCH_SYMBOL_T )
        {
            SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );

            if( symbol->GetLibId().GetLibNickname() == "power" )
            {
                nlohmann::json rail;
                rail["name"] = std::string( symbol->GetValue( false, &rootPath, false ).ToUTF8() );
                rail["voltage"] = nullptr;
                powerRails.push_back( rail );
            }
        }
    }

    sheet["power_rails"] = powerRails;

    ctx["sheets"] = nlohmann::json::array( { sheet } );
    ctx["available_symbol_libs"] = nlohmann::json::array();
    ctx["available_footprint_libs"] = nlohmann::json::array();

    return ctx;
}


// ---------------------------------------------------------------------------
// Cloud API communication
// ---------------------------------------------------------------------------

std::string COPPER_PANEL::callCloudAPI( const std::string& aEndpoint,
                                         const nlohmann::json& aBody )
{
    KICAD_CURL_EASY curl;

    std::string url = m_cloudUrl + aEndpoint;
    curl.SetURL( url );
    curl.SetHeader( "Content-Type", "application/json" );
    curl.SetHeader( "X-API-Key", m_apiKey );
    curl.SetHeader( "User-Agent", "KiCad-Copper/1.0" );

    std::string bodyStr = aBody.dump();
    curl.SetPostFields( bodyStr );
    curl.SetFollowRedirects( true );
    curl.SetConnectTimeout( 30 );

    int code = curl.Perform();

    if( code != 0 )
        throw std::runtime_error( "Connection failed: " + curl.GetErrorText( code ) );

    int status = curl.GetResponseStatusCode();
    const std::string& buffer = curl.GetBuffer();

    if( status >= 400 )
    {
        throw std::runtime_error( "API error (HTTP " + std::to_string( status ) + "): "
                                  + buffer );
    }

    return buffer;
}


std::vector<std::pair<std::string, std::string>> COPPER_PANEL::parseSSE( const std::string& aRaw )
{
    // Parse SSE stream into vector of (event, data) pairs
    std::vector<std::pair<std::string, std::string>> events;
    std::string currentEvent;
    std::string currentData;

    std::istringstream stream( aRaw );
    std::string line;

    while( std::getline( stream, line ) )
    {
        // Remove \r if present
        if( !line.empty() && line.back() == '\r' )
            line.pop_back();

        if( line.rfind( "event: ", 0 ) == 0 )
        {
            currentEvent = line.substr( 7 );
        }
        else if( line.rfind( "data: ", 0 ) == 0 )
        {
            currentData = line.substr( 6 );
        }
        else if( line.empty() )
        {
            if( !currentEvent.empty() || !currentData.empty() )
            {
                events.push_back( { currentEvent, currentData } );
                currentEvent.clear();
                currentData.clear();
            }
        }
    }

    // Catch last event if no trailing blank line
    if( !currentEvent.empty() || !currentData.empty() )
        events.push_back( { currentEvent, currentData } );

    return events;
}


// ---------------------------------------------------------------------------
// Chat endpoint
// ---------------------------------------------------------------------------

void COPPER_PANEL::doChat( const wxString& aPrompt )
{
    setBusy( true );

    m_history.push_back( { "user", std::string( aPrompt.ToUTF8() ) } );

    // Run on background thread to avoid blocking UI
    if( m_workerThread.joinable() )
        m_workerThread.detach();

    m_workerThread = std::thread( [this, prompt = std::string( aPrompt.ToUTF8() )]()
    {
        try
        {
            m_frame->CallAfter( [this]()
            {
                appendStatusMessage( wxT( "[Thinking] Analyzing your question..." ) );
            } );

            nlohmann::json ctx = buildProjectContext();

            m_frame->CallAfter( [this]()
            {
                appendStatusMessage( wxT( "[Thinking] Sending schematic context to Copper AI..." ) );
            } );

            nlohmann::json body;
            body["message"] = prompt;
            body["project_context"] = ctx;

            nlohmann::json historyArr = nlohmann::json::array();
            for( const auto& [role, content] : m_history )
            {
                historyArr.push_back( { { "role", role }, { "content", content } } );
            }
            body["history"] = historyArr;

            m_frame->CallAfter( [this]()
            {
                appendStatusMessage( wxT( "[Thinking] Waiting for response..." ) );
            } );

            std::string response = callCloudAPI( "/v1/copilot/chat", body );
            nlohmann::json respJson = nlohmann::json::parse( response );

            std::string reply = respJson.value( "reply", respJson.value( "response",
                                respJson.value( "message", "" ) ) );
            m_history.push_back( { "assistant", reply } );

            m_frame->CallAfter( [this, reply]()
            {
                appendAssistantMessage( wxString::FromUTF8( reply ) );
                setBusy( false );
            } );
        }
        catch( const std::exception& e )
        {
            std::string err = e.what();
            m_frame->CallAfter( [this, err]()
            {
                appendErrorMessage( wxString::FromUTF8( err ) );
                setBusy( false );
            } );
        }
    } );
}


// ---------------------------------------------------------------------------
// Generate endpoint (schematic generation)
// ---------------------------------------------------------------------------

void COPPER_PANEL::doGenerate( const wxString& aPrompt )
{
    setBusy( true );
    appendStatusMessage( wxT( "Generating schematic design..." ) );

    m_history.push_back( { "user", std::string( aPrompt.ToUTF8() ) } );

    if( m_workerThread.joinable() )
        m_workerThread.detach();

    m_workerThread = std::thread( [this, prompt = std::string( aPrompt.ToUTF8() )]()
    {
        try
        {
            nlohmann::json body;
            body["prompt"] = prompt;
            body["project_context"] = buildProjectContext();

            std::string vf = getVendorFilter();
            if( !vf.empty() )
                body["vendor_filter"] = vf;

            // Generate returns SSE stream - fetch raw response
            KICAD_CURL_EASY curl;
            std::string url = m_cloudUrl + "/v1/copilot/generate";
            curl.SetURL( url );
            curl.SetHeader( "Content-Type", "application/json" );
            curl.SetHeader( "X-API-Key", m_apiKey );
            curl.SetHeader( "User-Agent", "KiCad-Copper/1.0" );
            curl.SetHeader( "Accept", "text/event-stream" );
            curl.SetPostFields( body.dump() );
            curl.SetFollowRedirects( true );
            curl.SetConnectTimeout( 120 );

            int code = curl.Perform();

            if( code != 0 )
                throw std::runtime_error( "Connection failed: " + curl.GetErrorText( code ) );

            int status = curl.GetResponseStatusCode();

            if( status >= 400 )
                throw std::runtime_error( "API error (HTTP " + std::to_string( status )
                                          + "): " + curl.GetBuffer() );

            // Parse SSE events
            auto events = parseSSE( curl.GetBuffer() );
            std::string description;
            nlohmann::json lastPatch;

            for( const auto& [evt, data] : events )
            {
                if( data.empty() )
                    continue;

                try
                {
                    nlohmann::json eventData = nlohmann::json::parse( data );

                    if( evt == "stage" )
                    {
                        std::string msg = eventData.value( "message", "" );
                        std::string stage = eventData.value( "stage", "" );
                        m_frame->CallAfter( [this, stage, msg]()
                        {
                            appendStatusMessage( wxString::Format( wxT( "[%s] %s" ),
                                    wxString::FromUTF8( stage ),
                                    wxString::FromUTF8( msg ) ) );
                        } );
                    }
                    else if( evt == "operation" )
                    {
                        int idx = eventData.value( "index", 0 );
                        int total = eventData.value( "total", 0 );
                        m_frame->CallAfter( [this, idx, total]()
                        {
                            appendStatusMessage( wxString::Format(
                                    wxT( "[Applying] Operation %d of %d" ), idx + 1, total ) );
                        } );

                        // Collect operations into a patch
                        if( !lastPatch.contains( "operations" ) )
                            lastPatch["operations"] = nlohmann::json::array();

                        if( eventData.contains( "operation" ) )
                            lastPatch["operations"].push_back( eventData["operation"] );
                    }
                    else if( evt == "complete" )
                    {
                        description = eventData.value( "description", "Design generated." );
                    }
                    else if( evt == "error" )
                    {
                        std::string errMsg = eventData.value( "error", "Unknown error" );
                        m_frame->CallAfter( [this, errMsg]()
                        {
                            appendErrorMessage( wxString::FromUTF8( errMsg ) );
                        } );
                    }
                }
                catch( ... )
                {
                    // Skip unparseable events
                }
            }

            m_frame->CallAfter( [this, description, lastPatch]()
            {
                if( !description.empty() )
                    appendAssistantMessage( wxString::FromUTF8( description ) );

                if( lastPatch.contains( "operations" ) )
                    applySchematicPatch( lastPatch );

                setBusy( false );
            } );
        }
        catch( const std::exception& e )
        {
            std::string err = e.what();
            m_frame->CallAfter( [this, err]()
            {
                appendErrorMessage( wxString::FromUTF8( err ) );
                setBusy( false );
            } );
        }
    } );
}


// ---------------------------------------------------------------------------
// Recommend endpoint
// ---------------------------------------------------------------------------

void COPPER_PANEL::doRecommend( const wxString& aPrompt )
{
    setBusy( true );
    appendStatusMessage( wxT( "Searching for components..." ) );

    if( m_workerThread.joinable() )
        m_workerThread.detach();

    m_workerThread = std::thread( [this, prompt = std::string( aPrompt.ToUTF8() )]()
    {
        try
        {
            nlohmann::json body;
            body["prompt"] = prompt;
            body["project_context"] = buildProjectContext();

            std::string vf = getVendorFilter();
            if( !vf.empty() )
                body["vendor_filter"] = vf;

            std::string response = callCloudAPI( "/v1/copilot/recommend", body );
            nlohmann::json respJson = nlohmann::json::parse( response );

            std::string reply = respJson.value( "response", respJson.dump( 2 ) );

            m_frame->CallAfter( [this, reply]()
            {
                appendAssistantMessage( wxString::FromUTF8( reply ) );
                setBusy( false );
            } );
        }
        catch( const std::exception& e )
        {
            std::string err = e.what();
            m_frame->CallAfter( [this, err]()
            {
                appendErrorMessage( wxString::FromUTF8( err ) );
                setBusy( false );
            } );
        }
    } );
}


// ---------------------------------------------------------------------------
// Explain endpoint
// ---------------------------------------------------------------------------

void COPPER_PANEL::doExplain()
{
    setBusy( true );
    appendStatusMessage( wxT( "Analyzing schematic..." ) );

    if( m_workerThread.joinable() )
        m_workerThread.detach();

    m_workerThread = std::thread( [this]()
    {
        try
        {
            nlohmann::json body;
            body["project_context"] = buildProjectContext();

            // Explain returns SSE
            KICAD_CURL_EASY curl;
            std::string url = m_cloudUrl + "/v1/copilot/explain";
            curl.SetURL( url );
            curl.SetHeader( "Content-Type", "application/json" );
            curl.SetHeader( "X-API-Key", m_apiKey );
            curl.SetHeader( "User-Agent", "KiCad-Copper/1.0" );
            curl.SetHeader( "Accept", "text/event-stream" );
            curl.SetPostFields( body.dump() );
            curl.SetFollowRedirects( true );
            curl.SetConnectTimeout( 120 );

            int code = curl.Perform();

            if( code != 0 )
                throw std::runtime_error( "Connection failed: " + curl.GetErrorText( code ) );

            if( curl.GetResponseStatusCode() >= 400 )
                throw std::runtime_error( "API error: " + curl.GetBuffer() );

            auto events = parseSSE( curl.GetBuffer() );
            std::string explanation;

            for( const auto& [evt, data] : events )
            {
                if( data.empty() ) continue;

                try
                {
                    nlohmann::json eventData = nlohmann::json::parse( data );

                    if( evt == "stage" )
                    {
                        std::string msg = eventData.value( "message", "" );
                        m_frame->CallAfter( [this, msg]()
                        {
                            appendStatusMessage( wxString::Format( wxT( "[Analyzing] %s" ),
                                    wxString::FromUTF8( msg ) ) );
                        } );
                    }
                    else if( evt == "complete" )
                    {
                        explanation = eventData.value( "explanation",
                                      eventData.value( "response", "" ) );
                    }
                    else if( evt == "error" )
                    {
                        std::string errMsg = eventData.value( "error", "Unknown error" );
                        m_frame->CallAfter( [this, errMsg]()
                        {
                            appendErrorMessage( wxString::FromUTF8( errMsg ) );
                        } );
                    }
                }
                catch( ... ) {}
            }

            m_frame->CallAfter( [this, explanation]()
            {
                if( !explanation.empty() )
                    appendAssistantMessage( wxString::FromUTF8( explanation ) );
                setBusy( false );
            } );
        }
        catch( const std::exception& e )
        {
            std::string err = e.what();
            m_frame->CallAfter( [this, err]()
            {
                appendErrorMessage( wxString::FromUTF8( err ) );
                setBusy( false );
            } );
        }
    } );
}


// ---------------------------------------------------------------------------
// Verify endpoint
// ---------------------------------------------------------------------------

void COPPER_PANEL::doVerify()
{
    setBusy( true );
    appendStatusMessage( wxT( "Running design verification..." ) );

    if( m_workerThread.joinable() )
        m_workerThread.detach();

    m_workerThread = std::thread( [this]()
    {
        try
        {
            nlohmann::json body;
            body["project_context"] = buildProjectContext();

            // Verify returns SSE
            KICAD_CURL_EASY curl;
            std::string url = m_cloudUrl + "/v1/copilot/verify";
            curl.SetURL( url );
            curl.SetHeader( "Content-Type", "application/json" );
            curl.SetHeader( "X-API-Key", m_apiKey );
            curl.SetHeader( "User-Agent", "KiCad-Copper/1.0" );
            curl.SetHeader( "Accept", "text/event-stream" );
            curl.SetPostFields( body.dump() );
            curl.SetFollowRedirects( true );
            curl.SetConnectTimeout( 120 );

            int code = curl.Perform();

            if( code != 0 )
                throw std::runtime_error( "Connection failed: " + curl.GetErrorText( code ) );

            if( curl.GetResponseStatusCode() >= 400 )
                throw std::runtime_error( "API error: " + curl.GetBuffer() );

            auto events = parseSSE( curl.GetBuffer() );
            std::string report;

            for( const auto& [evt, data] : events )
            {
                if( data.empty() ) continue;

                try
                {
                    nlohmann::json eventData = nlohmann::json::parse( data );

                    if( evt == "stage" )
                    {
                        std::string msg = eventData.value( "message", "" );
                        m_frame->CallAfter( [this, msg]()
                        {
                            appendStatusMessage( wxString::Format( wxT( "[Verifying] %s" ),
                                    wxString::FromUTF8( msg ) ) );
                        } );
                    }
                    else if( evt == "complete" )
                    {
                        report = eventData.value( "report",
                                 eventData.value( "response", "" ) );
                    }
                    else if( evt == "error" )
                    {
                        std::string errMsg = eventData.value( "error", "Unknown error" );
                        m_frame->CallAfter( [this, errMsg]()
                        {
                            appendErrorMessage( wxString::FromUTF8( errMsg ) );
                        } );
                    }
                }
                catch( ... ) {}
            }

            m_frame->CallAfter( [this, report]()
            {
                if( !report.empty() )
                    appendAssistantMessage( wxString::FromUTF8( report ) );
                setBusy( false );
            } );
        }
        catch( const std::exception& e )
        {
            std::string err = e.what();
            m_frame->CallAfter( [this, err]()
            {
                appendErrorMessage( wxString::FromUTF8( err ) );
                setBusy( false );
            } );
        }
    } );
}


// ---------------------------------------------------------------------------
// Schematic patch application
// ---------------------------------------------------------------------------

void COPPER_PANEL::applySchematicPatch( const nlohmann::json& aPatch )
{
    if( !aPatch.contains( "operations" ) )
    {
        appendStatusMessage( wxT( "No operations in patch." ) );
        return;
    }

    TOOL_MANAGER* toolMgr = m_frame->GetToolManager();

    if( !toolMgr )
    {
        appendErrorMessage( wxT( "Tool manager not available." ) );
        return;
    }

    SCH_SCREEN* screen = m_frame->GetScreen();
    SCH_SHEET_PATH& sheetPath = m_frame->GetCurrentSheet();
    SCH_COMMIT commit( toolMgr );
    int applied = 0;
    int failed = 0;

    for( const auto& op : aPatch["operations"] )
    {
        std::string opType = op.value( "op_type", op.value( "type", "" ) );
        nlohmann::json data = op.value( "data", op );

        try
        {
            if( opType == "PLACE_COMPONENT" )
            {
                std::string symLib = data.value( "symbol_lib", "" );
                std::string symName = data.value( "symbol_name", "" );

                if( symLib.empty() || symName.empty() )
                {
                    failed++;
                    continue;
                }

                LIB_ID libId;
                libId.SetLibNickname( wxString::FromUTF8( symLib ) );
                libId.SetLibItemName( wxString::FromUTF8( symName ) );

                LIB_SYMBOL* libSymbol = m_frame->GetLibSymbol( libId );

                if( !libSymbol )
                {
                    appendStatusMessage( wxString::Format( wxT( "  Symbol not found: %s:%s" ),
                            wxString::FromUTF8( symLib ),
                            wxString::FromUTF8( symName ) ) );
                    failed++;
                    continue;
                }

                SCH_SYMBOL* symbol = new SCH_SYMBOL( *libSymbol, libId, &sheetPath, 1 );
                symbol->SetParent( screen );

                // Position (convert mm to internal units: 1mm = 25400 IU)
                nlohmann::json pos = data.value( "position",
                                      nlohmann::json( { { "x", 0 }, { "y", 0 } } ) );
                double xMm = pos.value( "x", 0.0 );
                double yMm = pos.value( "y", 0.0 );
                symbol->SetPosition( VECTOR2I( xMm * 25400, yMm * 25400 ) );

                // Reference and value
                std::string ref = data.value( "reference", "" );
                std::string val = data.value( "value", "" );

                if( !ref.empty() )
                    symbol->SetRef( &sheetPath, wxString::FromUTF8( ref ) );

                if( !val.empty() )
                    symbol->SetValue( &sheetPath, wxString::FromUTF8( val ) );

                // Footprint
                std::string fp = data.value( "footprint", "" );

                if( !fp.empty() )
                    symbol->SetFootprintFieldText( wxString::FromUTF8( fp ) );

                symbol->AutoplaceFields( screen, AUTOPLACE_AUTO );

                commit.Added( symbol, screen );
                applied++;
            }
            else if( opType == "ADD_WIRE" )
            {
                nlohmann::json startPos = data.value( "start",
                                           nlohmann::json( { { "x", 0 }, { "y", 0 } } ) );
                nlohmann::json endPos = data.value( "end",
                                         nlohmann::json( { { "x", 0 }, { "y", 0 } } ) );

                VECTOR2I start( startPos.value( "x", 0.0 ) * 25400,
                                startPos.value( "y", 0.0 ) * 25400 );
                VECTOR2I end( endPos.value( "x", 0.0 ) * 25400,
                              endPos.value( "y", 0.0 ) * 25400 );

                SCH_LINE* wire = new SCH_LINE( start, LAYER_WIRE );
                wire->SetEndPoint( end );
                wire->SetParent( screen );

                commit.Added( wire, screen );
                applied++;
            }
            else if( opType == "ADD_NET_LABEL" )
            {
                std::string name = data.value( "name", "" );
                nlohmann::json pos = data.value( "position",
                                      nlohmann::json( { { "x", 0 }, { "y", 0 } } ) );

                VECTOR2I labelPos( pos.value( "x", 0.0 ) * 25400,
                                   pos.value( "y", 0.0 ) * 25400 );

                SCH_LABEL* label = new SCH_LABEL( labelPos,
                                                   wxString::FromUTF8( name ) );
                label->SetParent( screen );

                commit.Added( label, screen );
                applied++;
            }
            else if( opType == "ADD_GLOBAL_LABEL" )
            {
                std::string name = data.value( "name", "" );
                nlohmann::json pos = data.value( "position",
                                      nlohmann::json( { { "x", 0 }, { "y", 0 } } ) );

                VECTOR2I labelPos( pos.value( "x", 0.0 ) * 25400,
                                   pos.value( "y", 0.0 ) * 25400 );

                SCH_GLOBALLABEL* label = new SCH_GLOBALLABEL( labelPos,
                                                               wxString::FromUTF8( name ) );
                label->SetParent( screen );

                commit.Added( label, screen );
                applied++;
            }
            else if( opType == "ADD_JUNCTION" )
            {
                nlohmann::json pos = data.value( "position",
                                      nlohmann::json( { { "x", 0 }, { "y", 0 } } ) );

                VECTOR2I jPos( pos.value( "x", 0.0 ) * 25400,
                               pos.value( "y", 0.0 ) * 25400 );

                SCH_JUNCTION* junction = new SCH_JUNCTION( jPos );
                junction->SetParent( screen );

                commit.Added( junction, screen );
                applied++;
            }
            else
            {
                // Unsupported op type — skip
                failed++;
            }
        }
        catch( const std::exception& e )
        {
            appendStatusMessage( wxString::Format( wxT( "  Failed: %s — %s" ),
                    wxString::FromUTF8( opType ),
                    wxString::FromUTF8( e.what() ) ) );
            failed++;
        }
    }

    if( applied > 0 )
    {
        commit.Push( _( "Copper AI: Apply schematic patch" ) );
        m_frame->GetCanvas()->Refresh();
    }

    appendStatusMessage( wxString::Format( wxT( "Applied %d operations (%d skipped)." ),
                                            applied, failed ) );
}


// ---------------------------------------------------------------------------
// Config management
// ---------------------------------------------------------------------------

void COPPER_PANEL::loadConfig()
{
    wxString configDir = wxStandardPaths::Get().GetUserConfigDir() + wxFileName::GetPathSeparator()
                         + wxT( ".copper" );
    wxString configPath = configDir + wxFileName::GetPathSeparator() + wxT( "config.json" );

    if( !wxFileName::FileExists( configPath ) )
        return;

    try
    {
        std::ifstream f( configPath.ToStdString() );
        nlohmann::json config = nlohmann::json::parse( f );

        if( config.contains( "api_key" ) )
            m_apiKey = config["api_key"].get<std::string>();

        if( config.contains( "cloud_url" ) )
            m_cloudUrl = config["cloud_url"].get<std::string>();
    }
    catch( ... )
    {
        // Ignore config errors
    }
}


void COPPER_PANEL::promptForApiKey()
{
    wxTextEntryDialog keyDlg( this,
                              wxT( "Enter your Copper API key.\n"
                                   "Register locally: curl -X POST http://localhost:8000/v1/auth/register "
                                   "-H \"Content-Type: application/json\" "
                                   "-d '{\"email\":\"you@test.com\",\"password\":\"test123\"}'" ),
                              wxT( "Copper API Key" ),
                              wxString::FromUTF8( m_apiKey ) );

    if( keyDlg.ShowModal() != wxID_OK )
        return;

    m_apiKey = std::string( keyDlg.GetValue().ToUTF8() );

    wxTextEntryDialog urlDlg( this,
                              wxT( "Copper API URL (leave default for local development):" ),
                              wxT( "Copper API URL" ),
                              wxString::FromUTF8( m_cloudUrl ) );

    if( urlDlg.ShowModal() == wxID_OK )
        m_cloudUrl = std::string( urlDlg.GetValue().ToUTF8() );

    // Save to config
    wxString configDir = wxStandardPaths::Get().GetUserConfigDir()
                         + wxFileName::GetPathSeparator() + wxT( ".copper" );

    if( !wxDirExists( configDir ) )
        wxMkdir( configDir );

    wxString configPath = configDir + wxFileName::GetPathSeparator() + wxT( "config.json" );

    try
    {
        nlohmann::json config;

        if( wxFileName::FileExists( configPath ) )
        {
            std::ifstream f( configPath.ToStdString() );
            config = nlohmann::json::parse( f );
        }

        config["api_key"] = m_apiKey;
        config["cloud_url"] = m_cloudUrl;

        std::ofstream f( configPath.ToStdString() );
        f << config.dump( 2 );
    }
    catch( ... )
    {
        appendErrorMessage( wxT( "Failed to save settings." ) );
    }
}
