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

#ifndef KICAD_COPPER_CHAT_PANEL_H
#define KICAD_COPPER_CHAT_PANEL_H

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/scrolwin.h>
#include <wx/stattext.h>

#include <memory>
#include <vector>
#include <string>

class SCH_EDIT_FRAME;
class COPPER_STAGE_PANEL;

namespace COPPER
{
    class AUTH;
    class CLIENT;
    struct CopperResponse;
    struct SSEEvent;
    struct SchematicContext;
    struct Operation;
}


/**
 * COPPER_CHAT_PANEL — Main dockable AI chat panel for the schematic editor.
 *
 * Layout:
 *   ┌─ Header ──────────────────────┐
 *   │ Copper AI    [Login] [⚙]      │
 *   ├───────────────────────────────┤
 *   │ (scrollable message area)     │
 *   │  - message bubbles            │
 *   │  - plan cards                 │
 *   │  - stage indicators           │
 *   │  - empty state + hint chips   │
 *   ├───────────────────────────────┤
 *   │ [Mode ▼] [input...] [Send →] │
 *   └───────────────────────────────┘
 */
class COPPER_CHAT_PANEL : public wxPanel
{
public:
    COPPER_CHAT_PANEL( wxWindow* aParent, SCH_EDIT_FRAME* aFrame );
    ~COPPER_CHAT_PANEL() override;

    /// Extract current schematic context (components, nets, etc.)
    COPPER::SchematicContext ExtractContext();

    /// Execute approved plan operations on the schematic
    void ExecuteOperations( const std::vector<COPPER::Operation>& aOperations );

    /// Called by frame when schematic changes externally
    void OnSchematicChanged();

private:
    // ── UI construction ──
    void buildHeader();
    void buildMessageArea();
    void buildInputBar();
    void buildEmptyState();

    // ── Event handlers ──
    void onLoginClicked( wxCommandEvent& aEvent );
    void onSettingsClicked( wxCommandEvent& aEvent );
    void onSendClicked( wxCommandEvent& aEvent );
    void onInputKeyDown( wxKeyEvent& aEvent );
    void onModeChanged( wxCommandEvent& aEvent );
    void onHintClicked( wxCommandEvent& aEvent );
    void onPlanApproved( wxCommandEvent& aEvent );
    void onPlanEdited( wxCommandEvent& aEvent );
    void onAuthSuccess( wxCommandEvent& aEvent );
    void onAuthFailure( wxCommandEvent& aEvent );

    // ── Message handling ──
    void addUserMessage( const wxString& aText );
    void addAIMessage( const wxString& aText );
    void addPlanCard( const COPPER::CopperResponse& aResponse );
    void showStages( const std::vector<std::pair<wxString, int>>& aStages );
    void updateStage( const wxString& aName, int aState );
    void scrollToBottom();
    void clearEmptyState();

    // ── API interaction ──
    void sendRequest( const wxString& aPrompt );
    void handleResponse( const COPPER::CopperResponse& aResponse );
    void handleSSEEvent( const COPPER::SSEEvent& aEvent );
    void handleError( const std::string& aError );

    // ── UI state ──
    void updateAuthUI();
    wxString getCurrentMode() const;

    // ── Members ──
    SCH_EDIT_FRAME*                     m_frame;

    // Auth & client
    std::unique_ptr<COPPER::AUTH>       m_auth;
    std::unique_ptr<COPPER::CLIENT>     m_client;

    // Header
    wxPanel*                            m_headerPanel;
    wxStaticText*                       m_titleLabel;
    wxButton*                           m_loginBtn;
    wxStaticText*                       m_userLabel;
    wxButton*                           m_settingsBtn;

    // Message area
    wxScrolledWindow*                   m_scrollArea;
    wxBoxSizer*                         m_messageSizer;

    // Empty state
    wxPanel*                            m_emptyStatePanel;

    // Stage indicators
    COPPER_STAGE_PANEL*                 m_stagePanel;

    // Input bar
    wxPanel*                            m_inputPanel;
    wxChoice*                           m_modeChoice;
    wxTextCtrl*                         m_inputText;
    wxButton*                           m_sendBtn;

    // State
    bool                                m_emptyStateVisible;
    std::vector<wxString>               m_conversationHistory;

    // Operations awaiting the user's Approve click on a plan card.
    // Cleared after ExecuteOperations or Cancel. See docs/INTEGRATION_V1_AUDIT.md
    // gap B1 / G-APPROVE.
    std::vector<COPPER::Operation>      m_pendingOps;
};


#endif // KICAD_COPPER_CHAT_PANEL_H
