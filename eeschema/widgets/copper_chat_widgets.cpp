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

#include <widgets/copper_chat_widgets.h>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>
#include <wx/textctrl.h>
#include <wx/timer.h>

#include <algorithm>
#include <cmath>


// ─── Event definitions ──────────────────────────────────────────────────────

wxDEFINE_EVENT( COPPER_EVT_PLAN_APPROVED, wxCommandEvent );
wxDEFINE_EVENT( COPPER_EVT_PLAN_EDITED, wxCommandEvent );
wxDEFINE_EVENT( COPPER_EVT_PLAN_DISMISSED, wxCommandEvent );
wxDEFINE_EVENT( COPPER_EVT_HINT_CLICKED, wxCommandEvent );
wxDEFINE_EVENT( COPPER_EVT_DESIGN_FORM_SUBMITTED, wxCommandEvent );
wxDEFINE_EVENT( COPPER_EVT_DESIGN_FORM_CANCELLED, wxCommandEvent );


// ═══════════════════════════════════════════════════════════════════════════
//  COPPER_MESSAGE_BUBBLE
// ═══════════════════════════════════════════════════════════════════════════

wxBEGIN_EVENT_TABLE( COPPER_MESSAGE_BUBBLE, wxPanel )
    EVT_PAINT( COPPER_MESSAGE_BUBBLE::OnPaint )
    EVT_SIZE( COPPER_MESSAGE_BUBBLE::OnSize )
wxEND_EVENT_TABLE()


COPPER_MESSAGE_BUBBLE::COPPER_MESSAGE_BUBBLE( wxWindow* aParent, const wxString& aText,
                                              Sender aSender ) :
        wxPanel( aParent, wxID_ANY ),
        m_text( aText ),
        m_sender( aSender ),
        m_cornerRadius( 10 ),
        m_padding( 12 )
{
    SetBackgroundStyle( wxBG_STYLE_PAINT );

    // Calculate initial size from text
    wxClientDC dc( this );
    dc.SetFont( GetFont() );

    int maxWidth = aParent->GetClientSize().GetWidth() - FromDIP( 60 );
    wxSize textSize = dc.GetMultiLineTextExtent( m_text );

    if( textSize.GetWidth() > maxWidth )
        textSize.SetWidth( maxWidth );

    int totalPad = FromDIP( m_padding * 2 );
    SetMinSize( wxSize( textSize.GetWidth() + totalPad, textSize.GetHeight() + totalPad ) );
}


void COPPER_MESSAGE_BUBBLE::OnPaint( wxPaintEvent& aEvent )
{
    wxAutoBufferedPaintDC dc( this );
    wxSize size = GetClientSize();
    int pad = FromDIP( m_padding );
    int radius = FromDIP( m_cornerRadius );

    // Background (parent color)
    dc.SetBrush( wxBrush( COPPER_COLORS::BG_PRIMARY ) );
    dc.SetPen( *wxTRANSPARENT_PEN );
    dc.DrawRectangle( 0, 0, size.x, size.y );

    // Bubble
    wxColour bubbleColor = ( m_sender == Sender::USER ) ? COPPER_COLORS::USER_BUBBLE
                                                        : COPPER_COLORS::AI_BUBBLE;
    dc.SetBrush( wxBrush( bubbleColor ) );

    // Compute bubble rect based on alignment
    int maxBubbleWidth = size.x - FromDIP( 40 );
    dc.SetFont( GetFont() );

    wxArrayString lines;
    int lineHeight = dc.GetCharHeight();
    int textWidth = 0;

    // Word wrap
    wxString remaining = m_text;

    while( !remaining.IsEmpty() )
    {
        wxString line;
        wxString word;

        for( size_t i = 0; i <= remaining.length(); i++ )
        {
            if( i == remaining.length() || remaining[i] == ' ' || remaining[i] == '\n' )
            {
                wxString testLine = line.IsEmpty() ? word : line + " " + word;
                wxSize testSize = dc.GetTextExtent( testLine );

                if( testSize.GetWidth() > maxBubbleWidth - pad * 2 && !line.IsEmpty() )
                {
                    lines.Add( line );
                    textWidth = std::max( textWidth, dc.GetTextExtent( line ).GetWidth() );
                    line = word;
                }
                else
                {
                    line = testLine;
                }

                if( i < remaining.length() && remaining[i] == '\n' )
                {
                    lines.Add( line );
                    textWidth = std::max( textWidth, dc.GetTextExtent( line ).GetWidth() );
                    line.Clear();
                }

                word.Clear();
            }
            else
            {
                word += remaining[i];
            }
        }

        if( !line.IsEmpty() )
        {
            lines.Add( line );
            textWidth = std::max( textWidth, dc.GetTextExtent( line ).GetWidth() );
        }

        break;
    }

    int bubbleW = textWidth + pad * 2;
    int bubbleH = (int)lines.GetCount() * lineHeight + pad * 2;
    int bubbleX = ( m_sender == Sender::USER ) ? size.x - bubbleW - FromDIP( 8 ) : FromDIP( 8 );
    int bubbleY = FromDIP( 4 );

    dc.DrawRoundedRectangle( bubbleX, bubbleY, bubbleW, bubbleH, radius );

    // Text
    dc.SetTextForeground( COPPER_COLORS::TEXT_PRIMARY );
    int y = bubbleY + pad;

    for( const wxString& line : lines )
    {
        dc.DrawText( line, bubbleX + pad, y );
        y += lineHeight;
    }

    // Update size to fit content
    int totalHeight = bubbleH + FromDIP( 8 );

    if( GetMinSize().GetHeight() != totalHeight )
    {
        SetMinSize( wxSize( -1, totalHeight ) );
        GetParent()->Layout();
    }
}


void COPPER_MESSAGE_BUBBLE::OnSize( wxSizeEvent& aEvent )
{
    Refresh();
    aEvent.Skip();
}


// ═══════════════════════════════════════════════════════════════════════════
//  COPPER_PLAN_CARD
// ═══════════════════════════════════════════════════════════════════════════

wxBEGIN_EVENT_TABLE( COPPER_PLAN_CARD, wxPanel )
    EVT_PAINT( COPPER_PLAN_CARD::OnPaint )
wxEND_EVENT_TABLE()


COPPER_PLAN_CARD::COPPER_PLAN_CARD( wxWindow* aParent, const std::vector<PlanStep>& aSteps,
                                    const wxString& aPlacementInfo ) :
        wxPanel( aParent, wxID_ANY ),
        m_steps( aSteps ),
        m_placementInfo( aPlacementInfo )
{
    SetBackgroundStyle( wxBG_STYLE_PAINT );
    SetBackgroundColour( COPPER_COLORS::PLAN_BG );

    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    // Header
    wxStaticText* header = new wxStaticText( this, wxID_ANY, wxT( "PLAN" ) );
    header->SetForegroundColour( COPPER_COLORS::PLAN_BORDER );
    header->SetFont( header->GetFont().Bold() );
    mainSizer->Add( header, 0, wxALL, FromDIP( 8 ) );

    // Steps
    for( const auto& step : m_steps )
    {
        wxString label = wxString::Format( wxT( "%d. %s" ), step.index, step.description );
        wxStaticText* stepText = new wxStaticText( this, wxID_ANY, label );
        stepText->SetForegroundColour( COPPER_COLORS::TEXT_PRIMARY );
        stepText->Wrap( aParent->GetClientSize().GetWidth() - FromDIP( 80 ) );
        mainSizer->Add( stepText, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );
    }

    // Placement info
    if( !m_placementInfo.IsEmpty() )
    {
        wxStaticText* placementText = new wxStaticText( this, wxID_ANY, m_placementInfo );
        placementText->SetForegroundColour( COPPER_COLORS::TEXT_MUTED );
        placementText->SetFont( placementText->GetFont().Italic() );
        mainSizer->Add( placementText, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );
    }

    // Buttons
    wxBoxSizer* btnSizer = new wxBoxSizer( wxHORIZONTAL );

    m_approveBtn = new wxButton( this, wxID_ANY, wxT( "Approve" ) );
    m_approveBtn->SetBackgroundColour( COPPER_COLORS::APPROVE_BG );
    m_approveBtn->SetForegroundColour( COPPER_COLORS::TEXT_PRIMARY );
    m_approveBtn->Bind( wxEVT_BUTTON, &COPPER_PLAN_CARD::OnApprove, this );

    m_editBtn = new wxButton( this, wxID_ANY, wxT( "Edit" ) );
    m_editBtn->SetBackgroundColour( COPPER_COLORS::BG_TERTIARY );
    m_editBtn->SetForegroundColour( COPPER_COLORS::TEXT_PRIMARY );
    m_editBtn->Bind( wxEVT_BUTTON, &COPPER_PLAN_CARD::OnEdit, this );

    m_dismissBtn = new wxButton( this, wxID_ANY, wxT( "Dismiss" ) );
    m_dismissBtn->SetBackgroundColour( COPPER_COLORS::BG_TERTIARY );
    m_dismissBtn->SetForegroundColour( COPPER_COLORS::TEXT_MUTED );
    m_dismissBtn->Bind( wxEVT_BUTTON, &COPPER_PLAN_CARD::OnDismiss, this );

    btnSizer->Add( m_approveBtn, 0, wxRIGHT, FromDIP( 8 ) );
    btnSizer->Add( m_editBtn, 0, wxRIGHT, FromDIP( 8 ) );
    btnSizer->Add( m_dismissBtn, 0 );
    mainSizer->Add( btnSizer, 0, wxALL, FromDIP( 8 ) );

    SetSizer( mainSizer );
    mainSizer->Fit( this );
}


void COPPER_PLAN_CARD::OnPaint( wxPaintEvent& aEvent )
{
    wxAutoBufferedPaintDC dc( this );
    wxSize size = GetClientSize();
    int radius = FromDIP( 8 );
    int border = FromDIP( 1 );

    // Background
    dc.SetBrush( wxBrush( COPPER_COLORS::PLAN_BG ) );
    dc.SetPen( wxPen( COPPER_COLORS::PLAN_BORDER, border ) );
    dc.DrawRoundedRectangle( 0, 0, size.x, size.y, radius );
}


void COPPER_PLAN_CARD::OnApprove( wxCommandEvent& aEvent )
{
    wxCommandEvent evt( COPPER_EVT_PLAN_APPROVED );
    evt.SetEventObject( this );
    ProcessWindowEvent( evt );
}


void COPPER_PLAN_CARD::OnEdit( wxCommandEvent& aEvent )
{
    wxCommandEvent evt( COPPER_EVT_PLAN_EDITED );
    evt.SetEventObject( this );
    ProcessWindowEvent( evt );
}


void COPPER_PLAN_CARD::OnDismiss( wxCommandEvent& aEvent )
{
    DisableActions();

    wxCommandEvent evt( COPPER_EVT_PLAN_DISMISSED );
    evt.SetEventObject( this );
    ProcessWindowEvent( evt );
}


void COPPER_PLAN_CARD::DisableActions()
{
    m_approveBtn->Disable();
    m_editBtn->Disable();
    m_dismissBtn->Disable();
}


// ═══════════════════════════════════════════════════════════════════════════
//  COPPER_DESIGN_SUMMARY_CARD
// ═══════════════════════════════════════════════════════════════════════════

wxBEGIN_EVENT_TABLE( COPPER_DESIGN_SUMMARY_CARD, wxPanel )
    EVT_PAINT( COPPER_DESIGN_SUMMARY_CARD::OnPaint )
wxEND_EVENT_TABLE()


COPPER_DESIGN_SUMMARY_CARD::COPPER_DESIGN_SUMMARY_CARD( wxWindow* aParent,
                                                        const Data& aData ) :
        wxPanel( aParent, wxID_ANY )
{
    SetBackgroundStyle( wxBG_STYLE_PAINT );
    SetBackgroundColour( COPPER_COLORS::PLAN_BG );

    const int wrapWidth = aParent->GetClientSize().GetWidth() - FromDIP( 80 );

    auto addLabel = [&]( wxBoxSizer* sizer, const wxString& text,
                         const wxColour& color, bool bold, bool italic,
                         int indent ) -> wxStaticText*
    {
        wxStaticText* t = new wxStaticText( this, wxID_ANY, text );
        t->SetForegroundColour( color );

        wxFont font = t->GetFont();

        if( bold )
            font = font.Bold();

        if( italic )
            font = font.Italic();

        t->SetFont( font );

        if( wrapWidth > FromDIP( 40 ) )
            t->Wrap( wrapWidth - indent );

        sizer->Add( t, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) + indent );
        return t;
    };

    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    // Header — board name when present, else generic.
    wxString headerText = aData.boardName.IsEmpty()
                                  ? wxT( "DESIGN SUMMARY" )
                                  : wxString::Format( wxT( "DESIGN SUMMARY — %s" ),
                                                      aData.boardName );
    wxStaticText* header = new wxStaticText( this, wxID_ANY, headerText );
    header->SetForegroundColour( COPPER_COLORS::PLAN_BORDER );
    header->SetFont( header->GetFont().Bold() );
    mainSizer->Add( header, 0, wxALL, FromDIP( 8 ) );

    if( !aData.boardDescription.IsEmpty() )
        addLabel( mainSizer, aData.boardDescription, COPPER_COLORS::TEXT_SECONDARY,
                  false, true, 0 );

    // Overview.
    if( !aData.overview.IsEmpty() )
        addLabel( mainSizer, aData.overview, COPPER_COLORS::TEXT_PRIMARY,
                  false, false, 0 );

    // Sections: group → purpose → refs.
    if( !aData.sections.empty() )
    {
        addLabel( mainSizer, wxT( "Sections" ), COPPER_COLORS::ACCENT, true, false, 0 );

        for( const auto& s : aData.sections )
        {
            addLabel( mainSizer, wxString::Format( wxT( "• %s — %s" ), s.group,
                                                   s.purpose ),
                      COPPER_COLORS::TEXT_PRIMARY, false, false, FromDIP( 8 ) );

            if( !s.references.IsEmpty() )
                addLabel( mainSizer, s.references, COPPER_COLORS::TEXT_MUTED,
                          false, false, FromDIP( 20 ) );
        }
    }

    // Power tree.
    if( !aData.power.empty() )
    {
        addLabel( mainSizer, wxT( "Power" ), COPPER_COLORS::ACCENT, true, false, 0 );

        for( const auto& p : aData.power )
        {
            wxString line = wxString::Format( wxT( "• %s  %s" ), p.rail, p.voltage );

            if( !p.source.IsEmpty() )
                line += wxString::Format( wxT( "  ← %s" ), p.source );

            if( !p.estCurrent.IsEmpty() )
                line += wxString::Format( wxT( "  (%s)" ), p.estCurrent );

            addLabel( mainSizer, line, COPPER_COLORS::TEXT_PRIMARY, false, false,
                      FromDIP( 8 ) );
        }
    }

    // BOM table.
    if( !aData.bom.empty() )
    {
        addLabel( mainSizer, wxT( "Bill of materials" ), COPPER_COLORS::ACCENT,
                  true, false, 0 );

        for( const auto& b : aData.bom )
        {
            wxString line = wxString::Format( wxT( "%d× %s  [%s]" ), b.quantity,
                                              b.value, b.references );
            addLabel( mainSizer, line, COPPER_COLORS::TEXT_PRIMARY, false, false,
                      FromDIP( 8 ) );

            wxString detail;

            if( !b.libId.IsEmpty() )
                detail = b.libId;

            if( !b.footprint.IsEmpty() )
                detail += ( detail.IsEmpty() ? wxString() : wxT( "  ·  " ) )
                          + b.footprint;

            if( !detail.IsEmpty() )
                addLabel( mainSizer, detail, COPPER_COLORS::TEXT_MUTED, false,
                          false, FromDIP( 20 ) );
        }
    }

    // Stats footer.
    if( !aData.stats.IsEmpty() )
        addLabel( mainSizer, aData.stats, COPPER_COLORS::TEXT_SECONDARY, false,
                  false, 0 );

    // Notes.
    if( !aData.notes.IsEmpty() )
        addLabel( mainSizer, aData.notes, COPPER_COLORS::TEXT_MUTED, false, true,
                  0 );

    SetSizer( mainSizer );
    mainSizer->Fit( this );
}


void COPPER_DESIGN_SUMMARY_CARD::OnPaint( wxPaintEvent& aEvent )
{
    wxAutoBufferedPaintDC dc( this );
    wxSize size = GetClientSize();
    int radius = FromDIP( 8 );
    int border = FromDIP( 1 );

    dc.SetBrush( wxBrush( COPPER_COLORS::PLAN_BG ) );
    dc.SetPen( wxPen( COPPER_COLORS::PLAN_BORDER, border ) );
    dc.DrawRoundedRectangle( 0, 0, size.x, size.y, radius );
}


// ═══════════════════════════════════════════════════════════════════════════
//  COPPER_STAGE_INDICATOR
// ═══════════════════════════════════════════════════════════════════════════

wxBEGIN_EVENT_TABLE( COPPER_STAGE_INDICATOR, wxPanel )
    EVT_PAINT( COPPER_STAGE_INDICATOR::OnPaint )
wxEND_EVENT_TABLE()


COPPER_STAGE_INDICATOR::COPPER_STAGE_INDICATOR( wxWindow* aParent, const wxString& aName,
                                                State aState ) :
        wxPanel( aParent, wxID_ANY ),
        m_name( aName ),
        m_state( aState )
{
    SetBackgroundStyle( wxBG_STYLE_PAINT );
    SetMinSize( FromDIP( wxSize( -1, 24 ) ) );
}


void COPPER_STAGE_INDICATOR::SetState( State aState )
{
    m_state = aState;
    Refresh();
}


void COPPER_STAGE_INDICATOR::OnPaint( wxPaintEvent& aEvent )
{
    wxAutoBufferedPaintDC dc( this );
    wxSize size = GetClientSize();

    dc.SetBrush( wxBrush( COPPER_COLORS::BG_PRIMARY ) );
    dc.SetPen( *wxTRANSPARENT_PEN );
    dc.DrawRectangle( 0, 0, size.x, size.y );

    // Icon
    wxString icon;
    wxColour iconColor;

    switch( m_state )
    {
    case State::PENDING:
        icon = wxT( "[ ]" );
        iconColor = COPPER_COLORS::STAGE_PENDING;
        break;
    case State::ACTIVE:
        icon = wxT( "[\xe2\x96\xb6]" );   // [▶]
        iconColor = COPPER_COLORS::STAGE_ACTIVE;
        break;
    case State::COMPLETE:
        icon = wxT( "[\xe2\x9c\x93]" );   // [✓]
        iconColor = COPPER_COLORS::STAGE_COMPLETE;
        break;
    case State::FAILED:
        icon = wxT( "[\xe2\x9c\x97]" );   // [✗]
        iconColor = COPPER_COLORS::STAGE_FAILED;
        break;
    }

    dc.SetFont( GetFont() );
    dc.SetTextForeground( iconColor );
    dc.DrawText( icon, FromDIP( 8 ), FromDIP( 3 ) );

    // Name
    wxColour textColor = ( m_state == State::ACTIVE ) ? COPPER_COLORS::TEXT_PRIMARY
                                                       : COPPER_COLORS::TEXT_SECONDARY;
    dc.SetTextForeground( textColor );
    dc.DrawText( m_name, FromDIP( 36 ), FromDIP( 3 ) );
}


// ═══════════════════════════════════════════════════════════════════════════
//  COPPER_HINT_CHIP
// ═══════════════════════════════════════════════════════════════════════════

wxBEGIN_EVENT_TABLE( COPPER_HINT_CHIP, wxPanel )
    EVT_PAINT( COPPER_HINT_CHIP::OnPaint )
    EVT_ENTER_WINDOW( COPPER_HINT_CHIP::OnMouseEnter )
    EVT_LEAVE_WINDOW( COPPER_HINT_CHIP::OnMouseLeave )
    EVT_LEFT_UP( COPPER_HINT_CHIP::OnClick )
wxEND_EVENT_TABLE()


COPPER_HINT_CHIP::COPPER_HINT_CHIP( wxWindow* aParent, const wxString& aText ) :
        wxPanel( aParent, wxID_ANY ),
        m_text( aText ),
        m_hovered( false )
{
    SetBackgroundStyle( wxBG_STYLE_PAINT );
    SetCursor( wxCursor( wxCURSOR_HAND ) );

    wxClientDC dc( this );
    dc.SetFont( GetFont() );
    wxSize textSize = dc.GetTextExtent( m_text );
    SetMinSize( wxSize( textSize.GetWidth() + FromDIP( 24 ), FromDIP( 32 ) ) );
}


void COPPER_HINT_CHIP::OnPaint( wxPaintEvent& aEvent )
{
    wxAutoBufferedPaintDC dc( this );
    wxSize size = GetClientSize();
    int radius = size.y / 2;

    dc.SetBrush( wxBrush( COPPER_COLORS::BG_PRIMARY ) );
    dc.SetPen( *wxTRANSPARENT_PEN );
    dc.DrawRectangle( 0, 0, size.x, size.y );

    wxColour bg = m_hovered ? COPPER_COLORS::BG_TERTIARY : COPPER_COLORS::CHIP_BG;
    wxColour border = m_hovered ? COPPER_COLORS::ACCENT : COPPER_COLORS::CHIP_BORDER;

    dc.SetBrush( wxBrush( bg ) );
    dc.SetPen( wxPen( border, FromDIP( 1 ) ) );
    dc.DrawRoundedRectangle( 0, 0, size.x, size.y, radius );

    dc.SetFont( GetFont() );
    dc.SetTextForeground( m_hovered ? COPPER_COLORS::ACCENT : COPPER_COLORS::TEXT_SECONDARY );
    wxSize textSize = dc.GetTextExtent( m_text );
    dc.DrawText( m_text, ( size.x - textSize.x ) / 2, ( size.y - textSize.y ) / 2 );
}


void COPPER_HINT_CHIP::OnMouseEnter( wxMouseEvent& aEvent )
{
    m_hovered = true;
    Refresh();
}


void COPPER_HINT_CHIP::OnMouseLeave( wxMouseEvent& aEvent )
{
    m_hovered = false;
    Refresh();
}


void COPPER_HINT_CHIP::OnClick( wxMouseEvent& aEvent )
{
    wxCommandEvent evt( COPPER_EVT_HINT_CLICKED );
    evt.SetString( m_text );
    evt.SetEventObject( this );
    ProcessWindowEvent( evt );
}


// ═══════════════════════════════════════════════════════════════════════════
//  COPPER_THINKING_INDICATOR
// ═══════════════════════════════════════════════════════════════════════════

namespace
{
    constexpr int THINK_TIMER_MS = 33;       // ~30 fps
    constexpr int THINK_DOTS = 10;           // scanner bar length
    constexpr int THINK_PHRASE_TICKS = 90;   // ~3 s per phrase

    const wxString THINK_PHRASES[] = {
        wxT( "Thinking" ),
        wxT( "Herding electrons" ),
        wxT( "Consulting the datasheets" ),
        wxT( "Routing the rat's nest" ),
        wxT( "Plating the traces" ),
        wxT( "Negotiating with Ohm's law" ),
        wxT( "Warming up the soldering iron" ),
    };

    constexpr size_t THINK_PHRASE_COUNT = sizeof( THINK_PHRASES ) / sizeof( THINK_PHRASES[0] );

    /// Linear blend between two colours, t in [0, 1].
    wxColour blendColour( const wxColour& aFrom, const wxColour& aTo, double aT )
    {
        auto mix = []( int a, int b, double t ) { return a + (int) ( ( b - a ) * t ); };
        return wxColour( mix( aFrom.Red(),   aTo.Red(),   aT ),
                         mix( aFrom.Green(), aTo.Green(), aT ),
                         mix( aFrom.Blue(),  aTo.Blue(),  aT ) );
    }
}


wxBEGIN_EVENT_TABLE( COPPER_THINKING_INDICATOR, wxPanel )
    EVT_PAINT( COPPER_THINKING_INDICATOR::OnPaint )
wxEND_EVENT_TABLE()


COPPER_THINKING_INDICATOR::COPPER_THINKING_INDICATOR( wxWindow* aParent ) :
        wxPanel( aParent, wxID_ANY ),
        m_phase( 0 ),
        m_phraseIdx( 0 )
{
    SetBackgroundStyle( wxBG_STYLE_PAINT );

    m_timer = new wxTimer( this );
    Bind( wxEVT_TIMER, &COPPER_THINKING_INDICATOR::OnTimer, this, m_timer->GetId() );
    m_timer->Start( THINK_TIMER_MS );
}


COPPER_THINKING_INDICATOR::~COPPER_THINKING_INDICATOR()
{
    m_timer->Stop();
    delete m_timer;
}


wxSize COPPER_THINKING_INDICATOR::DoGetBestSize() const
{
    // Wide enough for the bar plus the longest phrase, so the bubble doesn't
    // resize as phrases rotate.
    wxClientDC dc( const_cast<COPPER_THINKING_INDICATOR*>( this ) );
    dc.SetFont( GetFont() );

    int maxPhrase = 0;

    for( const wxString& p : THINK_PHRASES )
        maxPhrase = std::max( maxPhrase, dc.GetTextExtent( p + wxT( "..." ) ).x );

    int barWidth = THINK_DOTS * FromDIP( 9 );
    return wxSize( FromDIP( 12 + 10 ) + barWidth + FromDIP( 10 ) + maxPhrase + FromDIP( 12 + 12 ),
                   FromDIP( 34 ) );
}


void COPPER_THINKING_INDICATOR::OnTimer( wxTimerEvent& aEvent )
{
    m_phase++;

    if( m_phase % THINK_PHRASE_TICKS == 0 )
        m_phraseIdx = ( m_phraseIdx + 1 ) % THINK_PHRASE_COUNT;

    Refresh();
}


void COPPER_THINKING_INDICATOR::OnPaint( wxPaintEvent& aEvent )
{
    wxAutoBufferedPaintDC dc( this );
    wxSize size = GetClientSize();

    // Panel background
    dc.SetBrush( wxBrush( COPPER_COLORS::BG_PRIMARY ) );
    dc.SetPen( *wxTRANSPARENT_PEN );
    dc.DrawRectangle( 0, 0, size.x, size.y );

    // AI-side bubble
    dc.SetBrush( wxBrush( COPPER_COLORS::AI_BUBBLE ) );
    dc.DrawRoundedRectangle( 0, 0, size.x, size.y, FromDIP( 10 ) );

    dc.SetFont( GetFont() );

    const int dotSpacing = FromDIP( 9 );
    const int barX = FromDIP( 12 );
    const wxString dot = wxString::FromUTF8( "\xe2\x97\x8f" );  // ● BLACK CIRCLE
    const wxSize dotExtent = dc.GetTextExtent( dot );
    const int textY = ( size.y - dotExtent.y ) / 2;

    // Scanner sweep: triangle wave bouncing across [0, THINK_DOTS-1].
    // One full left→right→left cycle every ~1.6 s at 30 fps.
    const double speed = 0.25;
    const double range = THINK_DOTS - 1;
    double t = std::fmod( m_phase * speed, 2.0 * range );
    double pos = ( t <= range ) ? t : 2.0 * range - t;

    for( int i = 0; i < THINK_DOTS; i++ )
    {
        // Gaussian-ish falloff around the sweep position gives the bright
        // head a soft comet tail instead of a hard edge.
        double dist = std::abs( i - pos );
        double intensity = std::exp( -( dist * dist ) / 2.0 );

        dc.SetTextForeground( blendColour( COPPER_COLORS::TEXT_MUTED,
                                           COPPER_COLORS::ACCENT, intensity ) );
        dc.DrawText( dot, barX + i * dotSpacing, textY );
    }

    // Cycling phrase with a typed ellipsis (., .., ...)
    int dots = ( m_phase / ( THINK_PHRASE_TICKS / 6 ) ) % 3 + 1;
    wxString phrase = THINK_PHRASES[m_phraseIdx] + wxString( '.', dots );

    dc.SetTextForeground( COPPER_COLORS::TEXT_SECONDARY );
    dc.DrawText( phrase, barX + THINK_DOTS * dotSpacing + FromDIP( 10 ), textY );
}


// ═══════════════════════════════════════════════════════════════════════════
//  COPPER_STAGE_PANEL
// ═══════════════════════════════════════════════════════════════════════════

COPPER_STAGE_PANEL::COPPER_STAGE_PANEL( wxWindow* aParent ) :
        wxPanel( aParent, wxID_ANY )
{
    SetBackgroundColour( COPPER_COLORS::BG_PRIMARY );
    m_sizer = new wxBoxSizer( wxVERTICAL );
    SetSizer( m_sizer );
}


void COPPER_STAGE_PANEL::SetStages(
        const std::vector<std::pair<wxString, COPPER_STAGE_INDICATOR::State>>& aStages )
{
    Clear();

    for( const auto& [name, state] : aStages )
    {
        auto* indicator = new COPPER_STAGE_INDICATOR( this, name, state );
        m_indicators.push_back( indicator );
        m_sizer->Add( indicator, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP( 4 ) );
    }

    Layout();
}


void COPPER_STAGE_PANEL::UpdateStage( const wxString& aName,
                                       COPPER_STAGE_INDICATOR::State aState )
{
    for( auto* indicator : m_indicators )
    {
        if( indicator && indicator->GetStageName() == aName )
        {
            indicator->SetState( aState );
            return;
        }
    }

    // Stages stream in one at a time — append indicators for new names so
    // every stage shows up, not just the ones present at panel creation.
    auto* indicator = new COPPER_STAGE_INDICATOR( this, aName, aState );
    m_indicators.push_back( indicator );
    m_sizer->Add( indicator, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP( 4 ) );
    Layout();

    if( wxWindow* parent = GetParent() )
        parent->Layout();
}


void COPPER_STAGE_PANEL::Clear()
{
    m_sizer->Clear( true );
    m_indicators.clear();
}


// ═══════════════════════════════════════════════════════════════════════════
//  COPPER_DESIGN_FORM
// ═══════════════════════════════════════════════════════════════════════════

wxBEGIN_EVENT_TABLE( COPPER_DESIGN_FORM, wxPanel )
    EVT_PAINT( COPPER_DESIGN_FORM::OnPaint )
wxEND_EVENT_TABLE()


/// Form label helper — muted, small, consistent.
static wxStaticText* designFormLabel( wxWindow* aParent, const wxString& aText )
{
    wxStaticText* label = new wxStaticText( aParent, wxID_ANY, aText );
    label->SetForegroundColour( COPPER_COLORS::TEXT_SECONDARY );
    return label;
}


COPPER_DESIGN_FORM::COPPER_DESIGN_FORM( wxWindow* aParent ) :
        wxPanel( aParent, wxID_ANY )
{
    SetBackgroundStyle( wxBG_STYLE_PAINT );
    SetBackgroundColour( COPPER_COLORS::PLAN_BG );

    wxBoxSizer* mainSizer = new wxBoxSizer( wxVERTICAL );

    wxStaticText* header = new wxStaticText( this, wxID_ANY, wxT( "DESIGN FROM SCRATCH" ) );
    header->SetForegroundColour( COPPER_COLORS::ACCENT );
    header->SetFont( header->GetFont().Bold() );
    mainSizer->Add( header, 0, wxALL, FromDIP( 8 ) );

    // Purpose (required)
    mainSizer->Add( designFormLabel( this, wxT( "What is this board for?" ) ),
                    0, wxLEFT | wxRIGHT, FromDIP( 8 ) );

    m_purpose = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                FromDIP( wxSize( -1, 56 ) ), wxTE_MULTILINE );
    m_purpose->SetBackgroundColour( COPPER_COLORS::INPUT_BG );
    m_purpose->SetForegroundColour( COPPER_COLORS::TEXT_PRIMARY );
    m_purpose->SetHint( wxT( "e.g. battery-powered soil moisture sensor for a greenhouse" ) );
    mainSizer->Add( m_purpose, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    // Supplier
    mainSizer->Add( designFormLabel( this, wxT( "Preferred parts supplier" ) ),
                    0, wxLEFT | wxRIGHT, FromDIP( 8 ) );

    wxArrayString suppliers;
    suppliers.Add( wxT( "No preference" ) );
    suppliers.Add( wxT( "JLCPCB" ) );
    suppliers.Add( wxT( "PCBWay" ) );
    suppliers.Add( wxT( "LCSC" ) );
    suppliers.Add( wxT( "Digi-Key" ) );
    suppliers.Add( wxT( "Mouser" ) );
    suppliers.Add( wxT( "Farnell" ) );

    m_supplier = new wxChoice( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, suppliers );
    m_supplier->SetSelection( 0 );
    mainSizer->Add( m_supplier, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    // Mounting technology
    mainSizer->Add( designFormLabel( this, wxT( "Component mounting" ) ),
                    0, wxLEFT | wxRIGHT, FromDIP( 8 ) );

    wxArrayString mountings;
    mountings.Add( wxT( "No preference" ) );
    mountings.Add( wxT( "All SMD" ) );
    mountings.Add( wxT( "All through-hole" ) );
    mountings.Add( wxT( "Mixed SMD + through-hole" ) );

    m_mounting = new wxChoice( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, mountings );
    m_mounting->SetSelection( 0 );
    mainSizer->Add( m_mounting, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    // Assembly method
    mainSizer->Add( designFormLabel( this, wxT( "How will it be assembled?" ) ),
                    0, wxLEFT | wxRIGHT, FromDIP( 8 ) );

    wxArrayString assemblies;
    assemblies.Add( wxT( "No preference" ) );
    assemblies.Add( wxT( "Assembly service (PCBA)" ) );
    assemblies.Add( wxT( "Hand soldering" ) );

    m_assembly = new wxChoice( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, assemblies );
    m_assembly->SetSelection( 0 );
    mainSizer->Add( m_assembly, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    // Extra notes (optional)
    mainSizer->Add( designFormLabel( this, wxT( "Anything else? (optional)" ) ),
                    0, wxLEFT | wxRIGHT, FromDIP( 8 ) );

    m_notes = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              FromDIP( wxSize( -1, 44 ) ), wxTE_MULTILINE );
    m_notes->SetBackgroundColour( COPPER_COLORS::INPUT_BG );
    m_notes->SetForegroundColour( COPPER_COLORS::TEXT_PRIMARY );
    m_notes->SetHint( wxT( "size limits, connectors, budget, supply voltage..." ) );
    mainSizer->Add( m_notes, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    // Buttons
    wxBoxSizer* btnSizer = new wxBoxSizer( wxHORIZONTAL );

    m_createBtn = new wxButton( this, wxID_ANY, wxT( "Create Board" ) );
    m_createBtn->SetBackgroundColour( COPPER_COLORS::ACCENT );
    m_createBtn->SetForegroundColour( *wxWHITE );
    m_createBtn->Bind( wxEVT_BUTTON, &COPPER_DESIGN_FORM::OnCreateClicked, this );

    m_cancelBtn = new wxButton( this, wxID_ANY, wxT( "Cancel" ) );
    m_cancelBtn->SetBackgroundColour( COPPER_COLORS::BG_TERTIARY );
    m_cancelBtn->SetForegroundColour( COPPER_COLORS::TEXT_MUTED );
    m_cancelBtn->Bind( wxEVT_BUTTON, &COPPER_DESIGN_FORM::OnCancelClicked, this );

    btnSizer->Add( m_createBtn, 0, wxRIGHT, FromDIP( 8 ) );
    btnSizer->Add( m_cancelBtn, 0 );
    mainSizer->Add( btnSizer, 0, wxALL, FromDIP( 8 ) );

    SetSizer( mainSizer );
    mainSizer->Fit( this );
}


wxString COPPER_DESIGN_FORM::BuildPrompt() const
{
    wxString prompt = wxT( "Design a new circuit board from scratch.\n" );

    prompt += wxT( "Purpose: " ) + m_purpose->GetValue().Trim().Trim( false ) + wxT( "\n" );

    if( m_supplier->GetSelection() > 0 )
    {
        prompt += wxT( "Preferred supplier: " ) + m_supplier->GetStringSelection()
                  + wxT( " — choose parts that are stocked there.\n" );
    }

    switch( m_mounting->GetSelection() )
    {
    case 1: prompt += wxT( "Mounting: use surface-mount (SMD) packages only.\n" ); break;
    case 2: prompt += wxT( "Mounting: use through-hole (THT) packages only.\n" ); break;
    case 3: prompt += wxT( "Mounting: a mix of SMD and through-hole packages is fine.\n" ); break;
    default: break;
    }

    switch( m_assembly->GetSelection() )
    {
    case 1:
        prompt += wxT( "Assembly: machine-assembled (PCBA service) — fine-pitch "
                       "packages are acceptable.\n" );
        break;
    case 2:
        prompt += wxT( "Assembly: hand-soldered — prefer large, easy-to-solder "
                       "packages (0805 or larger passives, SOIC over QFN, avoid "
                       "BGA and fine-pitch parts).\n" );
        break;
    default: break;
    }

    wxString notes = m_notes->GetValue().Trim().Trim( false );

    if( !notes.IsEmpty() )
        prompt += wxT( "Additional requirements: " ) + notes + wxT( "\n" );

    return prompt;
}


void COPPER_DESIGN_FORM::OnPaint( wxPaintEvent& aEvent )
{
    wxAutoBufferedPaintDC dc( this );
    wxSize size = GetClientSize();

    dc.SetBrush( wxBrush( COPPER_COLORS::PLAN_BG ) );
    dc.SetPen( wxPen( COPPER_COLORS::ACCENT, FromDIP( 1 ) ) );
    dc.DrawRoundedRectangle( 0, 0, size.x, size.y, FromDIP( 8 ) );
}


void COPPER_DESIGN_FORM::OnCreateClicked( wxCommandEvent& aEvent )
{
    if( m_purpose->GetValue().Trim().Trim( false ).IsEmpty() )
    {
        m_purpose->SetFocus();
        return;
    }

    wxCommandEvent evt( COPPER_EVT_DESIGN_FORM_SUBMITTED );
    evt.SetEventObject( this );
    ProcessWindowEvent( evt );
}


void COPPER_DESIGN_FORM::OnCancelClicked( wxCommandEvent& aEvent )
{
    wxCommandEvent evt( COPPER_EVT_DESIGN_FORM_CANCELLED );
    evt.SetEventObject( this );
    ProcessWindowEvent( evt );
}
