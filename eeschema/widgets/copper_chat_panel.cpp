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

#include <widgets/copper_chat_panel.h>
#include <widgets/copper_chat_widgets.h>
#include <copper/copper_auth.h>
#include <copper/copper_client.h>
#include <copper/copper_types.h>
#include <sch_edit_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_line.h>
#include <sch_label.h>
#include <sch_junction.h>
#include <sch_no_connect.h>
#include <sch_bus_entry.h>
#include <sch_sheet.h>
#include <sch_field.h>
#include <eeschema_settings.h>
#include <connection_graph.h>
#include <sch_commit.h>

#include <wx/dcbuffer.h>
#include <wx/msgdlg.h>
#include <wx/textdlg.h>
#include <wx/utils.h>
#include <wx/wrapsizer.h>

#include <set>


// ═══════════════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════════════

COPPER_CHAT_PANEL::COPPER_CHAT_PANEL( wxWindow* aParent, SCH_EDIT_FRAME* aFrame ) :
        wxPanel( aParent, wxID_ANY ),
        m_frame( aFrame ),
        m_headerPanel( nullptr ),
        m_titleLabel( nullptr ),
        m_loginBtn( nullptr ),
        m_userLabel( nullptr ),
        m_settingsBtn( nullptr ),
        m_scrollArea( nullptr ),
        m_messageSizer( nullptr ),
        m_emptyStatePanel( nullptr ),
        m_stagePanel( nullptr ),
        m_inputPanel( nullptr ),
        m_modeChoice( nullptr ),
        m_inputText( nullptr ),
        m_sendBtn( nullptr ),
        m_emptyStateVisible( true )
{
    SetBackgroundColour( COPPER_COLORS::BG_PRIMARY );

    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    buildHeader();
    mainSizer->Add( m_headerPanel, 0, wxEXPAND );

    // Separator
    wxPanel* sep1 = new wxPanel( this, wxID_ANY, wxDefaultPosition, wxSize( -1, 1 ) );
    sep1->SetBackgroundColour( COPPER_COLORS::SEPARATOR );
    mainSizer->Add( sep1, 0, wxEXPAND );

    buildMessageArea();
    mainSizer->Add( m_scrollArea, 1, wxEXPAND );

    // Separator
    wxPanel* sep2 = new wxPanel( this, wxID_ANY, wxDefaultPosition, wxSize( -1, 1 ) );
    sep2->SetBackgroundColour( COPPER_COLORS::SEPARATOR );
    mainSizer->Add( sep2, 0, wxEXPAND );

    buildInputBar();
    mainSizer->Add( m_inputPanel, 0, wxEXPAND );

    SetSizer( mainSizer );

    // Build empty state (shown initially)
    buildEmptyState();

    // Initialize auth. ADR-005 resolution order: env > settings > default.
    EESCHEMA_SETTINGS* cfg = dynamic_cast<EESCHEMA_SETTINGS*>( m_frame->config() );
    std::string apiUrl = "https://api.coppereda.com";

    if( cfg && !cfg->m_Copper.api_url.IsEmpty() )
        apiUrl = cfg->m_Copper.api_url.ToStdString();

    wxString envUrl;

    if( wxGetEnv( wxS( "COPPER_API_URL" ), &envUrl ) && !envUrl.IsEmpty() )
        apiUrl = std::string( envUrl.ToUTF8() );

    m_auth = std::make_unique<COPPER::AUTH>( apiUrl );

    // Restore saved tokens from OS keychain
    m_auth->LoadSavedTokens();

    m_client = std::make_unique<COPPER::CLIENT>( m_auth.get() );

    // Bind auth events
    Bind( COPPER_EVT_AUTH_SUCCESS, &COPPER_CHAT_PANEL::onAuthSuccess, this );
    Bind( COPPER_EVT_AUTH_FAILURE, &COPPER_CHAT_PANEL::onAuthFailure, this );

    updateAuthUI();
}


COPPER_CHAT_PANEL::~COPPER_CHAT_PANEL()
{
    // Tokens are persisted in OS keychain by SECURE_TOKEN_STORE — nothing to save here
}


// ═══════════════════════════════════════════════════════════════════════════
//  UI Construction
// ═══════════════════════════════════════════════════════════════════════════

void COPPER_CHAT_PANEL::buildHeader()
{
    m_headerPanel = new wxPanel( this, wxID_ANY );
    m_headerPanel->SetBackgroundColour( COPPER_COLORS::BG_SECONDARY );
    m_headerPanel->SetMinSize( FromDIP( wxSize( -1, 40 ) ) );

    wxBoxSizer* headerSizer = new wxBoxSizer( wxHORIZONTAL );

    // Title
    m_titleLabel = new wxStaticText( m_headerPanel, wxID_ANY, wxT( "Copper AI" ) );
    m_titleLabel->SetForegroundColour( COPPER_COLORS::ACCENT );
    m_titleLabel->SetFont( m_titleLabel->GetFont().Bold().Larger() );
    headerSizer->Add( m_titleLabel, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP( 12 ) );

    // User label (hidden until authenticated)
    m_userLabel = new wxStaticText( m_headerPanel, wxID_ANY, wxEmptyString );
    m_userLabel->SetForegroundColour( COPPER_COLORS::TEXT_SECONDARY );
    m_userLabel->Hide();
    headerSizer->Add( m_userLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 8 ) );

    // Login button
    m_loginBtn = new wxButton( m_headerPanel, wxID_ANY, wxT( "Login" ),
                               wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT );
    m_loginBtn->SetBackgroundColour( COPPER_COLORS::ACCENT );
    m_loginBtn->SetForegroundColour( *wxWHITE );
    m_loginBtn->Bind( wxEVT_BUTTON, &COPPER_CHAT_PANEL::onLoginClicked, this );
    headerSizer->Add( m_loginBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 4 ) );

    // Settings button
    m_settingsBtn = new wxButton( m_headerPanel, wxID_ANY, wxT( "\xe2\x9a\x99" ),  // ⚙
                                  wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT );
    m_settingsBtn->SetBackgroundColour( COPPER_COLORS::BG_SECONDARY );
    m_settingsBtn->SetForegroundColour( COPPER_COLORS::TEXT_SECONDARY );
    m_settingsBtn->Bind( wxEVT_BUTTON, &COPPER_CHAT_PANEL::onSettingsClicked, this );
    headerSizer->Add( m_settingsBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 8 ) );

    m_headerPanel->SetSizer( headerSizer );
}


void COPPER_CHAT_PANEL::buildMessageArea()
{
    m_scrollArea = new wxScrolledWindow( this, wxID_ANY );
    m_scrollArea->SetBackgroundColour( COPPER_COLORS::BG_PRIMARY );
    m_scrollArea->SetScrollRate( 0, FromDIP( 8 ) );

    m_messageSizer = new wxBoxSizer( wxVERTICAL );
    m_scrollArea->SetSizer( m_messageSizer );
}


void COPPER_CHAT_PANEL::buildInputBar()
{
    m_inputPanel = new wxPanel( this, wxID_ANY );
    m_inputPanel->SetBackgroundColour( COPPER_COLORS::BG_SECONDARY );
    m_inputPanel->SetMinSize( FromDIP( wxSize( -1, 50 ) ) );

    wxBoxSizer* inputSizer = new wxBoxSizer( wxHORIZONTAL );

    // Mode dropdown
    wxArrayString modes;
    modes.Add( wxT( "Design" ) );
    modes.Add( wxT( "Chat" ) );
    modes.Add( wxT( "Recommend" ) );

    m_modeChoice = new wxChoice( m_inputPanel, wxID_ANY, wxDefaultPosition, wxDefaultSize, modes );
    m_modeChoice->SetSelection( 0 );
    m_modeChoice->Bind( wxEVT_CHOICE, &COPPER_CHAT_PANEL::onModeChanged, this );
    inputSizer->Add( m_modeChoice, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP( 8 ) );

    // Text input
    m_inputText = new wxTextCtrl( m_inputPanel, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                  wxDefaultSize, wxTE_PROCESS_ENTER );
    m_inputText->SetBackgroundColour( COPPER_COLORS::INPUT_BG );
    m_inputText->SetForegroundColour( COPPER_COLORS::TEXT_PRIMARY );
    m_inputText->SetHint( wxT( "What would you like to build?" ) );
    m_inputText->Bind( wxEVT_TEXT_ENTER, &COPPER_CHAT_PANEL::onSendClicked, this );
    m_inputText->Bind( wxEVT_KEY_DOWN, &COPPER_CHAT_PANEL::onInputKeyDown, this );
    inputSizer->Add( m_inputText, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP( 6 ) );

    // Send button
    m_sendBtn = new wxButton( m_inputPanel, wxID_ANY, wxT( "Send \xe2\x86\x92" ),  // Send →
                              wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT );
    m_sendBtn->SetBackgroundColour( COPPER_COLORS::ACCENT );
    m_sendBtn->SetForegroundColour( *wxWHITE );
    m_sendBtn->Bind( wxEVT_BUTTON, &COPPER_CHAT_PANEL::onSendClicked, this );
    inputSizer->Add( m_sendBtn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 8 ) );

    m_inputPanel->SetSizer( inputSizer );
}


void COPPER_CHAT_PANEL::buildEmptyState()
{
    m_emptyStatePanel = new wxPanel( m_scrollArea, wxID_ANY );
    m_emptyStatePanel->SetBackgroundColour( COPPER_COLORS::BG_PRIMARY );

    wxBoxSizer* emptySizer = new wxBoxSizer( wxVERTICAL );
    emptySizer->AddStretchSpacer( 1 );

    // Welcome text
    wxStaticText* welcomeText = new wxStaticText( m_emptyStatePanel, wxID_ANY,
                                                  wxT( "What would you like\nto build?" ) );
    welcomeText->SetForegroundColour( COPPER_COLORS::TEXT_MUTED );
    welcomeText->SetFont( welcomeText->GetFont().Larger().Larger() );
    emptySizer->Add( welcomeText, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP( 24 ) );

    // Hint chips
    wxWrapSizer* chipSizer = new wxWrapSizer( wxHORIZONTAL );

    wxArrayString hints;
    hints.Add( wxT( "Design a power supply" ) );
    hints.Add( wxT( "Find a USB connector" ) );
    hints.Add( wxT( "How do bypass caps work?" ) );
    hints.Add( wxT( "Add an LED circuit" ) );

    for( const wxString& hint : hints )
    {
        auto* chip = new COPPER_HINT_CHIP( m_emptyStatePanel, hint );
        chip->Bind( COPPER_EVT_HINT_CLICKED, &COPPER_CHAT_PANEL::onHintClicked, this );
        chipSizer->Add( chip, 0, wxALL, FromDIP( 4 ) );
    }

    emptySizer->Add( chipSizer, 0, wxALIGN_CENTER | wxLEFT | wxRIGHT, FromDIP( 16 ) );
    emptySizer->AddStretchSpacer( 2 );

    m_emptyStatePanel->SetSizer( emptySizer );
    m_messageSizer->Add( m_emptyStatePanel, 1, wxEXPAND );
    m_emptyStateVisible = true;
}


// ═══════════════════════════════════════════════════════════════════════════
//  Event Handlers
// ═══════════════════════════════════════════════════════════════════════════

void COPPER_CHAT_PANEL::onLoginClicked( wxCommandEvent& aEvent )
{
    if( m_auth->IsAuthenticated() )
    {
        // Already logged in — could show a logout option
        return;
    }

    m_auth->StartLogin( this );
    m_loginBtn->SetLabel( wxT( "..." ) );
    m_loginBtn->Disable();
}


void COPPER_CHAT_PANEL::onSettingsClicked( wxCommandEvent& aEvent )
{
    // Closes gap G-SETTINGS (docs/INTEGRATION_V1_AUDIT.md): edit the backend
    // API URL and persist it to EESCHEMA_SETTINGS::m_Copper.api_url.
    // Note: the COPPER_API_URL env var still overrides this at startup
    // (ADR-005 resolution order: env > settings > default).
    wxString current = wxString::FromUTF8( m_auth->GetApiUrl() );

    wxTextEntryDialog dlg( this,
                           wxT( "Copper backend API URL:" ),
                           wxT( "Copper Settings" ),
                           current );

    if( dlg.ShowModal() != wxID_OK )
        return;

    wxString url = dlg.GetValue().Trim().Trim( false );

    if( url.IsEmpty() )
        return;

    if( !url.StartsWith( wxT( "http://" ) ) && !url.StartsWith( wxT( "https://" ) ) )
    {
        wxMessageBox( wxT( "The API URL must start with http:// or https://" ),
                      wxT( "Copper Settings" ), wxICON_ERROR | wxOK, this );
        return;
    }

    // Strip any trailing slash so endpoint paths concatenate cleanly.
    while( url.EndsWith( wxT( "/" ) ) )
        url.RemoveLast();

    EESCHEMA_SETTINGS* cfg = dynamic_cast<EESCHEMA_SETTINGS*>( m_frame->config() );

    if( cfg )
        cfg->m_Copper.api_url = url;

    m_auth->SetApiUrl( std::string( url.ToUTF8() ) );
    addAIMessage( wxString::Format( wxT( "Backend URL set to %s" ), url ) );
}


void COPPER_CHAT_PANEL::onSendClicked( wxCommandEvent& aEvent )
{
    wxString text = m_inputText->GetValue().Trim();

    if( text.IsEmpty() )
        return;

    m_inputText->Clear();
    sendRequest( text );
}


void COPPER_CHAT_PANEL::onInputKeyDown( wxKeyEvent& aEvent )
{
    if( aEvent.GetKeyCode() == WXK_RETURN && !aEvent.ShiftDown() )
    {
        wxCommandEvent evt;
        onSendClicked( evt );
        return;
    }

    aEvent.Skip();
}


void COPPER_CHAT_PANEL::onModeChanged( wxCommandEvent& aEvent )
{
    // Update placeholder text based on mode
    wxString mode = getCurrentMode();

    if( mode == wxT( "Design" ) )
        m_inputText->SetHint( wxT( "What would you like to build?" ) );
    else if( mode == wxT( "Chat" ) )
        m_inputText->SetHint( wxT( "Ask a question..." ) );
    else if( mode == wxT( "Recommend" ) )
        m_inputText->SetHint( wxT( "What component do you need?" ) );
}


void COPPER_CHAT_PANEL::onHintClicked( wxCommandEvent& aEvent )
{
    wxString hint = aEvent.GetString();
    m_inputText->SetValue( hint );
    sendRequest( hint );
}


void COPPER_CHAT_PANEL::onPlanApproved( wxCommandEvent& aEvent )
{
    if( m_pendingOps.empty() )
    {
        addAIMessage( wxT( "Nothing to apply." ) );
        return;
    }

    std::vector<COPPER::Operation> ops;
    ops.swap( m_pendingOps );  // take ownership, leave member empty
    ExecuteOperations( ops );
    addAIMessage( wxString::Format( wxT( "Applied %zu operation(s). "
                                         "Press Ctrl-Z to undo." ),
                                    ops.size() ) );
}


void COPPER_CHAT_PANEL::onPlanEdited( wxCommandEvent& aEvent )
{
    // Inline plan editing is intentionally out of scope — see docs/ROADMAP.md R5.
    m_pendingOps.clear();
    addAIMessage( wxT( "Plan editing isn't supported yet. "
                       "Tell me what to change and I'll generate a new plan." ) );
}


void COPPER_CHAT_PANEL::onAuthSuccess( wxCommandEvent& aEvent )
{
    updateAuthUI();
    m_loginBtn->Enable();
    addAIMessage( wxT( "Logged in successfully! How can I help you?" ) );
}


void COPPER_CHAT_PANEL::onAuthFailure( wxCommandEvent& aEvent )
{
    m_loginBtn->SetLabel( wxT( "Login" ) );
    m_loginBtn->Enable();
    addAIMessage( wxT( "Login failed. Please try again." ) );
}


// ═══════════════════════════════════════════════════════════════════════════
//  Message Handling
// ═══════════════════════════════════════════════════════════════════════════

void COPPER_CHAT_PANEL::addUserMessage( const wxString& aText )
{
    clearEmptyState();

    auto* bubble = new COPPER_MESSAGE_BUBBLE( m_scrollArea, aText,
                                              COPPER_MESSAGE_BUBBLE::Sender::USER );
    m_messageSizer->Add( bubble, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                         FromDIP( 8 ) );

    m_conversationHistory.push_back( wxT( "user: " ) + aText );
    scrollToBottom();
}


void COPPER_CHAT_PANEL::addAIMessage( const wxString& aText )
{
    clearEmptyState();

    auto* bubble = new COPPER_MESSAGE_BUBBLE( m_scrollArea, aText,
                                              COPPER_MESSAGE_BUBBLE::Sender::AI );
    m_messageSizer->Add( bubble, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                         FromDIP( 8 ) );

    m_conversationHistory.push_back( wxT( "ai: " ) + aText );
    scrollToBottom();
}


void COPPER_CHAT_PANEL::addPlanCard( const COPPER::CopperResponse& aResponse )
{
    clearEmptyState();

    std::vector<COPPER_PLAN_CARD::PlanStep> steps;

    for( const auto& s : aResponse.plan.steps )
    {
        COPPER_PLAN_CARD::PlanStep step;
        step.index = s.index;
        step.description = wxString::FromUTF8( s.description );
        steps.push_back( step );
    }

    wxString placementInfo = wxString::FromUTF8( aResponse.plan.placement_info );

    auto* card = new COPPER_PLAN_CARD( m_scrollArea, steps, placementInfo );
    card->Bind( COPPER_EVT_PLAN_APPROVED, &COPPER_CHAT_PANEL::onPlanApproved, this );
    card->Bind( COPPER_EVT_PLAN_EDITED, &COPPER_CHAT_PANEL::onPlanEdited, this );

    m_messageSizer->Add( card, 0, wxEXPAND | wxALL, FromDIP( 8 ) );
    scrollToBottom();
}


void COPPER_CHAT_PANEL::showStages(
        const std::vector<std::pair<wxString, int>>& aStages )
{
    if( !m_stagePanel )
    {
        m_stagePanel = new COPPER_STAGE_PANEL( m_scrollArea );
        m_messageSizer->Add( m_stagePanel, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP( 8 ) );
    }

    std::vector<std::pair<wxString, COPPER_STAGE_INDICATOR::State>> stages;

    for( const auto& [name, state] : aStages )
    {
        stages.push_back( { name, static_cast<COPPER_STAGE_INDICATOR::State>( state ) } );
    }

    m_stagePanel->SetStages( stages );
    scrollToBottom();
}


void COPPER_CHAT_PANEL::scrollToBottom()
{
    m_scrollArea->FitInside();
    m_scrollArea->Layout();

    int x, y;
    m_scrollArea->GetVirtualSize( &x, &y );
    m_scrollArea->Scroll( 0, y );
}


void COPPER_CHAT_PANEL::clearEmptyState()
{
    if( m_emptyStateVisible && m_emptyStatePanel )
    {
        m_messageSizer->Detach( m_emptyStatePanel );
        m_emptyStatePanel->Destroy();
        m_emptyStatePanel = nullptr;
        m_emptyStateVisible = false;
    }
}


// ═══════════════════════════════════════════════════════════════════════════
//  API Interaction
// ═══════════════════════════════════════════════════════════════════════════

void COPPER_CHAT_PANEL::sendRequest( const wxString& aPrompt )
{
    if( !m_auth->IsAuthenticated() )
    {
        addAIMessage( wxT( "Please log in first to use Copper AI." ) );
        return;
    }

    if( m_client->IsBusy() )
    {
        addAIMessage( wxT( "Please wait for the current request to complete." ) );
        return;
    }

    addUserMessage( aPrompt );

    wxString mode = getCurrentMode();
    COPPER::SchematicContext ctx = ExtractContext();
    std::string prompt = aPrompt.ToStdString();

    // Reset stage panel for the new request (fixes gap G-STAGES).
    if( m_stagePanel )
    {
        m_messageSizer->Detach( m_stagePanel );
        m_stagePanel->Destroy();
        m_stagePanel = nullptr;
    }

    // Pending ops from any previous unfinished plan are stale now.
    m_pendingOps.clear();

    // Disable send while processing
    m_sendBtn->Disable();
    m_inputText->Disable();

    auto onResponse = [this]( const COPPER::CopperResponse& r )
    {
        // Must be called on UI thread via CallAfter
        CallAfter( [this, r]()
        {
            handleResponse( r );
            m_sendBtn->Enable();
            m_inputText->Enable();
            m_inputText->SetFocus();
        } );
    };

    auto onError = [this]( const std::string& err )
    {
        CallAfter( [this, err]()
        {
            handleError( err );
            m_sendBtn->Enable();
            m_inputText->Enable();
        } );
    };

    if( mode == wxT( "Design" ) )
    {
        // Use SSE streaming for design mode
        auto onEvent = [this]( const COPPER::SSEEvent& evt )
        {
            CallAfter( [this, evt]() { handleSSEEvent( evt ); } );
        };

        m_client->Generate( prompt, ctx, onEvent, onError );
    }
    else if( mode == wxT( "Chat" ) )
    {
        m_client->Chat( prompt, ctx, onResponse, onError );
    }
    else if( mode == wxT( "Recommend" ) )
    {
        m_client->Recommend( prompt, ctx, onResponse, onError );
    }
}


void COPPER_CHAT_PANEL::handleResponse( const COPPER::CopperResponse& aResponse )
{
    if( !aResponse.success )
    {
        addAIMessage( wxString::Format( wxT( "Error: %s" ),
                                        wxString::FromUTF8( aResponse.error ) ) );
        m_pendingOps.clear();
        return;
    }

    if( !aResponse.message.empty() )
        addAIMessage( wxString::FromUTF8( aResponse.message ) );

    // Stash operations from the final response so onPlanApproved can apply them.
    // See docs/INTEGRATION_V1_AUDIT.md gap B1 / G-APPROVE.
    m_pendingOps = aResponse.operations;

    if( !aResponse.plan.steps.empty() )
        addPlanCard( aResponse );
    else if( !m_pendingOps.empty() )
    {
        // No plan card but ops were sent — surface a small inline card so the
        // user still gets an explicit Approve click. This mirrors the design-
        // safe default of "never apply without confirmation". See PROTOCOL.md.
        COPPER::CopperResponse fauxPlan = aResponse;
        if( fauxPlan.plan.steps.empty() )
        {
            for( size_t i = 0; i < aResponse.operations.size(); ++i )
            {
                COPPER::PlanStep s;
                s.index = (int) i;
                s.description = aResponse.operations[i].type
                                + std::string( " (auto)" );
                fauxPlan.plan.steps.push_back( s );
            }
        }
        addPlanCard( fauxPlan );
    }
}


void COPPER_CHAT_PANEL::handleSSEEvent( const COPPER::SSEEvent& aEvent )
{
    if( aEvent.event == "stage" )
    {
        nlohmann::json data = aEvent.dataAsJson();
        wxString name = wxString::FromUTF8( data.value( "name", "" ) );
        wxString status = wxString::FromUTF8( data.value( "status", "pending" ) );

        int state = 0;  // PENDING

        if( status == "active" )       state = 1;
        else if( status == "complete" ) state = 2;
        else if( status == "error" )   state = 3;

        if( m_stagePanel )
        {
            m_stagePanel->UpdateStage( name, static_cast<COPPER_STAGE_INDICATOR::State>( state ) );
        }
        else
        {
            showStages( { { name, state } } );
        }
    }
    else if( aEvent.event == "message" )
    {
        nlohmann::json data = aEvent.dataAsJson();
        wxString text = wxString::FromUTF8( data.value( "text", "" ) );

        if( !text.IsEmpty() )
            addAIMessage( text );
    }
    else if( aEvent.event == "plan" )
    {
        COPPER::CopperResponse resp;
        nlohmann::json data = aEvent.dataAsJson();
        resp.plan = COPPER::PlanCard::fromJson( data );
        addPlanCard( resp );
    }
    else if( aEvent.event == "done" )
    {
        nlohmann::json data = aEvent.dataAsJson();

        // Defensive parsing (PROTOCOL.md says data is a flattened
        // CopperResponse, but backend v0.1.0 nested it under "plan").
        if( !data.contains( "operations" ) && data.contains( "plan" )
                && data["plan"].is_object() && data["plan"].contains( "operations" ) )
        {
            data = data["plan"];
        }

        COPPER::CopperResponse resp = COPPER::CopperResponse::fromJson( data );
        handleResponse( resp );

        m_sendBtn->Enable();
        m_inputText->Enable();
        m_inputText->SetFocus();
    }
    else if( aEvent.event == "error" )
    {
        nlohmann::json data = aEvent.dataAsJson();
        handleError( data.value( "message", "Unknown error" ) );

        m_sendBtn->Enable();
        m_inputText->Enable();
    }
}


void COPPER_CHAT_PANEL::handleError( const std::string& aError )
{
    // Closes gap G-STATES: classify transport errors into actionable states.
    // COPPER::CLIENT surfaces "HTTP <status>" for non-200 and
    // "Request failed: ..." / "Stream request failed: ..." for curl errors.
    if( aError.find( "HTTP 401" ) != std::string::npos
            || aError.find( "HTTP 403" ) != std::string::npos )
    {
        if( m_auth && m_auth->IsAuthenticated() )
            m_auth->Logout();

        updateAuthUI();
        addAIMessage( wxT( "The backend rejected your credentials (session expired "
                           "or unauthorized). Click Login to sign in again." ) );
        return;
    }

    if( aError.find( "HTTP 429" ) != std::string::npos )
    {
        addAIMessage( wxT( "The backend is rate-limiting requests. Wait a moment, "
                           "then send your message again." ) );
        return;
    }

    if( aError.rfind( "Request failed", 0 ) == 0
            || aError.rfind( "Stream request failed", 0 ) == 0
            || aError.find( "stream ended" ) != std::string::npos )
    {
        addAIMessage( wxString::Format(
                wxT( "Backend connection lost (%s). Check your network or the API "
                     "URL in Settings, then try again." ),
                wxString::FromUTF8( aError ) ) );
        return;
    }

    addAIMessage( wxString::Format( wxT( "Error: %s" ), wxString::FromUTF8( aError ) ) );
}


// ═══════════════════════════════════════════════════════════════════════════
//  Auth UI
// ═══════════════════════════════════════════════════════════════════════════

void COPPER_CHAT_PANEL::updateAuthUI()
{
    if( m_auth && m_auth->IsAuthenticated() )
    {
        m_loginBtn->Hide();
        m_userLabel->SetLabel( wxString::FromUTF8( m_auth->GetUserEmail() ) );
        m_userLabel->Show();
        m_inputText->Enable();
        m_sendBtn->Enable();
    }
    else
    {
        m_loginBtn->Show();
        m_loginBtn->SetLabel( wxT( "Login" ) );
        m_userLabel->Hide();
        // Still allow input — will prompt to login on send
    }

    m_headerPanel->Layout();
}


wxString COPPER_CHAT_PANEL::getCurrentMode() const
{
    if( m_modeChoice )
        return m_modeChoice->GetStringSelection();

    return wxT( "Design" );
}


// ═══════════════════════════════════════════════════════════════════════════
//  Schematic Context Extraction (replaces sidecar/context.py)
// ═══════════════════════════════════════════════════════════════════════════

COPPER::SchematicContext COPPER_CHAT_PANEL::ExtractContext()
{
    COPPER::SchematicContext ctx;

    if( !m_frame || !m_frame->Schematic().IsValid() )
        return ctx;

    SCHEMATIC& schematic = m_frame->Schematic();
    SCH_SCREEN* screen = schematic.RootScreen();

    if( !screen )
        return ctx;

    BOX2I bbox;
    bool bboxInit = false;

    // Walk all items on the current screen
    for( SCH_ITEM* item : screen->Items() )
    {
        // Update bounding box
        BOX2I itemBox = item->GetBoundingBox();

        if( !bboxInit )
        {
            bbox = itemBox;
            bboxInit = true;
        }
        else
        {
            bbox.Merge( itemBox );
        }

        switch( item->Type() )
        {
        case SCH_SYMBOL_T:
        {
            SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
            COPPER::ContextComponent comp;
            comp.reference = sym->GetRef( &schematic.CurrentSheet() ).ToStdString();
            comp.value = sym->GetField( FIELD_T::VALUE )->GetText().ToStdString();
            // UTF8 has implicit `operator const std::string&` — no ToStdString() needed.
            comp.lib_id = sym->GetLibId().Format();
            comp.position = sym->GetPosition();
            comp.rotation = sym->GetOrientation() * 90.0;
            ctx.components.push_back( comp );
            break;
        }
        default:
            break;
        }
    }

    // Extract nets from connection graph
    CONNECTION_GRAPH* connGraph = schematic.ConnectionGraph();

    if( connGraph )
    {
        // Get all net names
        auto netMap = connGraph->GetNetMap();

        for( const auto& [netKey, subgraphs] : netMap )
        {
            COPPER::ContextNet net;
            net.name = netKey.Name.ToStdString();

            // Determine if this is a power rail
            for( const auto* subgraph : subgraphs )
            {
                if( subgraph->GetDriver() && subgraph->GetDriver()->Type() == SCH_PIN_T )
                {
                    // Power-net detection used to live on SCH_CONNECTION (IsPowerConnection);
                    // in current KiCad it's an attribute of the SCH_PIN itself.
                    const SCH_PIN* drvPin = static_cast<const SCH_PIN*>( subgraph->GetDriver() );

                    if( drvPin && drvPin->IsPower() )
                        ctx.power_rails.push_back( net.name );
                }

                // Add connected pins
                for( SCH_ITEM* connItem : subgraph->GetItems() )
                {
                    if( connItem->Type() == SCH_PIN_T )
                    {
                        SCH_PIN* pin = static_cast<SCH_PIN*>( connItem );
                        // SCH_PIN::GetParentSymbol() returns the SYMBOL base class
                        // (could be LIB_SYMBOL preview or SCH_SYMBOL); we only care
                        // about placed schematic symbols here.
                        SCH_SYMBOL* parentSym = dynamic_cast<SCH_SYMBOL*>( pin->GetParentSymbol() );

                        if( parentSym )
                        {
                            std::string pinRef = parentSym->GetRef(
                                &schematic.CurrentSheet() ).ToStdString()
                                + ":" + pin->GetNumber().ToStdString();
                            net.connected_pins.push_back( pinRef );
                        }
                    }
                }
            }

            ctx.nets.push_back( net );
        }
    }

    // Bounding box
    if( bboxInit )
    {
        ctx.bounding_box_min = bbox.GetOrigin();
        ctx.bounding_box_max = bbox.GetEnd();
    }

    // Compute next free position (to the right of existing content, grid-aligned)
    int gridNm = 2540000;  // 2.54mm = 100mil grid

    if( bboxInit )
    {
        int freeX = bbox.GetRight() + gridNm * 10;  // 10 grid units right of content
        int freeY = bbox.GetOrigin().y;
        // Snap to grid
        freeX = ( freeX / gridNm ) * gridNm;
        freeY = ( freeY / gridNm ) * gridNm;
        ctx.free_position = VECTOR2I( freeX, freeY );
    }
    else
    {
        // Empty schematic — start near center
        ctx.free_position = VECTOR2I( gridNm * 40, gridNm * 30 );
    }

    // Current selection
    SELECTION& selection = m_frame->GetCurrentSelection();

    for( EDA_ITEM* item : selection )
    {
        if( item->Type() == SCH_SYMBOL_T )
        {
            SCH_SYMBOL* sym = static_cast<SCH_SYMBOL*>( item );
            ctx.selected_refs.push_back(
                sym->GetRef( &schematic.CurrentSheet() ).ToStdString() );
        }
    }

    return ctx;
}


// ═══════════════════════════════════════════════════════════════════════════
//  Action Execution (Phase 2f)
// ═══════════════════════════════════════════════════════════════════════════

void COPPER_CHAT_PANEL::ExecuteOperations(
        const std::vector<COPPER::Operation>& aOperations )
{
    if( !m_frame )
        return;

    SCH_SCREEN* screen = m_frame->Schematic().RootScreen();

    if( !screen )
        return;

    if( aOperations.empty() )
        return;

    // ── Fail-closed validation (ADR-004 + PROTOCOL.md hard-rejects) ──
    // Match the Python validators in copper_integration/validators.py:
    //  - known op.type
    //  - PLACE_COMPONENT: non-empty lib_id with ':', non-empty reference,
    //    coords in [-1e9, 1e9], rotation in {0,90,180,270}, unique refs.
    //  - ADD_WIRE: non-zero length, coords in range.
    //  - ADD_LABEL: non-empty name <=64 chars, known label_type.
    //  - ADD_POWER_SYMBOL: non-empty net_name, coords in range.
    constexpr long long kCoordMin = -1000000000LL;
    constexpr long long kCoordMax =  1000000000LL;
    std::set<std::string> seenRefs;

    auto coordOk = []( const nlohmann::json& j, const char* k )
    {
        if( !j.contains( k ) || !j[k].is_number_integer() )
            return false;
        long long v = j[k].get<long long>();
        return v >= kCoordMin && v <= kCoordMax;
    };

    for( const auto& op : aOperations )
    {
        if( op.type == "PLACE_COMPONENT" )
        {
            const std::string libId = op.data.value( "lib_id", "" );
            const std::string ref = op.data.value( "reference", "" );

            if( libId.empty() || libId.find( ':' ) == std::string::npos )
            {
                addAIMessage( wxString::Format(
                    wxT( "Plan rejected: PLACE_COMPONENT missing/invalid lib_id (got %s). "
                         "Nothing applied." ),
                    wxString::FromUTF8( libId ) ) );
                return;
            }

            if( ref.empty() )
            {
                addAIMessage( wxT( "Plan rejected: PLACE_COMPONENT missing reference. "
                                   "Nothing applied." ) );
                return;
            }

            if( !seenRefs.insert( ref ).second )
            {
                addAIMessage( wxString::Format(
                    wxT( "Plan rejected: duplicate reference %s. Nothing applied." ),
                    wxString::FromUTF8( ref ) ) );
                return;
            }

            if( !coordOk( op.data, "x" ) || !coordOk( op.data, "y" ) )
            {
                addAIMessage( wxT( "Plan rejected: PLACE_COMPONENT coordinates "
                                   "out of range. Nothing applied." ) );
                return;
            }
        }
        else if( op.type == "ADD_WIRE" )
        {
            if( !coordOk( op.data, "start_x" ) || !coordOk( op.data, "start_y" )
                || !coordOk( op.data, "end_x" ) || !coordOk( op.data, "end_y" ) )
            {
                addAIMessage( wxT( "Plan rejected: ADD_WIRE bad coordinates. "
                                   "Nothing applied." ) );
                return;
            }

            const int sx = op.data.value( "start_x", 0 );
            const int sy = op.data.value( "start_y", 0 );
            const int ex = op.data.value( "end_x", 0 );
            const int ey = op.data.value( "end_y", 0 );

            if( sx == ex && sy == ey )
            {
                addAIMessage( wxT( "Plan rejected: zero-length wire. Nothing applied." ) );
                return;
            }
        }
        else if( op.type == "ADD_LABEL" )
        {
            const std::string name = op.data.value( "name", "" );
            const std::string kind = op.data.value( "label_type", "local" );

            if( name.empty() || name.size() > 64 )
            {
                addAIMessage( wxT( "Plan rejected: ADD_LABEL bad name. Nothing applied." ) );
                return;
            }

            if( kind != "local" && kind != "global" && kind != "hierarchical" )
            {
                addAIMessage( wxString::Format(
                    wxT( "Plan rejected: unknown label_type '%s'. Nothing applied." ),
                    wxString::FromUTF8( kind ) ) );
                return;
            }

            if( !coordOk( op.data, "x" ) || !coordOk( op.data, "y" ) )
            {
                addAIMessage( wxT( "Plan rejected: ADD_LABEL bad coords. Nothing applied." ) );
                return;
            }
        }
        else if( op.type == "ADD_JUNCTION" )
        {
            if( !coordOk( op.data, "x" ) || !coordOk( op.data, "y" ) )
            {
                addAIMessage( wxT( "Plan rejected: ADD_JUNCTION bad coords. "
                                   "Nothing applied." ) );
                return;
            }
        }
        else if( op.type == "ADD_POWER_SYMBOL" )
        {
            if( op.data.value( "net_name", std::string() ).empty() )
            {
                addAIMessage( wxT( "Plan rejected: ADD_POWER_SYMBOL empty net_name. "
                                   "Nothing applied." ) );
                return;
            }

            if( !coordOk( op.data, "x" ) || !coordOk( op.data, "y" ) )
            {
                addAIMessage( wxT( "Plan rejected: ADD_POWER_SYMBOL bad coords. "
                                   "Nothing applied." ) );
                return;
            }
        }
        else
        {
            addAIMessage( wxString::Format(
                wxT( "Plan rejected: unknown op type '%s'. Nothing applied." ),
                wxString::FromUTF8( op.type ) ) );
            return;
        }
    }

    // Create a commit for undo support
    SCH_COMMIT commit( m_frame );

    for( const auto& op : aOperations )
    {
        if( op.type == "PLACE_COMPONENT" )
        {
            // Extract data
            std::string libIdStr = op.data.value( "lib_id", "" );
            std::string ref = op.data.value( "reference", "" );
            std::string val = op.data.value( "value", "" );
            int posX = op.data.value( "x", 0 );
            int posY = op.data.value( "y", 0 );

            LIB_ID libId;
            libId.Parse( wxString::FromUTF8( libIdStr ) );

            // Load the symbol from library — if it's missing we cannot
            // safely partial-apply (ADR-004). Discard the commit and surface.
            LIB_SYMBOL* libSym = m_frame->GetLibSymbol( libId );

            if( !libSym )
            {
                // SCH_COMMIT destructor will clean up any items we added so
                // far. We never pushed, so the schematic is unchanged.
                addAIMessage( wxString::Format(
                    wxT( "Symbol not found in libraries: %s. "
                         "Nothing applied." ),
                    wxString::FromUTF8( libIdStr ) ) );
                return;
            }

            SCH_SYMBOL* symbol = new SCH_SYMBOL( *libSym, libId,
                                                  &m_frame->Schematic().CurrentSheet(),
                                                  0 );  // unit
            symbol->SetPosition( VECTOR2I( posX, posY ) );

            if( !ref.empty() )
                symbol->SetRef( &m_frame->Schematic().CurrentSheet(),
                               wxString::FromUTF8( ref ) );

            if( !val.empty() )
                symbol->GetField( FIELD_T::VALUE )->SetText( wxString::FromUTF8( val ) );

            commit.Add( symbol, screen );
        }
        else if( op.type == "ADD_WIRE" )
        {
            int startX = op.data.value( "start_x", 0 );
            int startY = op.data.value( "start_y", 0 );
            int endX = op.data.value( "end_x", 0 );
            int endY = op.data.value( "end_y", 0 );

            SCH_LINE* wire = new SCH_LINE( VECTOR2I( startX, startY ), LAYER_WIRE );
            wire->SetEndPoint( VECTOR2I( endX, endY ) );
            commit.Add( wire, screen );
        }
        else if( op.type == "ADD_LABEL" )
        {
            std::string name = op.data.value( "name", "" );
            int posX = op.data.value( "x", 0 );
            int posY = op.data.value( "y", 0 );
            std::string labelType = op.data.value( "label_type", "local" );

            SCH_LABEL_BASE* label = nullptr;

            if( labelType == "global" )
                label = new SCH_GLOBALLABEL( VECTOR2I( posX, posY ), wxString::FromUTF8( name ) );
            else if( labelType == "hierarchical" )
                label = new SCH_HIERLABEL( VECTOR2I( posX, posY ), wxString::FromUTF8( name ) );
            else
                label = new SCH_LABEL( VECTOR2I( posX, posY ), wxString::FromUTF8( name ) );

            commit.Add( label, screen );
        }
        else if( op.type == "ADD_JUNCTION" )
        {
            int posX = op.data.value( "x", 0 );
            int posY = op.data.value( "y", 0 );

            SCH_JUNCTION* junction = new SCH_JUNCTION( VECTOR2I( posX, posY ) );
            commit.Add( junction, screen );
        }
        else if( op.type == "ADD_POWER_SYMBOL" )
        {
            std::string netName = op.data.value( "net_name", "" );
            int posX = op.data.value( "x", 0 );
            int posY = op.data.value( "y", 0 );

            // Power symbols are SCH_SYMBOLs from the power library
            LIB_ID libId;
            libId.Parse( wxString::Format( wxT( "power:%s" ),
                                           wxString::FromUTF8( netName ) ) );

            LIB_SYMBOL* libSym = m_frame->GetLibSymbol( libId );

            if( libSym )
            {
                SCH_SYMBOL* symbol = new SCH_SYMBOL( *libSym, libId,
                                                      &m_frame->Schematic().CurrentSheet(),
                                                      0 );
                symbol->SetPosition( VECTOR2I( posX, posY ) );
                commit.Add( symbol, screen );
            }
        }
    }

    // Push the commit (creates undo entry)
    commit.Push( _( "Copper AI: Execute plan" ) );

    // Refresh the canvas
    m_frame->GetCanvas()->Refresh();
    m_frame->RefreshNetNavigator();
    m_frame->RecalculateConnections( nullptr, NO_CLEANUP );
}


void COPPER_CHAT_PANEL::OnSchematicChanged()
{
    // Called when the schematic is modified externally.
    // Could update context display or clear stale state.
}
