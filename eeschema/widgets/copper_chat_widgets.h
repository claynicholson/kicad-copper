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

#ifndef KICAD_COPPER_CHAT_WIDGETS_H
#define KICAD_COPPER_CHAT_WIDGETS_H

#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/dcbuffer.h>
#include <wx/scrolwin.h>

#include <vector>
#include <string>
#include <functional>


// ─── Color constants (Copper dark theme) ────────────────────────────────────

namespace COPPER_COLORS
{
    const wxColour BG_PRIMARY( 22, 22, 26 );
    const wxColour BG_SECONDARY( 30, 30, 36 );
    const wxColour BG_TERTIARY( 40, 40, 48 );
    const wxColour USER_BUBBLE( 55, 55, 68 );
    const wxColour AI_BUBBLE( 35, 38, 45 );
    const wxColour PLAN_BG( 28, 35, 45 );
    const wxColour PLAN_BORDER( 60, 100, 160 );
    const wxColour TEXT_PRIMARY( 220, 220, 225 );
    const wxColour TEXT_SECONDARY( 150, 150, 158 );
    const wxColour TEXT_MUTED( 100, 100, 110 );
    const wxColour ACCENT( 200, 120, 60 );       // Copper brand
    const wxColour ACCENT_HOVER( 220, 140, 80 );
    const wxColour APPROVE_BG( 40, 80, 50 );
    const wxColour APPROVE_BORDER( 60, 140, 80 );
    const wxColour STAGE_ACTIVE( 80, 160, 220 );
    const wxColour STAGE_COMPLETE( 60, 180, 100 );
    const wxColour STAGE_PENDING( 80, 80, 90 );
    const wxColour CHIP_BG( 40, 42, 50 );
    const wxColour CHIP_BORDER( 70, 72, 85 );
    const wxColour INPUT_BG( 35, 35, 42 );
    const wxColour INPUT_BORDER( 60, 60, 72 );
    const wxColour SEPARATOR( 45, 45, 55 );
}


// ─── COPPER_MESSAGE_BUBBLE ──────────────────────────────────────────────────

/**
 * A rounded message bubble displaying user or AI text.
 * User messages are right-aligned with USER_BUBBLE color.
 * AI messages are left-aligned with AI_BUBBLE color.
 */
class COPPER_MESSAGE_BUBBLE : public wxPanel
{
public:
    enum class Sender { USER, AI };

    COPPER_MESSAGE_BUBBLE( wxWindow* aParent, const wxString& aText, Sender aSender );

protected:
    void OnPaint( wxPaintEvent& aEvent );
    void OnSize( wxSizeEvent& aEvent );

private:
    wxString m_text;
    Sender   m_sender;
    int      m_cornerRadius;
    int      m_padding;

    wxDECLARE_EVENT_TABLE();
};


// ─── COPPER_PLAN_CARD ───────────────────────────────────────────────────────

wxDECLARE_EVENT( COPPER_EVT_PLAN_APPROVED, wxCommandEvent );
wxDECLARE_EVENT( COPPER_EVT_PLAN_EDITED, wxCommandEvent );

/**
 * Displays a plan from the AI with numbered steps and Approve/Edit buttons.
 */
class COPPER_PLAN_CARD : public wxPanel
{
public:
    struct PlanStep
    {
        int         index;
        wxString    description;
    };

    COPPER_PLAN_CARD( wxWindow* aParent, const std::vector<PlanStep>& aSteps,
                      const wxString& aPlacementInfo );

protected:
    void OnPaint( wxPaintEvent& aEvent );
    void OnApprove( wxCommandEvent& aEvent );
    void OnEdit( wxCommandEvent& aEvent );

private:
    std::vector<PlanStep> m_steps;
    wxString              m_placementInfo;
    wxButton*             m_approveBtn;
    wxButton*             m_editBtn;

    wxDECLARE_EVENT_TABLE();
};


// ─── COPPER_STAGE_INDICATOR ─────────────────────────────────────────────────

/**
 * Shows pipeline stage progress (Searching → Designing → Validating).
 */
class COPPER_STAGE_INDICATOR : public wxPanel
{
public:
    enum class State { PENDING, ACTIVE, COMPLETE, ERROR };

    COPPER_STAGE_INDICATOR( wxWindow* aParent, const wxString& aName, State aState = State::PENDING );

    void SetState( State aState );
    State GetState() const { return m_state; }

protected:
    void OnPaint( wxPaintEvent& aEvent );

private:
    wxString m_name;
    State    m_state;

    wxDECLARE_EVENT_TABLE();
};


// ─── COPPER_HINT_CHIP ───────────────────────────────────────────────────────

wxDECLARE_EVENT( COPPER_EVT_HINT_CLICKED, wxCommandEvent );

/**
 * A pill-shaped clickable hint/example chip.
 * When clicked, fires COPPER_EVT_HINT_CLICKED with the hint text.
 */
class COPPER_HINT_CHIP : public wxPanel
{
public:
    COPPER_HINT_CHIP( wxWindow* aParent, const wxString& aText );

    wxString GetHintText() const { return m_text; }

protected:
    void OnPaint( wxPaintEvent& aEvent );
    void OnMouseEnter( wxMouseEvent& aEvent );
    void OnMouseLeave( wxMouseEvent& aEvent );
    void OnClick( wxMouseEvent& aEvent );

private:
    wxString m_text;
    bool     m_hovered;

    wxDECLARE_EVENT_TABLE();
};


// ─── COPPER_STAGE_PANEL ─────────────────────────────────────────────────────

/**
 * Container for multiple stage indicators shown during generation.
 */
class COPPER_STAGE_PANEL : public wxPanel
{
public:
    COPPER_STAGE_PANEL( wxWindow* aParent );

    void SetStages( const std::vector<std::pair<wxString, COPPER_STAGE_INDICATOR::State>>& aStages );
    void UpdateStage( const wxString& aName, COPPER_STAGE_INDICATOR::State aState );
    void Clear();

private:
    wxBoxSizer*                            m_sizer;
    std::vector<COPPER_STAGE_INDICATOR*>   m_indicators;
};


#endif // KICAD_COPPER_CHAT_WIDGETS_H
