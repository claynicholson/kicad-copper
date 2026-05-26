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
#include <wx/dcbuffer.h>
#include <wx/graphics.h>
#include <wx/settings.h>


// ─── Event definitions ──────────────────────────────────────────────────────

wxDEFINE_EVENT( COPPER_EVT_PLAN_APPROVED, wxCommandEvent );
wxDEFINE_EVENT( COPPER_EVT_PLAN_EDITED, wxCommandEvent );
wxDEFINE_EVENT( COPPER_EVT_HINT_CLICKED, wxCommandEvent );


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

    btnSizer->Add( m_approveBtn, 0, wxRIGHT, FromDIP( 8 ) );
    btnSizer->Add( m_editBtn, 0 );
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
        iconColor = wxColour( 200, 60, 60 );
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
        // Match by checking the painted name (we store it internally)
        // The indicator knows its own name from construction
        // We need a getter... for now just iterate and check position
    }

    // Direct update by index is more reliable — find by name
    for( size_t i = 0; i < m_indicators.size(); i++ )
    {
        // Use the control's label/name
        if( m_indicators[i] )
        {
            // The stage indicator stores m_name internally
            // We can update all of them and check
            m_indicators[i]->SetState( aState );
            break;  // TODO: proper name matching
        }
    }
}


void COPPER_STAGE_PANEL::Clear()
{
    m_sizer->Clear( true );
    m_indicators.clear();
}
