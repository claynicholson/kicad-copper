/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
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

#include <widgets/copper_placement.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <set>
#include <string>

#include <lib_symbol.h>
#include <sch_pin.h>
#include <wx/string.h>
#include <wx/tokenzr.h>

namespace
{

// All layout math is in nanometers (the protocol's coordinate unit).
// eeschema internal units are 100 nm.
constexpr long long IU_TO_NM = 100;
constexpr long long GRID_NM = 2540000;            // 2.54 mm — the schematic grid
constexpr long long MARGIN_NM = 10 * GRID_NM;     // sheet margin
constexpr long long COL_GAP_NM = 12 * GRID_NM;    // gap between flow columns
constexpr long long CLUSTER_GAP_NM = 8 * GRID_NM; // gap between stacked clusters
constexpr long long CELL_PAD_NM = 9 * GRID_NM;    // padding around grid cells
constexpr long long SAT_PAD_NM = 2 * GRID_NM;     // collision halo around parts
constexpr long long SECTION_PAD_NM = 4 * GRID_NM; // frame padding around sections
constexpr long long DEFAULT_EXTENT_NM = 4 * GRID_NM;

long long snapNm( long long v )
{
    const long long half = GRID_NM / 2;
    long long q = ( v >= 0 ) ? ( v + half ) / GRID_NM : -( ( -v + half ) / GRID_NM );
    return q * GRID_NM;
}

struct PartGeom
{
    size_t      opIdx = 0;
    wxString    ref;
    LIB_SYMBOL* sym = nullptr;
    long long   w = 2 * DEFAULT_EXTENT_NM;
    long long   h = 2 * DEFAULT_EXTENT_NM;
    // body center offset from the symbol anchor, schematic coords (y down)
    long long   cx = 0;
    long long   cy = 0;
};

struct Cluster
{
    std::string           moduleId;
    std::string           role;
    int                   flowRank = 3;
    wxString              anchorRef;
    std::vector<wxString> refs; // placed, non-satellite members
};

struct Rect
{
    long long x1 = 0;
    long long y1 = 0;
    long long x2 = 0;
    long long y2 = 0;
};

bool rectsOverlap( const Rect& a, const Rect& b )
{
    return a.x1 < b.x2 && b.x1 < a.x2 && a.y1 < b.y2 && b.y1 < a.y2;
}

Rect rectAt( long long cx, long long cy, long long w, long long h, long long pad )
{
    return { cx - w / 2 - pad, cy - h / 2 - pad, cx + w / 2 + pad, cy + h / 2 + pad };
}

struct Attachment
{
    wxString ref;
    wxString toRef;
    wxString toPin;
};

wxString normalizeToken( const wxString& aName )
{
    wxString out = aName;
    out.Replace( wxT( "~{" ), wxEmptyString );
    out.Replace( wxT( "}" ), wxEmptyString );

    int paren = out.Find( wxT( '(' ) );

    if( paren != wxNOT_FOUND )
        out = out.Left( paren );

    return out.Trim().Trim( false );
}


// Library-side twin of the panel's findSymbolPin: match a backend pin token
// against a LIB_SYMBOL's pins (exact name/number, normalized, '/' aliases,
// single-pin fallback).
SCH_PIN* findLibPin( LIB_SYMBOL* aSym, const wxString& aToken )
{
    std::vector<SCH_PIN*> pins = aSym->GetGraphicalPins();

    for( SCH_PIN* pin : pins )
    {
        if( pin->GetName() == aToken || pin->GetNumber() == aToken )
            return pin;
    }

    for( SCH_PIN* pin : pins )
    {
        if( normalizeToken( pin->GetName() ) == aToken )
            return pin;
    }

    for( SCH_PIN* pin : pins )
    {
        wxStringTokenizer tok( pin->GetName(), wxT( "/" ) );

        while( tok.HasMoreTokens() )
        {
            if( normalizeToken( tok.GetNextToken() ) == aToken )
                return pin;
        }
    }

    if( pins.size() == 1 )
        return pins[0];

    return nullptr;
}

} // namespace


namespace COPPER_PLACEMENT
{

int RefinePlacement( std::vector<COPPER::Operation>& aOperations,
                     const std::function<LIB_SYMBOL*( const wxString& )>& aResolve,
                     std::vector<SECTION_BOX>* aSections )
{
    // ── Locate the advisory hints; without them keep backend coordinates ──
    const nlohmann::json* hints = nullptr;

    for( const auto& op : aOperations )
    {
        if( op.type == "PLACEMENT_HINTS" )
        {
            hints = &op.data;
            break;
        }
    }

    if( !hints )
        return 0;

    // ── Real geometry for every placed part ──
    std::map<wxString, PartGeom> geom;

    for( size_t i = 0; i < aOperations.size(); ++i )
    {
        const auto& op = aOperations[i];

        if( op.type != "PLACE_COMPONENT" )
            continue;

        PartGeom g;
        g.opIdx = i;
        g.ref = wxString::FromUTF8( op.data.value( "reference", "" ) );

        if( LIB_SYMBOL* sym = aResolve( wxString::FromUTF8( op.data.value( "lib_id", "" ) ) ) )
        {
            g.sym = sym;
            BOX2I bb = sym->GetBodyBoundingBox( 1, 1, true, false );

            if( bb.GetWidth() > 0 && bb.GetHeight() > 0 )
            {
                g.w = (long long) bb.GetWidth() * IU_TO_NM;
                g.h = (long long) bb.GetHeight() * IU_TO_NM;
                // Library frame is y-up; schematic placement flips y.
                g.cx = (long long) bb.GetCenter().x * IU_TO_NM;
                g.cy = -(long long) bb.GetCenter().y * IU_TO_NM;
            }
        }

        geom[g.ref] = g;
    }

    if( geom.empty() )
        return 0;

    // ── Parse clusters + attachments ──
    std::vector<Cluster> clusters;
    std::set<wxString>   clustered;

    if( hints->contains( "clusters" ) && ( *hints )["clusters"].is_array() )
    {
        for( const auto& jc : ( *hints )["clusters"] )
        {
            Cluster c;
            c.moduleId = jc.value( "module_id", "" );
            c.role = jc.value( "role", "" );
            c.flowRank = jc.value( "flow_rank", 3 );
            c.anchorRef = wxString::FromUTF8( jc.value( "anchor_ref", "" ) );

            if( jc.contains( "refs" ) && jc["refs"].is_array() )
            {
                for( const auto& jr : jc["refs"] )
                {
                    wxString r = wxString::FromUTF8( jr.get<std::string>() );

                    if( geom.count( r ) && clustered.insert( r ).second )
                        c.refs.push_back( r );
                }
            }

            if( !c.refs.empty() )
                clusters.push_back( c );
        }
    }

    // Parts no cluster claimed go in a trailing misc cluster.
    {
        Cluster misc;
        misc.moduleId = "_misc";
        misc.flowRank = 4;

        for( const auto& [ref, g] : geom )
        {
            if( !clustered.count( ref ) )
                misc.refs.push_back( ref );
        }

        if( !misc.refs.empty() )
            clusters.push_back( misc );
    }

    std::map<wxString, Attachment> satellites; // satellite ref -> attachment

    if( hints->contains( "attachments" ) && ( *hints )["attachments"].is_array() )
    {
        for( const auto& ja : ( *hints )["attachments"] )
        {
            Attachment a;
            a.ref = wxString::FromUTF8( ja.value( "ref", "" ) );
            a.toRef = wxString::FromUTF8( ja.value( "to_ref", "" ) );
            a.toPin = wxString::FromUTF8( ja.value( "to_pin", "" ) );

            if( geom.count( a.ref ) && geom.count( a.toRef ) && a.ref != a.toRef )
                satellites[a.ref] = a;
        }
    }

    // A satellite's host must not itself be a satellite (no chains).
    for( auto it = satellites.begin(); it != satellites.end(); )
    {
        if( satellites.count( it->second.toRef ) )
            it = satellites.erase( it );
        else
            ++it;
    }

    // ── Per-cluster shelf layout (anchor left, support grid right) ──
    struct Laid
    {
        std::map<wxString, std::pair<long long, long long>> rel; // ref -> center
        long long w = 0;
        long long h = 0;
    };

    std::vector<std::pair<const Cluster*, Laid>> laidClusters;

    for( const Cluster& c : clusters )
    {
        std::vector<wxString> members;

        for( const wxString& r : c.refs )
        {
            if( !satellites.count( r ) )
                members.push_back( r );
        }

        if( members.empty() )
            continue;

        // Anchor first, the rest by reference.
        std::sort( members.begin(), members.end(),
                   [&]( const wxString& a, const wxString& b )
                   {
                       bool aa = ( a == c.anchorRef );
                       bool ab = ( b == c.anchorRef );

                       if( aa != ab )
                           return aa;

                       return a.Cmp( b ) < 0;
                   } );

        Laid laid;
        const PartGeom& ag = geom[members[0]];

        size_t nOthers = members.size() - 1;

        if( nOthers == 0 )
        {
            laid.rel[members[0]] = { ag.w / 2, ag.h / 2 };
            laid.w = ag.w;
            laid.h = ag.h;
            laidClusters.push_back( { &c, laid } );
            continue;
        }

        int cols = std::max( 1, (int) std::lround( std::sqrt( (double) nOthers ) ) );
        int rows = (int) ( ( nOthers + cols - 1 ) / cols );

        std::vector<long long> colW( cols, 0 );
        std::vector<long long> rowH( rows, 0 );

        for( size_t i = 1; i < members.size(); ++i )
        {
            const PartGeom& g = geom[members[i]];
            int r = (int) ( ( i - 1 ) / cols );
            int col = (int) ( ( i - 1 ) % cols );
            colW[col] = std::max( colW[col], g.w + CELL_PAD_NM );
            rowH[r] = std::max( rowH[r], g.h + CELL_PAD_NM );
        }

        long long bandW = 0;
        std::vector<long long> colCx;

        for( long long w : colW )
        {
            colCx.push_back( bandW + w / 2 );
            bandW += w;
        }

        long long bandH = 0;
        std::vector<long long> rowCy;

        for( long long h : rowH )
        {
            rowCy.push_back( bandH + h / 2 );
            bandH += h;
        }

        laid.w = ag.w + COL_GAP_NM / 2 + bandW;
        laid.h = std::max( ag.h, bandH );

        // Anchor on the left, vertically centered; support band to its right.
        laid.rel[members[0]] = { ag.w / 2, laid.h / 2 };

        for( size_t i = 1; i < members.size(); ++i )
        {
            int r = (int) ( ( i - 1 ) / cols );
            int col = (int) ( ( i - 1 ) % cols );
            laid.rel[members[i]] = { ag.w + COL_GAP_NM / 2 + colCx[col], rowCy[r] };
        }

        laidClusters.push_back( { &c, laid } );
    }

    // ── Flow columns: rank ascending left → right, clusters stacked ──
    std::map<int, std::vector<size_t>> byRank;

    for( size_t i = 0; i < laidClusters.size(); ++i )
        byRank[laidClusters[i].first->flowRank].push_back( i );

    std::map<wxString, std::pair<long long, long long>> absCenter; // ref -> abs body center
    long long curX = MARGIN_NM;

    for( const auto& [rank, idxs] : byRank )
    {
        long long colWidth = 0;
        long long curY = MARGIN_NM;

        for( size_t idx : idxs )
        {
            const Laid& laid = laidClusters[idx].second;

            for( const auto& [ref, rc] : laid.rel )
                absCenter[ref] = { curX + rc.first, curY + rc.second };

            colWidth = std::max( colWidth, laid.w );
            curY += laid.h + CLUSTER_GAP_NM;
        }

        curX += colWidth + COL_GAP_NM;
    }

    // ── Satellite snap: park each satellite just off its host's REAL pin ──
    // Collision is rectangle-vs-rectangle over every placed body (with a
    // grid halo), so satellites can never sit on a neighbor — they slide
    // perpendicular until they find genuinely free board.
    std::vector<Rect> placedRects;

    for( const auto& [ref, c] : absCenter )
    {
        const PartGeom& g = geom[ref];
        placedRects.push_back( rectAt( c.first, c.second, g.w, g.h, SAT_PAD_NM ) );
    }

    auto collides = [&]( const Rect& r )
    {
        for( const Rect& p : placedRects )
        {
            if( rectsOverlap( r, p ) )
                return true;
        }

        return false;
    };

    for( const auto& [ref, att] : satellites )
    {
        const PartGeom& host = geom[att.toRef];
        const PartGeom& sat = geom[ref];
        auto hostIt = absCenter.find( att.toRef );

        if( hostIt == absCenter.end() || !host.sym )
            continue;

        SCH_PIN* pin = findLibPin( host.sym, att.toPin );

        if( !pin )
            continue;

        // Host pin position in absolute schematic coords (y flipped).
        long long pinX = hostIt->second.first - host.cx
                         + (long long) pin->GetPosition().x * IU_TO_NM;
        long long pinY = hostIt->second.second - host.cy
                         - (long long) pin->GetPosition().y * IU_TO_NM;

        // Outward = away from the host body center, dominant axis.
        long long dx = pinX - hostIt->second.first;
        long long dy = pinY - hostIt->second.second;
        bool horizontal = std::llabs( dx ) >= std::llabs( dy );
        long long dirX = horizontal ? ( dx >= 0 ? 1 : -1 ) : 0;
        long long dirY = horizontal ? 0 : ( dy >= 0 ? 1 : -1 );

        long long reach = horizontal ? sat.w / 2 : sat.h / 2;
        long long cxNm = pinX + dirX * ( 3 * GRID_NM + reach );
        long long cyNm = pinY + dirY * ( 3 * GRID_NM + reach );

        // Slide perpendicular one slot at a time until nothing overlaps.
        long long perpX = horizontal ? 0 : 1;
        long long perpY = horizontal ? 1 : 0;
        long long stepW = ( horizontal ? sat.h : sat.w ) + 2 * GRID_NM;
        int guard = 0;

        while( collides( rectAt( cxNm, cyNm, sat.w, sat.h, SAT_PAD_NM ) ) && guard++ < 128 )
        {
            cxNm += perpX * stepW;
            cyNm += perpY * stepW;
        }

        absCenter[ref] = { cxNm, cyNm };
        placedRects.push_back( rectAt( cxNm, cyNm, sat.w, sat.h, SAT_PAD_NM ) );
    }

    // ── Section frames: one padded bounds per cluster, satellites included ──
    if( aSections )
    {
        std::map<wxString, size_t> clusterOf;

        for( size_t i = 0; i < laidClusters.size(); ++i )
        {
            for( const auto& [ref, rc] : laidClusters[i].second.rel )
            {
                (void) rc;
                clusterOf[ref] = i;
            }
        }

        for( const auto& [ref, att] : satellites )
        {
            auto it = clusterOf.find( att.toRef );

            if( it != clusterOf.end() && absCenter.count( ref ) )
                clusterOf[ref] = it->second;
        }

        std::vector<Rect> bounds( laidClusters.size(),
                                  Rect{ LLONG_MAX, LLONG_MAX, LLONG_MIN, LLONG_MIN } );
        std::vector<int>  memberCount( laidClusters.size(), 0 );
        std::vector<bool> hasRealPart( laidClusters.size(), false );

        for( const auto& [ref, ci] : clusterOf )
        {
            auto it = absCenter.find( ref );

            if( it == absCenter.end() )
                continue;

            const PartGeom& g = geom[ref];
            Rect& b = bounds[ci];
            b.x1 = std::min( b.x1, it->second.first - g.w / 2 );
            b.y1 = std::min( b.y1, it->second.second - g.h / 2 );
            b.x2 = std::max( b.x2, it->second.first + g.w / 2 );
            b.y2 = std::max( b.y2, it->second.second + g.h / 2 );

            memberCount[ci]++;

            if( !ref.StartsWith( wxT( "#" ) ) )
                hasRealPart[ci] = true;
        }

        for( size_t i = 0; i < laidClusters.size(); ++i )
        {
            const Rect& b = bounds[i];

            if( b.x1 > b.x2 )
                continue;

            // A frame around one lonely symbol (or a pile of virtual power
            // symbols: PWR_FLAG / GND rails) is noise, not a section.
            if( memberCount[i] < 2 || !hasRealPart[i] )
                continue;

            const Cluster* c = laidClusters[i].first;
            SECTION_BOX sec;

            if( c->moduleId == "_misc" )
                sec.title = wxT( "misc" );
            else if( !c->role.empty() && c->role != "other" )
                sec.title = wxString::Format( wxT( "%s  [%s]" ),
                                              wxString::FromUTF8( c->moduleId ),
                                              wxString::FromUTF8( c->role ) );
            else
                sec.title = wxString::FromUTF8( c->moduleId );

            sec.x = snapNm( b.x1 - SECTION_PAD_NM );
            sec.y = snapNm( b.y1 - SECTION_PAD_NM );
            sec.w = snapNm( b.x2 + SECTION_PAD_NM ) - sec.x;
            sec.h = snapNm( b.y2 + SECTION_PAD_NM ) - sec.y;
            aSections->push_back( sec );
        }
    }

    // ── Write back: body center → symbol anchor, snapped to grid ──
    int moved = 0;

    for( const auto& [ref, c] : absCenter )
    {
        const PartGeom& g = geom[ref];
        long long x = snapNm( c.first - g.cx );
        long long y = snapNm( c.second - g.cy );

        nlohmann::json& data = aOperations[g.opIdx].data;
        data["x"] = x;
        data["y"] = y;
        moved++;
    }

    return moved;
}

} // namespace COPPER_PLACEMENT
