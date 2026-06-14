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
    // ── Surfaces (dark → raised) ──────────────────────────────────────────
    const wxColour BG_PRIMARY( 22, 22, 26 );        // app / message-list canvas
    const wxColour BG_SECONDARY( 28, 28, 34 );       // header / input chrome
    const wxColour BG_TERTIARY( 40, 40, 48 );        // pressed / secondary buttons
    const wxColour SURFACE_RAISED( 33, 34, 40 );     // elevated cards & AI bubbles

    // ── Message bubbles ───────────────────────────────────────────────────
    const wxColour USER_BUBBLE( 52, 53, 64 );        // user, slightly lighter surface
    const wxColour AI_BUBBLE( 33, 34, 40 );           // AI, flat raised surface

    // ── Cards (plan / summary / form) ─────────────────────────────────────
    const wxColour PLAN_BG( 30, 32, 40 );
    const wxColour PLAN_BORDER( 86, 116, 166 );       // calmer steel-blue accent

    // ── Text ──────────────────────────────────────────────────────────────
    const wxColour TEXT_PRIMARY( 226, 227, 232 );     // body
    const wxColour TEXT_SECONDARY( 158, 162, 172 );   // captions / metadata (WCAG-ish)
    const wxColour TEXT_MUTED( 120, 124, 134 );        // de-emphasized detail

    // ── Brand accent ──────────────────────────────────────────────────────
    const wxColour ACCENT( 200, 120, 60 );            // Copper brand
    const wxColour ACCENT_HOVER( 220, 140, 80 );
    const wxColour ACCENT_PRESSED( 178, 104, 50 );    // active/pressed copper
    const wxColour FOCUS_RING( 200, 120, 60 );        // input focus emphasis

    // ── Action button (approve) ───────────────────────────────────────────
    const wxColour APPROVE_BG( 42, 84, 54 );
    const wxColour APPROVE_BORDER( 64, 146, 86 );

    // ── Stage states ──────────────────────────────────────────────────────
    const wxColour STAGE_ACTIVE( 92, 166, 224 );
    const wxColour STAGE_COMPLETE( 78, 188, 118 );
    const wxColour STAGE_PENDING( 92, 96, 108 );
    const wxColour STAGE_FAILED( 214, 84, 84 );

    // ── Chips / inputs / borders ──────────────────────────────────────────
    const wxColour CHIP_BG( 38, 40, 48 );
    const wxColour CHIP_BORDER( 64, 67, 80 );
    const wxColour INPUT_BG( 33, 34, 41 );
    const wxColour INPUT_BORDER( 58, 60, 72 );
    const wxColour BORDER_SUBTLE( 48, 50, 60 );       // low-contrast 1px hairlines
    const wxColour SEPARATOR( 40, 42, 51 );
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
    int      m_lineGap;     // extra leading added to each line for breathing room

    wxDECLARE_EVENT_TABLE();
};


// ─── COPPER_PLAN_CARD ───────────────────────────────────────────────────────

wxDECLARE_EVENT( COPPER_EVT_PLAN_APPROVED, wxCommandEvent );
wxDECLARE_EVENT( COPPER_EVT_PLAN_EDITED, wxCommandEvent );
wxDECLARE_EVENT( COPPER_EVT_PLAN_DISMISSED, wxCommandEvent );

/**
 * Displays a plan from the AI with numbered steps and Approve/Edit/Dismiss
 * buttons.
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

    /// Grey out the action buttons (after the plan is applied or dismissed).
    void DisableActions();

protected:
    void OnPaint( wxPaintEvent& aEvent );
    void OnApprove( wxCommandEvent& aEvent );
    void OnEdit( wxCommandEvent& aEvent );
    void OnDismiss( wxCommandEvent& aEvent );

private:
    std::vector<PlanStep> m_steps;
    wxString              m_placementInfo;
    wxButton*             m_approveBtn;
    wxButton*             m_editBtn;
    wxButton*             m_dismissBtn;

    wxDECLARE_EVENT_TABLE();
};


// ─── COPPER_DESIGN_SUMMARY_CARD ─────────────────────────────────────────────

/**
 * A compact, read-only "design summary card" rendered after a successful
 * generate/apply. Shows the overview, per-section list (group → purpose →
 * refs), the power tree, a BOM table, and a one-line stats footer.
 *
 * The widget owns a flattened copy of the summary fields (mirrors how
 * COPPER_PLAN_CARD owns PlanStep) so this header stays free of copper_types.h.
 */
class COPPER_DESIGN_SUMMARY_CARD : public wxPanel
{
public:
    struct Section
    {
        wxString group;
        wxString purpose;
        wxString references;  // pre-joined "R1, R2, C3"
    };

    struct PowerRail
    {
        wxString rail;
        wxString voltage;
        wxString source;
        wxString estCurrent;
    };

    struct BomRow
    {
        wxString references;  // pre-joined
        int      quantity = 0;
        wxString value;
        wxString libId;
        wxString footprint;
    };

    struct Data
    {
        wxString               boardName;
        wxString               boardDescription;
        wxString               overview;
        std::vector<Section>   sections;
        std::vector<PowerRail> power;
        std::vector<BomRow>    bom;
        wxString               stats;  // pre-formatted one-liner
        wxString               notes;
    };

    COPPER_DESIGN_SUMMARY_CARD( wxWindow* aParent, const Data& aData );

protected:
    void OnPaint( wxPaintEvent& aEvent );

private:
    wxDECLARE_EVENT_TABLE();
};


// ─── COPPER_STAGE_INDICATOR ─────────────────────────────────────────────────

/**
 * Shows pipeline stage progress (Searching → Designing → Validating).
 */
class COPPER_STAGE_INDICATOR : public wxPanel
{
public:
    // NB: do NOT name an enumerator ERROR — windows.h defines it as a macro
    // (0u from wingdi.h), which pre-processes our enumerator into garbage and
    // breaks compilation on any TU that transitively includes <windows.h>.
    enum class State { PENDING, ACTIVE, COMPLETE, FAILED };

    COPPER_STAGE_INDICATOR( wxWindow* aParent, const wxString& aName, State aState = State::PENDING );

    void SetState( State aState );
    State GetState() const { return m_state; }
    // Not GetName(): that would shadow wxWindow::GetName().
    const wxString& GetStageName() const { return m_name; }

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


// ─── COPPER_DESIGN_FORM ─────────────────────────────────────────────────────

wxDECLARE_EVENT( COPPER_EVT_DESIGN_FORM_SUBMITTED, wxCommandEvent );
wxDECLARE_EVENT( COPPER_EVT_DESIGN_FORM_CANCELLED, wxCommandEvent );

class wxTextCtrl;
class wxChoice;

/**
 * "Design from scratch" intake form. Instead of free-form chat, the user
 * fills out structured fields (purpose, supplier, mounting, assembly) and
 * the form composes a constraint-rich prompt for the design pipeline.
 *
 * Fires COPPER_EVT_DESIGN_FORM_SUBMITTED (prompt available via BuildPrompt())
 * or COPPER_EVT_DESIGN_FORM_CANCELLED.
 */
class COPPER_DESIGN_FORM : public wxPanel
{
public:
    COPPER_DESIGN_FORM( wxWindow* aParent );

    /// Compose the structured design prompt from the current field values.
    wxString BuildPrompt() const;

protected:
    void OnPaint( wxPaintEvent& aEvent );
    void OnCreateClicked( wxCommandEvent& aEvent );
    void OnCancelClicked( wxCommandEvent& aEvent );

private:
    wxTextCtrl* m_purpose;
    wxChoice*   m_supplier;
    wxChoice*   m_mounting;
    wxChoice*   m_assembly;
    wxTextCtrl* m_notes;
    wxButton*   m_createBtn;
    wxButton*   m_cancelBtn;

    wxDECLARE_EVENT_TABLE();
};


// ─── COPPER_THINKING_INDICATOR ──────────────────────────────────────────────

class wxTimer;
class wxTimerEvent;

/**
 * Animated text "thinking" indicator shown while a request is in flight.
 *
 * Renders as an AI-side bubble containing a scanner bar — a row of text
 * dots with a bright copper highlight that sweeps back and forth (bounces
 * at the ends), each dot blending TEXT_MUTED → ACCENT by distance from the
 * sweep position — followed by a cycling copper-flavored status phrase.
 * Pure text, ~30 fps, no bitmaps.
 *
 * Lifetime: create + add to a sizer to start animating; Destroy() to stop
 * (the timer is owned and stops in the destructor).
 */
class COPPER_THINKING_INDICATOR : public wxPanel
{
public:
    COPPER_THINKING_INDICATOR( wxWindow* aParent );
    ~COPPER_THINKING_INDICATOR() override;

protected:
    wxSize DoGetBestSize() const override;

    void OnPaint( wxPaintEvent& aEvent );
    void OnTimer( wxTimerEvent& aEvent );

private:
    wxTimer* m_timer;
    int      m_phase;        // animation clock, one tick per timer fire
    size_t   m_phraseIdx;    // index into the phrase rotation

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
