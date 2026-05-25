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

#ifndef KICAD_COPPER_AUTH_H
#define KICAD_COPPER_AUTH_H

#include <memory>
#include <string>
#include <wx/event.h>

#include <oauth/oauth_session.h>
#include <oauth/oauth_loopback_server.h>
#include <oauth/secure_token_store.h>

wxDECLARE_EVENT( COPPER_EVT_AUTH_SUCCESS, wxCommandEvent );
wxDECLARE_EVENT( COPPER_EVT_AUTH_FAILURE, wxCommandEvent );

class wxEvtHandler;

namespace COPPER
{

/**
 * Handles OAuth browser login flow for Copper authentication.
 *
 * Wraps KiCad's existing OAUTH_SESSION, OAUTH_LOOPBACK_SERVER, and
 * SECURE_TOKEN_STORE infrastructure. Tokens are persisted in the OS keychain
 * via SECURE_TOKEN_STORE with provider "copper".
 *
 * Flow:
 * 1. Builds authorization URL via OAUTH_SESSION (with PKCE)
 * 2. Opens default browser to the authorization URL
 * 3. Receives callback via OAUTH_LOOPBACK_SERVER
 * 4. Exchanges auth code for access + refresh tokens
 * 5. Stores tokens in OS keychain and fires COPPER_EVT_AUTH_SUCCESS
 */
class AUTH : public wxEvtHandler
{
public:
    AUTH( const std::string& aApiUrl = "https://api.copper.dev" );
    ~AUTH() override;

    /// Start the OAuth login flow (opens browser)
    void StartLogin( wxEvtHandler* aEventSink );

    /// Cancel an in-progress login
    void CancelLogin();

    /// Load tokens from OS keychain (call on startup)
    bool LoadSavedTokens();

    /// Refresh the access token using the refresh token
    bool RefreshToken();

    /// Logout: clear tokens from memory and keychain
    void Logout();

    /// Check if we have a valid (non-expired) access token
    bool IsAuthenticated() const;

    /// Get the current access token (may be empty)
    std::string GetAccessToken() const;

    /// Get the refresh token for persistence
    std::string GetRefreshToken() const;

    /// Get the user's email (extracted from /me endpoint)
    std::string GetUserEmail() const { return m_userEmail; }

    /// Get the API base URL
    std::string GetApiUrl() const { return m_apiUrl; }

    /// Set the API base URL
    void SetApiUrl( const std::string& aUrl ) { m_apiUrl = aUrl; }

private:
    /// Handle the loopback server result (auth code received or error)
    void onLoopbackResult( wxCommandEvent& aEvent );

    /// Exchange authorization code for tokens via POST to token endpoint
    bool exchangeCodeForTokens( const std::string& aCode );

    /// Store current tokens to OS keychain
    void saveTokens();

    /// Fetch user profile info from /auth/me
    void fetchUserProfile();

    std::string    m_apiUrl;
    std::string    m_userEmail;
    wxEvtHandler*  m_eventSink = nullptr;

    OAUTH_SESSION                          m_session;
    std::unique_ptr<OAUTH_LOOPBACK_SERVER> m_loopbackServer;
    SECURE_TOKEN_STORE                     m_tokenStore;
    OAUTH_TOKEN_SET                        m_tokens;

    static const wxString PROVIDER_ID;
    static const wxString ACCOUNT_ID;
    static const wxString CLIENT_ID;
    static const wxString CALLBACK_PATH;
};

}  // namespace COPPER

#endif // KICAD_COPPER_AUTH_H
