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

#ifndef COPPER_PLACEMENT_H
#define COPPER_PLACEMENT_H

#include <functional>
#include <vector>

#include <copper/copper_types.h>
#include <wx/string.h>

class LIB_SYMBOL;

namespace COPPER_PLACEMENT
{

/**
 * A functional section of the schematic: one hint cluster's final footprint
 * (members + satellites), padded. Coordinates in nanometers, y down.
 */
struct SECTION_BOX
{
    wxString  title;
    long long x = 0;
    long long y = 0;
    long long w = 0;
    long long h = 0;
};

/**
 * Client-side placement refinement: rewrite PLACE_COMPONENT x/y (nanometers)
 * in @a aOperations using REAL library symbol geometry, guided by the
 * advisory PLACEMENT_HINTS op the backend emits.
 *
 * The backend compiles meaning (cluster roles, flow ranks, satellite
 * attachments); the client compiles geometry (real bboxes, real pin
 * positions, grid snapping). Pure transformation: ops in, ops out — no
 * schematic state is touched.
 *
 * No-op when the plan carries no PLACEMENT_HINTS (old backends keep their
 * coordinate fallback).
 *
 * @param aOperations plan ops, modified in place.
 * @param aResolve    lib_id string -> LIB_SYMBOL* (nullptr if unresolvable).
 * @param aSections   optional out: one padded bounding box per hint cluster,
 *                    suitable for drawing section frames.
 * @return number of PLACE_COMPONENT ops whose coordinates were rewritten.
 */
int RefinePlacement( std::vector<COPPER::Operation>& aOperations,
                     const std::function<LIB_SYMBOL*( const wxString& )>& aResolve,
                     std::vector<SECTION_BOX>* aSections = nullptr );

} // namespace COPPER_PLACEMENT

#endif // COPPER_PLACEMENT_H
