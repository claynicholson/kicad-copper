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
#include <lib_symbol.h>

#include <nlohmann/json.hpp>

#include <wx/button.h>
#include <wx/richtext/richtextctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/dir.h>

#include <fstream>


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
        m_cloudUrl( "https://api.copper.dev" ),
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

    // Separator
    wxPanel* sep = new wxPanel( this, wxID_ANY, wxDefaultPosition, wxSize( -1, 1 ) );
    sep->SetBackgroundColour( BORDER_COLOR );
    mainSizer->Add( sep, 0, wxEXPAND | wxLEFT | wxRIGHT, 8 );

    // Conversation area
    m_conversation = new wxRichTextCtrl( this, wxID_ANY, wxEmptyString,
                                          wxDefaultPosition, wxDefaultSize,
                                          wxRE_MULTILINE | wxRE_READONLY | wxBORDER_NONE );
    m_conversation->SetBackgroundColour( BG_PRIMARY );
    m_conversation->SetForegroundColour( TEXT_PRIMARY );
    mainSizer->Add( m_conversation, 1, wxEXPAND | wxALL, 4 );

    // Welcome message
    m_conversation->SetDefaultStyle( wxRichTextAttr() );
    m_conversation->BeginTextColour( TEXT_SECONDARY );
    m_conversation->WriteText( wxT( "Welcome to Copper AI. Ask me to design circuits, "
                                     "explain schematics, find components, or verify your designs.\n\n"
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

    ctx["project_name"] = std::string( m_frame->Prj().GetProjectName().ToUTF8() );
    ctx["schematic_path"] = std::string( m_frame->GetCurrentFileName().ToUTF8() );

    // Components
    nlohmann::json components = nlohmann::json::array();

    for( SCH_ITEM* item : screen->Items() )
    {
        if( item->Type() != SCH_SYMBOL_T )
            continue;

        SCH_SYMBOL* symbol = static_cast<SCH_SYMBOL*>( item );
        nlohmann::json comp;

        comp["reference"] = std::string( symbol->GetRef( &rootPath, false ).ToUTF8() );
        comp["value"] = std::string( symbol->GetValue( false, &rootPath, false ).ToUTF8() );
        comp["lib_id"] = std::string( symbol->GetLibId().Format().wx_str() );

        VECTOR2I pos = symbol->GetPosition();
        comp["x"] = pos.x / 25400.0;  // Convert to mm
        comp["y"] = pos.y / 25400.0;

        wxString footprint = symbol->GetFootprintFieldText( true, &rootPath, false );
        comp["footprint"] = std::string( footprint.ToUTF8() );

        components.push_back( comp );
    }

    ctx["components"] = components;

    // Wires
    nlohmann::json wires = nlohmann::json::array();

    for( SCH_ITEM* item : screen->Items() )
    {
        if( item->Type() != SCH_LINE_T )
            continue;

        SCH_LINE* line = static_cast<SCH_LINE*>( item );

        if( !line->IsWire() )
            continue;

        nlohmann::json wire;
        wire["x1"] = line->GetStartPoint().x / 25400.0;
        wire["y1"] = line->GetStartPoint().y / 25400.0;
        wire["x2"] = line->GetEndPoint().x / 25400.0;
        wire["y2"] = line->GetEndPoint().y / 25400.0;
        wires.push_back( wire );
    }

    ctx["wires"] = wires;

    // Net labels
    nlohmann::json labels = nlohmann::json::array();

    for( SCH_ITEM* item : screen->Items() )
    {
        if( item->Type() == SCH_LABEL_T || item->Type() == SCH_GLOBAL_LABEL_T )
        {
            SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( item );
            nlohmann::json lbl;

            lbl["name"] = std::string( label->GetText().ToUTF8() );
            lbl["x"] = label->GetPosition().x / 25400.0;
            lbl["y"] = label->GetPosition().y / 25400.0;
            lbl["type"] = ( item->Type() == SCH_GLOBAL_LABEL_T ) ? "global" : "net";

            labels.push_back( lbl );
        }
    }

    ctx["labels"] = labels;

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
    curl.SetHeader( "Authorization", "Bearer " + m_apiKey );
    curl.SetHeader( "User-Agent", "KiCad-Copper/1.0" );
    curl.SetPostFields( aBody.dump() );
    curl.SetFollowRedirects( true );
    curl.SetConnectTimeout( 30 );

    int code = curl.Perform();

    if( code != 0 )
        throw std::runtime_error( "HTTP request failed: " + curl.GetErrorText( code ) );

    int status = curl.GetResponseStatusCode();

    if( status >= 400 )
    {
        throw std::runtime_error( "API error (HTTP " + std::to_string( status ) + "): "
                                  + curl.GetBuffer() );
    }

    return curl.GetBuffer();
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
            nlohmann::json body;
            body["message"] = prompt;
            body["project_context"] = buildProjectContext();

            nlohmann::json historyArr = nlohmann::json::array();
            for( const auto& [role, content] : m_history )
            {
                historyArr.push_back( { { "role", role }, { "content", content } } );
            }
            body["history"] = historyArr;

            std::string response = callCloudAPI( "/v1/copilot/chat", body );
            nlohmann::json respJson = nlohmann::json::parse( response );

            std::string reply = respJson.value( "response", respJson.value( "message", "" ) );
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

            std::string response = callCloudAPI( "/v1/copilot/generate", body );
            nlohmann::json respJson = nlohmann::json::parse( response );

            std::string description = respJson.value( "description", "Design generated." );

            m_frame->CallAfter( [this, respJson, description]()
            {
                appendAssistantMessage( wxString::FromUTF8( description ) );

                if( respJson.contains( "patch" ) )
                {
                    appendStatusMessage( wxT( "Applying schematic changes..." ) );
                    applySchematicPatch( respJson["patch"] );
                    appendStatusMessage( wxT( "Schematic updated." ) );
                }

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

            std::string response = callCloudAPI( "/v1/copilot/explain", body );
            nlohmann::json respJson = nlohmann::json::parse( response );

            std::string explanation = respJson.value( "explanation",
                                                       respJson.value( "response", "" ) );

            m_frame->CallAfter( [this, explanation]()
            {
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

            std::string response = callCloudAPI( "/v1/copilot/verify", body );
            nlohmann::json respJson = nlohmann::json::parse( response );

            std::string report = respJson.value( "report", respJson.value( "response", "" ) );

            m_frame->CallAfter( [this, report]()
            {
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
    // TODO: Implement native schematic manipulation using KiCad objects
    // For now, log what would be applied
    if( !aPatch.contains( "operations" ) )
    {
        appendStatusMessage( wxT( "No operations in patch." ) );
        return;
    }

    int count = 0;

    for( const auto& op : aPatch["operations"] )
    {
        std::string type = op.value( "type", "" );
        count++;

        if( type == "PLACE_COMPONENT" )
        {
            appendStatusMessage( wxString::Format( wxT( "  Place: %s (%s)" ),
                    wxString::FromUTF8( op.value( "reference", "?" ) ),
                    wxString::FromUTF8( op.value( "lib_id", "?" ) ) ) );
        }
        else if( type == "ADD_WIRE" )
        {
            appendStatusMessage( wxT( "  Add wire" ) );
        }
        else if( type == "ADD_NET_LABEL" || type == "ADD_GLOBAL_LABEL" )
        {
            appendStatusMessage( wxString::Format( wxT( "  Add label: %s" ),
                    wxString::FromUTF8( op.value( "name", "?" ) ) ) );
        }
    }

    appendStatusMessage( wxString::Format( wxT( "%d operations logged (apply not yet implemented)." ),
                                            count ) );
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
    wxTextEntryDialog dlg( this,
                           wxT( "Enter your Copper API key.\n"
                                "Get one at https://copper.dev/dashboard/api-keys" ),
                           wxT( "Copper API Key" ),
                           wxString::FromUTF8( m_apiKey ) );

    if( dlg.ShowModal() == wxID_OK )
    {
        m_apiKey = std::string( dlg.GetValue().ToUTF8() );

        // Save to config
        wxString configDir = wxStandardPaths::Get().GetUserConfigDir()
                             + wxFileName::GetPathSeparator() + wxT( ".copper" );

        if( !wxDirExists( configDir ) )
            wxMkdir( configDir );

        wxString configPath = configDir + wxFileName::GetPathSeparator() + wxT( "config.json" );

        try
        {
            nlohmann::json config;

            // Read existing config if present
            if( wxFileName::FileExists( configPath ) )
            {
                std::ifstream f( configPath.ToStdString() );
                config = nlohmann::json::parse( f );
            }

            config["api_key"] = m_apiKey;

            std::ofstream f( configPath.ToStdString() );
            f << config.dump( 2 );
        }
        catch( ... )
        {
            appendErrorMessage( wxT( "Failed to save API key." ) );
        }
    }
}
