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

#ifndef COPPER_PANEL_H
#define COPPER_PANEL_H

#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <wx/panel.h>

#include <nlohmann/json_fwd.hpp>

class SCH_EDIT_FRAME;
class wxButton;
class wxChoice;
class wxRichTextCtrl;
class wxStaticText;
class wxTextCtrl;


class COPPER_PANEL : public wxPanel
{
public:
    explicit COPPER_PANEL( SCH_EDIT_FRAME* aParent );
    ~COPPER_PANEL() override;

    void Activate();

    static const wxString PaneName() { return wxT( "CopperPanel" ); }

private:
    // UI construction
    void buildUI();

    // Event handlers
    void onSendMessage( wxCommandEvent& aEvent );
    void onInputKeyDown( wxKeyEvent& aEvent );
    void onDebugButton( wxCommandEvent& aEvent );

    // Message display
    void appendMessage( const wxString& aRole, const wxString& aText );
    void appendUserMessage( const wxString& aText );
    void appendAssistantMessage( const wxString& aText );
    void appendErrorMessage( const wxString& aText );
    void appendStatusMessage( const wxString& aText );
    void clearConversation();
    void scrollToBottom();
    void setStatus( const wxString& aText );
    void setBusy( bool aBusy );

    // Intent detection
    wxString detectIntent( const wxString& aPrompt );
    std::string getVendorFilter();

    // API calls (background thread)
    void doChat( const wxString& aPrompt );
    void doGenerate( const wxString& aPrompt );
    void doRecommend( const wxString& aPrompt );
    void doExplain();
    void doVerify();

    // Schematic context
    nlohmann::json buildProjectContext();

    // Schematic patch application
    void applySchematicPatch( const nlohmann::json& aPatch );

    // Post-placement wire fixup: read actual pin positions and re-route
    void fixupWires();

    // Library import (LCSC → KiCad via cloud API)
    bool fetchAndInstallLibrary( const std::string& aLcscPn );

    // HTTP helpers
    std::string callCloudAPI( const std::string& aEndpoint, const nlohmann::json& aBody );
    std::vector<std::pair<std::string, std::string>> parseSSE( const std::string& aRaw );

    // Config
    void loadConfig();
    void promptForApiKey();

private:
    SCH_EDIT_FRAME*    m_frame;

    // UI widgets
    wxRichTextCtrl*    m_conversation;
    wxTextCtrl*        m_input;
    wxButton*          m_sendButton;
    wxButton*          m_debugButton;
    wxChoice*          m_vendorChoice;
    wxStaticText*      m_statusText;

    // State
    std::vector<std::pair<std::string, std::string>> m_history;
    std::string        m_apiKey;
    std::string        m_cloudUrl;
    bool               m_busy;
    std::thread        m_workerThread;
};

#endif // COPPER_PANEL_H
