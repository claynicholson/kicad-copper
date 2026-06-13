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
#include <widgets/copper_placement.h>
#include <copper/copper_auth.h>
#include <copper/copper_client.h>
#include <copper/copper_types.h>
#include <sch_edit_frame.h>
#include <schematic.h>
#include <sch_screen.h>
#include <sch_symbol.h>
#include <sch_pin.h>
#include <sch_line.h>
#include <sch_label.h>
#include <sch_junction.h>
#include <sch_no_connect.h>
#include <sch_shape.h>
#include <sch_text.h>
#include <sch_bus_entry.h>
#include <sch_sheet.h>
#include <sch_field.h>
#include <eeschema_settings.h>
#include <connection_graph.h>
#include <sch_commit.h>
#include <sch_marker.h>
#include <erc/erc.h>
#include <erc/erc_settings.h>
#include <kiway.h>
#include <project_sch.h>
#include <libraries/symbol_library_adapter.h>
#include <magic_enum.hpp>
#include <tool/tool_manager.h>
#include <tool/actions.h>

#include <wx/dcbuffer.h>
#include <wx/stattext.h>
#include <wx/tokenzr.h>
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
        m_designForm( nullptr ),
        m_thinking( nullptr ),
        m_inputPanel( nullptr ),
        m_modeChoice( nullptr ),
        m_inputText( nullptr ),
        m_sendBtn( nullptr ),
        m_emptyStateVisible( true ),
        m_planCardShown( false )
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

    // Debug mode (COPPER_DEBUG=1): badge the header and trace the resolved
    // endpoint so transport failures can be diagnosed from the log.
    if( COPPER::DebugEnabled() )
    {
        m_titleLabel->SetLabel( wxT( "Copper AI [debug]" ) );
        m_titleLabel->SetToolTip( wxString::Format(
                wxT( "API: %s\nLog: %s" ),
                wxString::FromUTF8( apiUrl ),
                wxString::FromUTF8( COPPER::DebugLogPath() ) ) );
        m_headerPanel->Layout();

        COPPER::DebugLog( "panel init: api_url=" + apiUrl
                          + " authenticated=" + ( m_auth->IsAuthenticated() ? "yes" : "no" ) );
    }

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
    modes.Add( wxT( "From Scratch" ) );

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

    // Structured intake — preferred entry point for brand-new boards
    wxButton* scratchBtn = new wxButton( m_emptyStatePanel, wxID_ANY,
                                         wxT( "Design from scratch \xe2\x86\x92" ) );  // →
    scratchBtn->SetBackgroundColour( COPPER_COLORS::ACCENT );
    scratchBtn->SetForegroundColour( *wxWHITE );
    scratchBtn->Bind( wxEVT_BUTTON,
                      [this]( wxCommandEvent& ) { showDesignForm(); } );
    emptySizer->Add( scratchBtn, 0, wxALIGN_CENTER | wxBOTTOM, FromDIP( 16 ) );

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
    else if( mode == wxT( "From Scratch" ) )
    {
        m_inputText->SetHint( wxT( "Fill out the form above, then Create Board" ) );
        showDesignForm();
    }
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

    // ExecuteOperations reports its own outcome (success message or a
    // fail-closed rejection) — don't claim success here.
    ExecuteOperations( ops );

    if( auto* card = dynamic_cast<COPPER_PLAN_CARD*>( aEvent.GetEventObject() ) )
        card->DisableActions();
}


void COPPER_CHAT_PANEL::onPlanDismissed( wxCommandEvent& aEvent )
{
    m_pendingOps.clear();
    m_pendingSummary.reset();
    addAIMessage( wxT( "Plan dismissed. Nothing was applied." ) );
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
    if( COPPER::DebugEnabled() )
        COPPER::DebugLog( "ai-msg: " + std::string( aText.ToUTF8() ) );

    clearEmptyState();

    auto* bubble = new COPPER_MESSAGE_BUBBLE( m_scrollArea, aText,
                                              COPPER_MESSAGE_BUBBLE::Sender::AI );
    m_messageSizer->Add( bubble, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP,
                         FromDIP( 8 ) );

    m_conversationHistory.push_back( wxT( "ai: " ) + aText );
    bumpThinkingToBottom();
    scrollToBottom();
}


void COPPER_CHAT_PANEL::addLogMessage( const wxString& aText )
{
    if( COPPER::DebugEnabled() )
        COPPER::DebugLog( "log: " + std::string( aText.ToUTF8() ) );

    clearEmptyState();

    auto* line = new wxStaticText( m_scrollArea, wxID_ANY, wxT( "\u00B7 " ) + aText );
    wxFont font = line->GetFont();
    font.SetFamily( wxFONTFAMILY_TELETYPE );
    font.SetPointSize( std::max( 7, font.GetPointSize() - 1 ) );
    line->SetFont( font );
    line->SetForegroundColour( wxColour( 140, 140, 140 ) );

    m_messageSizer->Add( line, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP( 14 ) );

    bumpThinkingToBottom();
    scrollToBottom();

    // Apply + ERC run synchronously on the UI thread — force an immediate
    // repaint so the log actually streams instead of appearing all at once.
    m_scrollArea->Update();
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
    card->Bind( COPPER_EVT_PLAN_DISMISSED, &COPPER_CHAT_PANEL::onPlanDismissed, this );

    m_messageSizer->Add( card, 0, wxEXPAND | wxALL, FromDIP( 8 ) );
    bumpThinkingToBottom();
    scrollToBottom();
}


void COPPER_CHAT_PANEL::addDesignSummaryCard( const COPPER::DesignSummary& aSummary )
{
    clearEmptyState();

    auto joinRefs = []( const std::vector<std::string>& refs ) -> wxString
    {
        wxString out;

        for( size_t i = 0; i < refs.size(); ++i )
        {
            if( i )
                out += wxT( ", " );

            out += wxString::FromUTF8( refs[i] );
        }

        return out;
    };

    COPPER_DESIGN_SUMMARY_CARD::Data data;
    data.boardName = wxString::FromUTF8( aSummary.board_name );
    data.boardDescription = wxString::FromUTF8( aSummary.board_description );
    data.overview = wxString::FromUTF8( aSummary.overview );
    data.notes = wxString::FromUTF8( aSummary.notes );

    for( const auto& s : aSummary.sections )
    {
        COPPER_DESIGN_SUMMARY_CARD::Section sec;
        sec.group = wxString::FromUTF8( s.group );
        sec.purpose = wxString::FromUTF8( s.purpose );
        sec.references = joinRefs( s.references );
        data.sections.push_back( sec );
    }

    for( const auto& p : aSummary.power )
    {
        COPPER_DESIGN_SUMMARY_CARD::PowerRail rail;
        rail.rail = wxString::FromUTF8( p.rail );
        rail.voltage = wxString::FromUTF8( p.voltage );
        rail.source = wxString::FromUTF8( p.source );
        rail.estCurrent = wxString::FromUTF8( p.est_current );
        data.power.push_back( rail );
    }

    for( const auto& b : aSummary.bom )
    {
        COPPER_DESIGN_SUMMARY_CARD::BomRow row;
        row.references = joinRefs( b.references );
        row.quantity = b.quantity;
        row.value = wxString::FromUTF8( b.value );
        row.libId = wxString::FromUTF8( b.lib_id );
        row.footprint = wxString::FromUTF8( b.footprint );
        data.bom.push_back( row );
    }

    const COPPER::DesignStats& st = aSummary.stats;
    data.stats = wxString::Format(
            wxT( "%d parts (%d unique) · %d nets · %d no-connects" ),
            st.parts, st.unique_parts, st.nets, st.no_connects );

    auto* card = new COPPER_DESIGN_SUMMARY_CARD( m_scrollArea, data );
    m_messageSizer->Add( card, 0, wxEXPAND | wxALL, FromDIP( 8 ) );
    bumpThinkingToBottom();
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
    bumpThinkingToBottom();
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


void COPPER_CHAT_PANEL::bumpThinkingToBottom()
{
    // Mid-stream items (messages, stages, plan cards) are appended after the
    // indicator — move it back to the end so the sweep always reads as
    // "still working" at the bottom of the conversation.
    if( !m_thinking )
        return;

    m_messageSizer->Detach( m_thinking );
    m_messageSizer->Add( m_thinking, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxBOTTOM,
                         FromDIP( 8 ) );
}


void COPPER_CHAT_PANEL::showThinking()
{
    if( m_thinking )
        return;

    clearEmptyState();

    m_thinking = new COPPER_THINKING_INDICATOR( m_scrollArea );
    m_messageSizer->Add( m_thinking, 0, wxALIGN_LEFT | wxLEFT | wxRIGHT | wxBOTTOM,
                         FromDIP( 8 ) );
    scrollToBottom();
}


void COPPER_CHAT_PANEL::hideThinking()
{
    if( !m_thinking )
        return;

    m_messageSizer->Detach( m_thinking );
    m_thinking->Destroy();
    m_thinking = nullptr;

    m_scrollArea->FitInside();
    m_scrollArea->Layout();
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


void COPPER_CHAT_PANEL::showDesignForm()
{
    if( m_designForm )
    {
        scrollToBottom();
        return;
    }

    clearEmptyState();

    m_designForm = new COPPER_DESIGN_FORM( m_scrollArea );
    m_designForm->Bind( COPPER_EVT_DESIGN_FORM_SUBMITTED,
                        &COPPER_CHAT_PANEL::onDesignFormSubmitted, this );
    m_designForm->Bind( COPPER_EVT_DESIGN_FORM_CANCELLED,
                        &COPPER_CHAT_PANEL::onDesignFormCancelled, this );

    m_messageSizer->Add( m_designForm, 0, wxEXPAND | wxALL, FromDIP( 8 ) );
    scrollToBottom();
}


void COPPER_CHAT_PANEL::destroyDesignForm()
{
    if( !m_designForm )
        return;

    m_messageSizer->Detach( m_designForm );

    // Deletion must be deferred: this runs from the form's own button events.
    m_designForm->Hide();
    wxWindow* form = m_designForm;
    m_designForm = nullptr;
    CallAfter( [form]() { form->Destroy(); } );

    m_scrollArea->FitInside();
    m_scrollArea->Layout();
}


void COPPER_CHAT_PANEL::onDesignFormSubmitted( wxCommandEvent& aEvent )
{
    wxString prompt = m_designForm->BuildPrompt();
    destroyDesignForm();

    // The composed prompt goes through the streaming design pipeline.
    m_modeChoice->SetSelection( 0 );  // back to Design
    m_inputText->SetHint( wxT( "What would you like to build?" ) );
    sendRequest( prompt );
}


void COPPER_CHAT_PANEL::onDesignFormCancelled( wxCommandEvent& aEvent )
{
    destroyDesignForm();

    m_modeChoice->SetSelection( 0 );  // back to Design
    m_inputText->SetHint( wxT( "What would you like to build?" ) );

    // Restore the welcome screen if the conversation hasn't started.
    if( m_conversationHistory.empty() && !m_emptyStateVisible )
    {
        buildEmptyState();
        m_scrollArea->FitInside();
        m_scrollArea->Layout();
    }
}


// ═══════════════════════════════════════════════════════════════════════════
//  API Interaction
// ═══════════════════════════════════════════════════════════════════════════

void COPPER_CHAT_PANEL::sendRequest( const wxString& aPrompt )
{
    // Auth is optional — the hosted backend accepts anonymous requests, and
    // the client attaches a Bearer token when one is available.
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
    m_planCardShown = false;

    // Disable send while processing
    m_sendBtn->Disable();
    m_inputText->Disable();

    showThinking();

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

    if( mode == wxT( "Design" ) || mode == wxT( "From Scratch" ) )
    {
        // Use SSE streaming for design mode (From Scratch composes a design
        // prompt and rides the same pipeline)
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
    hideThinking();

    if( !aResponse.success )
    {
        addAIMessage( wxString::Format( wxT( "Error: %s" ),
                                        wxString::FromUTF8( aResponse.error ) ) );
        m_pendingOps.clear();
        m_pendingSummary.reset();
        return;
    }

    if( !aResponse.message.empty() )
    {
        // The stream may already have delivered the same text as a "message"
        // event — don't repeat it.
        wxString msg = wxString::FromUTF8( aResponse.message );

        if( m_conversationHistory.empty()
                || m_conversationHistory.back() != wxT( "ai: " ) + msg )
        {
            addAIMessage( msg );
        }
    }

    // Stash operations from the final response so onPlanApproved can apply them.
    // See docs/INTEGRATION_V1_AUDIT.md gap B1 / G-APPROVE.
    m_pendingOps = aResponse.operations;

    // Stash the optional design summary (PROTOCOL.md §design_summary) so the
    // read-only card renders after a successful apply. Absent on older
    // backends / the module-IR path.
    m_pendingSummary = aResponse.design_summary;

    if( m_planCardShown )
    {
        // The "plan" SSE event already rendered this request's card; the
        // "done" payload only needed to be stashed (operations above).
        return;
    }

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
        m_planCardShown = true;
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
    hideThinking();

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
                           "or unauthorized). Click Login to sign in again, or set "
                           "COPPER_API_TOKEN to a token the backend accepts." ) );
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
        wxString msg = wxString::Format(
                wxT( "Backend connection lost (%s). Check your network or the API "
                     "URL in Settings, then try again." ),
                wxString::FromUTF8( aError ) );

        if( COPPER::DebugEnabled() )
        {
            msg += wxString::Format( wxT( "\n\nAPI: %s\nDebug log: %s" ),
                                     wxString::FromUTF8( m_auth->GetApiUrl() ),
                                     wxString::FromUTF8( COPPER::DebugLogPath() ) );
        }
        else
        {
            msg += wxT( "\n\nTip: relaunch with COPPER_DEBUG=1 to trace requests "
                        "to a log file." );
        }

        addAIMessage( msg );
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

// Protocol coordinates are nanometers (ADR-006, PROTOCOL.md); eeschema
// internal units are 100 nm (SCH_IU_PER_MM = 1e4). Convert at the boundary —
// using nm as IU places everything 100x off the sheet.
constexpr int SCH_NM_PER_IU = 100;

static inline int nmToIu( long long aNm )
{
    return static_cast<int>( aNm / SCH_NM_PER_IU );
}

static inline VECTOR2I iuToNm( const VECTOR2I& aIu )
{
    return VECTOR2I( aIu.x * SCH_NM_PER_IU, aIu.y * SCH_NM_PER_IU );
}


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
            comp.position = iuToNm( sym->GetPosition() );
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

    // Bounding box (bbox is IU; the protocol wants nm)
    if( bboxInit )
    {
        ctx.bounding_box_min = iuToNm( bbox.GetOrigin() );
        ctx.bounding_box_max = iuToNm( bbox.GetEnd() );
    }

    // Compute next free position (to the right of existing content,
    // grid-aligned). Grid math in IU, converted to nm at the end.
    constexpr int gridIu = 25400;     // 2.54mm = 100mil grid, in IU
    constexpr int gridNm = 2540000;   // same grid in protocol nm

    if( bboxInit )
    {
        int freeX = bbox.GetRight() + gridIu * 10;  // 10 grid units right of content
        int freeY = bbox.GetOrigin().y;
        // Snap to grid
        freeX = ( freeX / gridIu ) * gridIu;
        freeY = ( freeY / gridIu ) * gridIu;
        ctx.free_position = iuToNm( VECTOR2I( freeX, freeY ) );
    }
    else
    {
        // Empty schematic — start near the center of an A4 sheet (nm)
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

LIB_SYMBOL* COPPER_CHAT_PANEL::resolveLibSymbol( const LIB_ID& aLibId )
{
    if( LIB_SYMBOL* sym = m_frame->GetLibSymbol( aLibId ) )
        return sym;

    // GetLibSymbol only serves libraries the async preload already loaded
    // (fetchIfLoaded). If the preload raced the global-table load (or never
    // ran), every library reports "Library not found in library table" and
    // every lookup fails. Force a synchronous load of just this library,
    // then retry.
    wxString nickname = aLibId.GetLibNickname();
    SYMBOL_LIBRARY_ADAPTER* adapter = PROJECT_SCH::SymbolLibAdapter( &m_frame->Prj() );

    if( !adapter )
        return nullptr;

    std::optional<LIB_STATUS> status = adapter->LoadOne( nickname );

    if( COPPER::DebugEnabled() )
    {
        std::string msg = "resolveLibSymbol: forced LoadOne(" + std::string( nickname.ToUTF8() ) + ") -> ";

        if( status )
        {
            msg += std::string( magic_enum::enum_name( status->load_status ) );

            if( status->error )
                msg += " error=" + std::string( status->error->message.ToUTF8() );
        }
        else
        {
            msg += "nullopt";
        }

        COPPER::DebugLog( msg );

        // One-time dump of the overall library state for diagnosis.
        static bool dumped = false;

        if( !dumped )
        {
            dumped = true;

            wxString envDir;
            wxGetEnv( wxT( "KICAD10_SYMBOL_DIR" ), &envDir );
            COPPER::DebugLog( "lib-diag: KICAD10_SYMBOL_DIR=" + std::string( envDir.ToUTF8() ) );

            int loaded = 0, errored = 0, other = 0;

            for( const auto& [nick, st] : adapter->GetLibraryStatuses() )
            {
                if( st.load_status == LOAD_STATUS::LOADED )
                    loaded++;
                else if( st.load_status == LOAD_STATUS::LOAD_ERROR )
                    errored++;
                else
                    other++;
            }

            COPPER::DebugLog( "lib-diag: statuses loaded=" + std::to_string( loaded )
                              + " error=" + std::to_string( errored )
                              + " other=" + std::to_string( other ) );
        }
    }

    LIB_SYMBOL* sym = m_frame->GetLibSymbol( aLibId );

    if( !sym && COPPER::DebugEnabled() )
        COPPER::DebugLog( "resolveLibSymbol: post-LoadOne lookup STILL null for "
                          + std::string( aLibId.Format().c_str() ) );

    return sym;
}


// Strip KiCad pin-name decorations so backend pin names match library pins:
// "~{WP}(IO2)" -> "WP", "DI(IO0)" -> "DI".
static wxString normalizePinName( const wxString& aName )
{
    wxString out = aName;
    out.Replace( wxT( "~{" ), wxEmptyString );
    out.Replace( wxT( "}" ), wxEmptyString );

    int paren = out.Find( wxT( '(' ) );

    if( paren != wxNOT_FOUND )
        out = out.Left( paren );

    return out.Trim().Trim( false );
}


// Match a backend pin token against a symbol's pins and return EVERY pad it
// resolves to. A pin NUMBER is a physical pad, so an exact-number match
// anchors exactly that one pad. A pin NAME can repeat across pads (USB-C
// "D+"/"GND"/"VBUS" each sit on two pads), so a name match returns ALL pads
// carrying that name — one label / one no-connect per pad. Resolution order:
// exact number, exact name, normalized name, '/'-separated alias segment
// ("SDA/SDI/SDO" matches "SDI"), then single-pin-any-token fallback.
static std::vector<SCH_PIN*> findSymbolPins( SCH_SYMBOL* aSymbol,
                                             const SCH_SHEET_PATH& aSheet,
                                             const wxString& aToken )
{
    std::vector<SCH_PIN*> pins = aSymbol->GetPins( &aSheet );
    std::vector<SCH_PIN*> matches;

    // Pin numbers are unique per pad: an exact hit is a single physical pad.
    for( SCH_PIN* pin : pins )
    {
        if( pin->GetNumber() == aToken )
            return { pin };
    }

    // Names can repeat: gather every pad whose name equals the token.
    for( SCH_PIN* pin : pins )
    {
        if( pin->GetName() == aToken )
            matches.push_back( pin );
    }

    if( !matches.empty() )
        return matches;

    for( SCH_PIN* pin : pins )
    {
        if( normalizePinName( pin->GetName() ) == aToken )
            matches.push_back( pin );
    }

    if( !matches.empty() )
        return matches;

    for( SCH_PIN* pin : pins )
    {
        wxStringTokenizer tok( pin->GetName(), wxT( "/" ) );

        while( tok.HasMoreTokens() )
        {
            if( normalizePinName( tok.GetNextToken() ) == aToken )
            {
                matches.push_back( pin );
                break;
            }
        }
    }

    if( !matches.empty() )
        return matches;

    // Single-pin symbols (power symbols, PWR_FLAG) are unambiguous no
    // matter what the backend calls the pin.
    if( pins.size() == 1 )
        return { pins[0] };

    return {};
}


void COPPER_CHAT_PANEL::ExecuteOperations(
        const std::vector<COPPER::Operation>& aOperations )
{
    if( COPPER::DebugEnabled() )
        COPPER::DebugLog( "ExecuteOperations: entry ops=" + std::to_string( aOperations.size() )
                          + " frame=" + std::to_string( m_frame != nullptr ) );

    if( !m_frame )
        return;

    // Use the screen the user is currently viewing — SCH_COMMIT only adds
    // items to the live VIEW when the commit screen matches the frame's
    // current screen (sch_commit.cpp pushSchEdit). Committing to RootScreen
    // while viewing another sheet silently hides every new item.
    SCH_SCREEN* screen = m_frame->GetScreen();

    if( !screen )
    {
        if( COPPER::DebugEnabled() )
            COPPER::DebugLog( "ExecuteOperations: no RootScreen, bailing" );

        return;
    }

    if( aOperations.empty() )
        return;

    // ── Fail-closed validation (ADR-004 + PROTOCOL.md hard-rejects) ──
    // Match the Python validators in copper_integration/validators.py:
    //  - known op.type
    //  - PLACE_COMPONENT: non-empty lib_id with ':', non-empty reference,
    //    coords in [-1e9, 1e9], rotation in {0,90,180,270}, unique refs,
    //    optional footprint (string; 'Lib:Name' when non-empty, missing ⇒ "").
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

            // `footprint` is optional/additive (PROTOCOL.md): missing means "".
            // When present it must be a string; when non-empty it must be a
            // KiCad footprint id 'Lib:Name' (contain ':').
            if( op.data.contains( "footprint" ) )
            {
                if( !op.data["footprint"].is_string() )
                {
                    addAIMessage( wxT( "Plan rejected: PLACE_COMPONENT footprint "
                                       "must be a string. Nothing applied." ) );
                    return;
                }

                const std::string fp = op.data["footprint"].get<std::string>();

                if( !fp.empty() && fp.find( ':' ) == std::string::npos )
                {
                    addAIMessage( wxString::Format(
                        wxT( "Plan rejected: PLACE_COMPONENT footprint must be "
                             "'Lib:Name' or empty (got %s). Nothing applied." ),
                        wxString::FromUTF8( fp ) ) );
                    return;
                }
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
        else if( op.type == "ADD_PIN_LABEL" )
        {
            const std::string style = op.data.value( "style", "global" );

            if( op.data.value( "reference", std::string() ).empty()
                || op.data.value( "pin", std::string() ).empty()
                || op.data.value( "net_name", std::string() ).empty() )
            {
                addAIMessage( wxT( "Plan rejected: ADD_PIN_LABEL missing "
                                   "reference/pin/net_name. Nothing applied." ) );
                return;
            }

            if( style != "global" && style != "power" )
            {
                addAIMessage( wxString::Format(
                    wxT( "Plan rejected: unknown ADD_PIN_LABEL style '%s'. "
                         "Nothing applied." ),
                    wxString::FromUTF8( style ) ) );
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
        else if( op.type == "ADD_NO_CONNECT" )
        {
            if( op.data.value( "reference", std::string() ).empty()
                || op.data.value( "pin", std::string() ).empty() )
            {
                addAIMessage( wxT( "Plan rejected: ADD_NO_CONNECT missing "
                                   "reference/pin. Nothing applied." ) );
                return;
            }
        }
        else if( op.type == "PLACEMENT_HINTS" )
        {
            // Advisory: semantic placement hints consumed by RefinePlacement.
            // Never schematic-mutating, so nothing to validate.
        }
        else
        {
            addAIMessage( wxString::Format(
                wxT( "Plan rejected: unknown op type '%s'. Nothing applied." ),
                wxString::FromUTF8( op.type ) ) );
            return;
        }
    }

    // ── Client-side placement refinement (PLACEMENT_HINTS) ──
    // The backend's coordinates come from auto-laid-out symbol stand-ins;
    // rewrite them here against the REAL library geometry before committing.
    std::vector<COPPER::Operation> ops = aOperations;

    addLogMessage( wxString::Format( wxT( "validate: %zu operation(s) passed "
                                          "fail-closed checks" ),
                                     aOperations.size() ) );

    std::vector<COPPER_PLACEMENT::SECTION_BOX> sections;

    int refined = COPPER_PLACEMENT::RefinePlacement( ops,
            [this]( const wxString& aLibIdStr ) -> LIB_SYMBOL*
            {
                LIB_ID id;

                if( id.Parse( aLibIdStr ) >= 0 )
                    return nullptr;

                return resolveLibSymbol( id );
            },
            &sections );

    if( COPPER::DebugEnabled() )
        COPPER::DebugLog( "ExecuteOperations: RefinePlacement rewrote "
                          + std::to_string( refined ) + " placement(s)" );

    if( refined > 0 )
        addLogMessage( wxString::Format( wxT( "placement: refined %d position(s) "
                                              "against real library geometry" ),
                                         refined ) );
    else
        addLogMessage( wxT( "placement: no hints in plan — using backend "
                            "coordinates" ) );

    {
        size_t nSym = 0, nLbl = 0, nPwr = 0, nNc = 0;

        for( const auto& op : ops )
        {
            if( op.type == "PLACE_COMPONENT" )
                nSym++;
            else if( op.type == "ADD_PIN_LABEL" )
                op.data.value( "style", "global" ) == "power" ? nPwr++ : nLbl++;
            else if( op.type == "ADD_NO_CONNECT" )
                nNc++;
        }

        addLogMessage( wxString::Format( wxT( "apply: %zu symbol(s), %zu net "
                                              "label(s), %zu power pin(s), %zu "
                                              "no-connect(s)" ),
                                         nSym, nLbl, nPwr, nNc ) );
    }

    // Create a commit for undo support
    SCH_COMMIT commit( m_frame );

    // Symbols placed by THIS plan, so ADD_PIN_LABEL ops can resolve real
    // pin positions without searching the screen.
    std::map<wxString, SCH_SYMBOL*> placedByRef;

    for( const auto& op : ops )
    {
        if( op.type == "PLACE_COMPONENT" )
        {
            // Extract data
            std::string libIdStr = op.data.value( "lib_id", "" );
            std::string ref = op.data.value( "reference", "" );
            std::string val = op.data.value( "value", "" );
            std::string footprint = op.data.value( "footprint", "" );
            int posX = nmToIu( op.data.value( "x", 0LL ) );
            int posY = nmToIu( op.data.value( "y", 0LL ) );

            LIB_ID libId;
            libId.Parse( wxString::FromUTF8( libIdStr ) );

            // Load the symbol from library — if it's missing we cannot
            // safely partial-apply (ADR-004). Discard the commit and surface.
            LIB_SYMBOL* libSym = resolveLibSymbol( libId );

            if( !libSym )
            {
                if( COPPER::DebugEnabled() )
                    COPPER::DebugLog( "ExecuteOperations: PLACE_COMPONENT symbol not found: "
                                      + libIdStr + " — discarding commit" );

                // SCH_COMMIT destructor will clean up any items we added so
                // far. We never pushed, so the schematic is unchanged.
                addAIMessage( wxString::Format(
                    wxT( "Symbol not found in libraries: %s. Nothing applied. "
                         "Check that the KiCad symbol libraries are installed "
                         "(Preferences > Manage Symbol Libraries)." ),
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

            // Assign the footprint so PCB layout works without a manual
            // assignment pass. "" (power symbols, older backends) leaves the
            // field untouched.
            if( !footprint.empty() )
                symbol->SetFootprintFieldText( wxString::FromUTF8( footprint ) );

            placedByRef[ wxString::FromUTF8( ref ) ] = symbol;
            commit.Add( symbol, screen );
        }
        else if( op.type == "ADD_PIN_LABEL" )
        {
            wxString ref = wxString::FromUTF8( op.data.value( "reference", "" ) );
            wxString pinTok = wxString::FromUTF8( op.data.value( "pin", "" ) );
            std::string netName = op.data.value( "net_name", "" );
            std::string style = op.data.value( "style", "global" );

            SCH_SHEET_PATH& sheet = m_frame->Schematic().CurrentSheet();
            SCH_SYMBOL* host = nullptr;
            auto it = placedByRef.find( ref );

            if( it != placedByRef.end() )
            {
                host = it->second;
            }
            else
            {
                // Edit flows label pins of symbols that already exist.
                for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
                {
                    SCH_SYMBOL* candidate = static_cast<SCH_SYMBOL*>( item );

                    if( candidate->GetRef( &sheet ) == ref )
                    {
                        host = candidate;
                        break;
                    }
                }
            }

            if( !host )
            {
                if( COPPER::DebugEnabled() )
                    COPPER::DebugLog( "ExecuteOperations: ADD_PIN_LABEL no symbol for ref "
                                      + std::string( ref.ToUTF8() ) );

                addAIMessage( wxString::Format(
                    wxT( "Plan failed: no placed symbol with reference %s for "
                         "net %s. Nothing applied." ),
                    ref, wxString::FromUTF8( netName ) ) );
                return;
            }

            // A pin NAME ("D+") can sit on several pads; label EVERY one so
            // no sibling pad is left dangling. A pin NUMBER resolves to a
            // single pad (see findSymbolPins).
            std::vector<SCH_PIN*> targetPins = findSymbolPins( host, sheet, pinTok );

            if( targetPins.empty() )
            {
                if( COPPER::DebugEnabled() )
                    COPPER::DebugLog( "ExecuteOperations: ADD_PIN_LABEL pin '"
                                      + std::string( pinTok.ToUTF8() ) + "' not on "
                                      + std::string( ref.ToUTF8() ) );

                addAIMessage( wxString::Format(
                    wxT( "Plan failed: pin '%s' not found on %s (net %s). "
                         "Nothing applied." ),
                    pinTok, ref, wxString::FromUTF8( netName ) ) );
                return;
            }

            for( SCH_PIN* pin : targetPins )
            {
                VECTOR2I pinPos = pin->GetPosition();

                if( style == "power" )
                {
                    LIB_ID powerId;
                    powerId.Parse( wxString::Format( wxT( "power:%s" ),
                                                     wxString::FromUTF8( netName ) ) );

                    LIB_SYMBOL* libSym = resolveLibSymbol( powerId );

                    if( !libSym )
                    {
                        addAIMessage( wxString::Format(
                            wxT( "Symbol not found in libraries: power:%s. "
                                 "Nothing applied." ),
                            wxString::FromUTF8( netName ) ) );
                        return;
                    }

                    // Route a short wire stub away from the pin and hang the
                    // power symbol off its end, so the symbol never sits on top
                    // of the host body. GND-ish nets drop below; rails rise.
                    constexpr int GRID_IU = 25400; // 2.54 mm in eeschema IU

                    VECTOR2I out( 0, 0 );

                    switch( pin->GetOrientation() )
                    {
                    case PIN_ORIENTATION::PIN_RIGHT: out = { -1, 0 }; break;
                    case PIN_ORIENTATION::PIN_LEFT:  out = { 1, 0 };  break;
                    case PIN_ORIENTATION::PIN_UP:    out = { 0, 1 };  break;
                    case PIN_ORIENTATION::PIN_DOWN:  out = { 0, -1 }; break;
                    default: break;
                    }

                    wxString netUpper = wxString::FromUTF8( netName ).Upper();
                    int ty = netUpper.Contains( wxT( "GND" ) ) ? 1 : -1;

                    VECTOR2I end = pinPos;

                    if( out.x != 0 )
                        end = pinPos + VECTOR2I( out.x * 2 * GRID_IU, 0 );
                    else if( out.y == ty )
                        end = pinPos + VECTOR2I( 0, ty * 2 * GRID_IU );

                    // (vertical pin pointing away from the net's natural
                    // direction: keep the symbol directly on the pin)

                    if( end != pinPos )
                    {
                        SCH_LINE* wire = new SCH_LINE( pinPos, LAYER_WIRE );
                        wire->SetEndPoint( end );
                        commit.Add( wire, screen );
                    }

                    SCH_SYMBOL* powerSym = new SCH_SYMBOL( *libSym, powerId, &sheet, 0 );
                    powerSym->SetPosition( end );

                    // NEVER rotate power symbols: the library defaults already
                    // encode the human convention (GND hangs down, +rails point
                    // up). The symbol's own pin sits at its origin, so the
                    // connection point stays exactly on the stub end regardless.
                    commit.Add( powerSym, screen );
                }
                else
                {
                    SCH_GLOBALLABEL* label = new SCH_GLOBALLABEL( pinPos,
                                                                  wxString::FromUTF8( netName ) );

                    // Text extends away from the symbol body.
                    switch( pin->GetOrientation() )
                    {
                    case PIN_ORIENTATION::PIN_RIGHT: label->SetSpinStyle( SPIN_STYLE::LEFT );   break;
                    case PIN_ORIENTATION::PIN_LEFT:  label->SetSpinStyle( SPIN_STYLE::RIGHT );  break;
                    case PIN_ORIENTATION::PIN_UP:    label->SetSpinStyle( SPIN_STYLE::BOTTOM ); break;
                    case PIN_ORIENTATION::PIN_DOWN:  label->SetSpinStyle( SPIN_STYLE::UP );     break;
                    default:                         label->SetSpinStyle( SPIN_STYLE::LEFT );   break;
                    }

                    commit.Add( label, screen );
                }
            }
        }
        else if( op.type == "ADD_NO_CONNECT" )
        {
            wxString ref = wxString::FromUTF8( op.data.value( "reference", "" ) );
            wxString pinTok = wxString::FromUTF8( op.data.value( "pin", "" ) );

            SCH_SHEET_PATH& sheet = m_frame->Schematic().CurrentSheet();
            SCH_SYMBOL* host = nullptr;
            auto it = placedByRef.find( ref );

            if( it != placedByRef.end() )
            {
                host = it->second;
            }
            else
            {
                for( SCH_ITEM* item : screen->Items().OfType( SCH_SYMBOL_T ) )
                {
                    SCH_SYMBOL* candidate = static_cast<SCH_SYMBOL*>( item );

                    if( candidate->GetRef( &sheet ) == ref )
                    {
                        host = candidate;
                        break;
                    }
                }
            }

            if( !host )
            {
                addAIMessage( wxString::Format(
                    wxT( "Plan failed: no placed symbol with reference %s for "
                         "no-connect. Nothing applied." ), ref ) );
                return;
            }

            // Same multi-pad semantics as ADD_PIN_LABEL: a name that maps to
            // several pads gets one no-connect per pad; a number hits one pad.
            std::vector<SCH_PIN*> targetPins = findSymbolPins( host, sheet, pinTok );

            if( targetPins.empty() )
            {
                addAIMessage( wxString::Format(
                    wxT( "Plan failed: pin '%s' not found on %s for "
                         "no-connect. Nothing applied." ), pinTok, ref ) );
                return;
            }

            for( SCH_PIN* pin : targetPins )
                commit.Add( new SCH_NO_CONNECT( pin->GetPosition() ), screen );
        }
        else if( op.type == "ADD_WIRE" )
        {
            int startX = nmToIu( op.data.value( "start_x", 0LL ) );
            int startY = nmToIu( op.data.value( "start_y", 0LL ) );
            int endX = nmToIu( op.data.value( "end_x", 0LL ) );
            int endY = nmToIu( op.data.value( "end_y", 0LL ) );

            SCH_LINE* wire = new SCH_LINE( VECTOR2I( startX, startY ), LAYER_WIRE );
            wire->SetEndPoint( VECTOR2I( endX, endY ) );
            commit.Add( wire, screen );
        }
        else if( op.type == "ADD_LABEL" )
        {
            std::string name = op.data.value( "name", "" );
            int posX = nmToIu( op.data.value( "x", 0LL ) );
            int posY = nmToIu( op.data.value( "y", 0LL ) );
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
            int posX = nmToIu( op.data.value( "x", 0LL ) );
            int posY = nmToIu( op.data.value( "y", 0LL ) );

            SCH_JUNCTION* junction = new SCH_JUNCTION( VECTOR2I( posX, posY ) );
            commit.Add( junction, screen );
        }
        else if( op.type == "ADD_POWER_SYMBOL" )
        {
            std::string netName = op.data.value( "net_name", "" );
            int posX = nmToIu( op.data.value( "x", 0LL ) );
            int posY = nmToIu( op.data.value( "y", 0LL ) );

            // Power symbols are SCH_SYMBOLs from the power library
            LIB_ID libId;
            libId.Parse( wxString::Format( wxT( "power:%s" ),
                                           wxString::FromUTF8( netName ) ) );

            LIB_SYMBOL* libSym = resolveLibSymbol( libId );

            if( !libSym )
            {
                if( COPPER::DebugEnabled() )
                    COPPER::DebugLog( "ExecuteOperations: ADD_POWER_SYMBOL not found: power:"
                                      + netName + " — discarding commit" );

                // Fail closed like PLACE_COMPONENT (ADR-004): a silently
                // skipped power symbol leaves a net floating.
                addAIMessage( wxString::Format(
                    wxT( "Symbol not found in libraries: power:%s. "
                         "Nothing applied. Check that the KiCad symbol "
                         "libraries are installed (Preferences > Manage "
                         "Symbol Libraries)." ),
                    wxString::FromUTF8( netName ) ) );
                return;
            }

            SCH_SYMBOL* symbol = new SCH_SYMBOL( *libSym, libId,
                                                  &m_frame->Schematic().CurrentSheet(),
                                                  0 );
            symbol->SetPosition( VECTOR2I( posX, posY ) );
            commit.Add( symbol, screen );
        }
    }

    // ── Section frames: dashed box + title per functional cluster ──
    if( !sections.empty() )
    {
        constexpr int GRID_IU = 25400; // 2.54 mm in eeschema IU

        for( const COPPER_PLACEMENT::SECTION_BOX& sec : sections )
        {
            VECTOR2I tl( (int) ( sec.x / 100 ), (int) ( sec.y / 100 ) );
            VECTOR2I br( (int) ( ( sec.x + sec.w ) / 100 ),
                         (int) ( ( sec.y + sec.h ) / 100 ) );

            SCH_SHAPE* box = new SCH_SHAPE( SHAPE_T::RECTANGLE );
            box->SetPosition( tl );
            box->SetEnd( br );
            box->SetStroke( STROKE_PARAMS( 0, LINE_STYLE::DASH,
                                           KIGFX::COLOR4D( 0.4, 0.4, 0.4, 1.0 ) ) );
            commit.Add( box, screen );

            SCH_TEXT* title = new SCH_TEXT( VECTOR2I( tl.x, tl.y - GRID_IU / 2 ),
                                            sec.title );
            title->SetHorizJustify( GR_TEXT_H_ALIGN_LEFT );
            title->SetVertJustify( GR_TEXT_V_ALIGN_BOTTOM );
            title->SetBold( true );
            commit.Add( title, screen );
        }

        addLogMessage( wxString::Format( wxT( "layout: %zu section frame(s) drawn" ),
                                         sections.size() ) );
    }

    if( COPPER::DebugEnabled() )
        COPPER::DebugLog( "ExecuteOperations: validation+build OK, pushing commit ("
                          + std::to_string( aOperations.size() ) + " ops)" );

    // Push the commit (creates undo entry)
    commit.Push( _( "Copper AI: Execute plan" ) );

    if( COPPER::DebugEnabled() )
        COPPER::DebugLog( "ExecuteOperations: commit pushed, refreshing canvas" );

    addLogMessage( wxT( "apply: commit pushed — rebuilding connectivity" ) );

    // Refresh the canvas and bring the new items into view — generated
    // boards are often placed outside the current viewport, which looks
    // like "nothing happened".
    m_frame->GetCanvas()->Refresh();
    m_frame->RefreshNetNavigator();
    m_frame->RecalculateConnections( nullptr, NO_CLEANUP );

    if( TOOL_MANAGER* mgr = m_frame->GetToolManager() )
        mgr->RunAction( ACTIONS::zoomFitScreen );

    // ── Post-apply ERC: check, narrate, auto-fix, repeat ──
    int remainingErrors = runErcAutoFix();

    if( remainingErrors == 0 )
    {
        addAIMessage( wxString::Format( wxT( "Applied %zu operation(s) — ERC "
                                             "clean. Press Ctrl-Z to undo." ),
                                        aOperations.size() ) );
    }
    else
    {
        addAIMessage( wxString::Format( wxT( "Applied %zu operation(s). %d ERC "
                                             "error(s) remain — see the log "
                                             "above and the markers on the "
                                             "schematic. Press Ctrl-Z to undo." ),
                                        aOperations.size(), remainingErrors ) );
    }

    // Render the optional design-summary card (read-only) once the apply
    // succeeded. Consume it so re-applies don't re-render a stale summary.
    if( m_pendingSummary.has_value() )
    {
        addDesignSummaryCard( *m_pendingSummary );
        m_pendingSummary.reset();
    }
}


int COPPER_CHAT_PANEL::runErcAutoFix()
{
    if( !m_frame )
        return 0;

    SCHEMATIC& sch = m_frame->Schematic();
    int errors = 0;
    int warnings = 0;

    constexpr int MAX_PASSES = 3;

    for( int pass = 1; pass <= MAX_PASSES; ++pass )
    {
        addLogMessage( wxString::Format( wxT( "erc: pass %d — running electrical "
                                              "rules check" ), pass ) );

        // Clear stale markers so each pass reports current truth.
        {
            SCH_SCREENS screens( sch.Root() );
            screens.DeleteAllMarkers( MARKER_BASE::MARKER_ERC, true );
        }

        ERC_TESTER tester( &sch );
        tester.RunTests( m_frame->GetCanvas()->GetView()->GetDrawingSheet(), m_frame,
                         m_frame->Kiway().KiFACE( KIWAY::FACE_CVPCB ), &m_frame->Prj(),
                         nullptr );

        errors = 0;
        warnings = 0;
        std::vector<VECTOR2I> fixablePins;

        SCH_SCREENS screens( sch.Root() );

        for( SCH_SCREEN* scr = screens.GetFirst(); scr; scr = screens.GetNext() )
        {
            for( SCH_ITEM* item : scr->Items().OfType( SCH_MARKER_T ) )
            {
                SCH_MARKER* marker = static_cast<SCH_MARKER*>( item );
                std::shared_ptr<RC_ITEM> rc = marker->GetRCItem();

                if( !rc )
                    continue;

                SEVERITY sev = sch.ErcSettings().GetSeverity( rc->GetErrorCode() );

                if( sev == RPT_SEVERITY_IGNORE )
                    continue;

                const wxChar* sevName = ( sev == RPT_SEVERITY_ERROR ) ? wxT( "error" )
                                                                      : wxT( "warning" );

                if( sev == RPT_SEVERITY_ERROR )
                    errors++;
                else
                    warnings++;

                // Name the offending items so the log is actionable —
                // "Pin not connected" forty times tells you nothing.
                wxString where;

                for( const KIID& kiid : { rc->GetMainItemID(), rc->GetAuxItemID() } )
                {
                    if( kiid == niluuid )
                        continue;

                    SCH_SHEET_PATH  itemPath;
                    SCH_ITEM*       offender = sch.ResolveItem( kiid, &itemPath, true );

                    if( !offender )
                        continue;

                    wxString desc;

                    if( offender->Type() == SCH_PIN_T )
                    {
                        SCH_PIN*    pin = static_cast<SCH_PIN*>( offender );
                        SCH_SYMBOL* parent =
                                static_cast<SCH_SYMBOL*>( pin->GetParentSymbol() );

                        desc = wxString::Format( wxT( "%s pin %s (%s)" ),
                                parent ? parent->GetRef( &itemPath ) : wxString( wxT( "?" ) ),
                                pin->GetNumber(), pin->GetName() );
                    }
                    else
                    {
                        desc = offender->GetItemDescription( m_frame, true );
                    }

                    where += where.IsEmpty() ? wxT( " — " ) : wxT( " / " );
                    where += desc;
                }

                addLogMessage( wxString::Format( wxT( "erc: [%s] %s%s" ), sevName,
                                                 rc->GetErrorMessage( true ), where ) );

                if( rc->GetErrorCode() == ERCE_PIN_NOT_CONNECTED )
                    fixablePins.push_back( marker->GetPosition() );
            }
        }

        if( errors == 0 && warnings == 0 )
        {
            addLogMessage( wxT( "erc: clean — no issues" ) );
            break;
        }

        if( fixablePins.empty() || pass == MAX_PASSES )
        {
            addLogMessage( wxString::Format( wxT( "erc: %d error(s), %d warning(s) "
                                                  "— nothing more auto-fixable" ),
                                             errors, warnings ) );
            break;
        }

        // Auto-fix: every unconnected pin gets an explicit no-connect flag.
        SCH_COMMIT fixCommit( m_frame );

        for( const VECTOR2I& pos : fixablePins )
            fixCommit.Add( new SCH_NO_CONNECT( pos ), m_frame->GetScreen() );

        fixCommit.Push( _( "Copper AI: ERC auto-fix" ) );

        addLogMessage( wxString::Format( wxT( "erc: auto-fixed %zu unconnected "
                                              "pin(s) with no-connect flags" ),
                                         fixablePins.size() ) );

        m_frame->RecalculateConnections( nullptr, NO_CLEANUP );
    }

    m_frame->GetCanvas()->Refresh();

    return errors;
}


void COPPER_CHAT_PANEL::OnSchematicChanged()
{
    // Called when the schematic is modified externally.
    // Could update context display or clear stale state.
}
