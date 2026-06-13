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

#ifndef KICAD_COPPER_TYPES_H
#define KICAD_COPPER_TYPES_H

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <nlohmann/json.hpp>
#include <math/vector2d.h>

namespace COPPER
{

/// A component in the schematic context sent to the cloud API
struct ContextComponent
{
    std::string reference;
    std::string value;
    std::string lib_id;
    VECTOR2I    position;
    double      rotation = 0.0;

    nlohmann::json toJson() const
    {
        return {
            { "reference", reference },
            { "value", value },
            { "lib_id", lib_id },
            { "position", { { "x", position.x }, { "y", position.y } } },
            { "rotation", rotation }
        };
    }
};

/// A net in the schematic context
struct ContextNet
{
    std::string              name;
    std::vector<std::string> connected_pins;  // e.g. ["R1:1", "C1:2"]

    nlohmann::json toJson() const
    {
        return {
            { "name", name },
            { "connected_pins", connected_pins }
        };
    }
};

/// Full schematic context sent with each API request
struct SchematicContext
{
    std::vector<ContextComponent> components;
    std::vector<ContextNet>       nets;
    std::vector<std::string>      power_rails;
    VECTOR2I                      bounding_box_min;
    VECTOR2I                      bounding_box_max;
    VECTOR2I                      free_position;
    std::vector<std::string>      selected_refs;

    nlohmann::json toJson() const
    {
        nlohmann::json j;
        j["components"] = nlohmann::json::array();
        for( const auto& c : components )
            j["components"].push_back( c.toJson() );

        j["nets"] = nlohmann::json::array();
        for( const auto& n : nets )
            j["nets"].push_back( n.toJson() );

        j["power_rails"] = power_rails;
        j["bounding_box"] = {
            { "min", { { "x", bounding_box_min.x }, { "y", bounding_box_min.y } } },
            { "max", { { "x", bounding_box_max.x }, { "y", bounding_box_max.y } } }
        };
        j["free_position"] = { { "x", free_position.x }, { "y", free_position.y } };
        j["selected_refs"] = selected_refs;
        return j;
    }
};

/// An operation returned by the cloud API (plan step to execute)
struct Operation
{
    std::string    type;  // PLACE_COMPONENT, ADD_WIRE, ADD_LABEL, ADD_JUNCTION, ADD_POWER_SYMBOL
    nlohmann::json data;

    static Operation fromJson( const nlohmann::json& j )
    {
        Operation op;
        op.type = j.value( "type", "" );
        op.data = j.value( "data", nlohmann::json::object() );
        return op;
    }
};

/// A step in a plan card
struct PlanStep
{
    int         index;
    std::string description;

    static PlanStep fromJson( const nlohmann::json& j )
    {
        PlanStep s;
        s.index = j.value( "index", 0 );
        s.description = j.value( "description", "" );
        return s;
    }
};

/// A plan card returned by the cloud API
struct PlanCard
{
    std::vector<PlanStep> steps;
    std::string           placement_info;

    static PlanCard fromJson( const nlohmann::json& j )
    {
        PlanCard p;
        p.placement_info = j.value( "placement_info", "" );

        if( j.contains( "steps" ) && j["steps"].is_array() )
        {
            for( const auto& s : j["steps"] )
                p.steps.push_back( PlanStep::fromJson( s ) );
        }

        return p;
    }
};

/// One named functional block in the design summary
struct DesignSection
{
    std::string              group;
    std::string              purpose;
    std::vector<std::string> references;

    static DesignSection fromJson( const nlohmann::json& j )
    {
        DesignSection s;
        s.group = j.value( "group", "" );
        s.purpose = j.value( "purpose", "" );

        if( j.contains( "references" ) && j["references"].is_array() )
        {
            for( const auto& r : j["references"] )
            {
                if( r.is_string() )
                    s.references.push_back( r.get<std::string>() );
            }
        }

        return s;
    }
};

/// One rail in the power tree
struct DesignPowerRail
{
    std::string rail;
    std::string voltage;
    std::string source;
    std::string est_current;

    static DesignPowerRail fromJson( const nlohmann::json& j )
    {
        DesignPowerRail p;
        p.rail = j.value( "rail", "" );
        p.voltage = j.value( "voltage", "" );
        p.source = j.value( "source", "" );
        p.est_current = j.value( "est_current", "" );
        return p;
    }
};

/// One row of the bill of materials
struct DesignBomRow
{
    std::vector<std::string> references;
    int                      quantity = 0;
    std::string              value;
    std::string              lib_id;
    std::string              footprint;

    static DesignBomRow fromJson( const nlohmann::json& j )
    {
        DesignBomRow b;

        if( j.contains( "references" ) && j["references"].is_array() )
        {
            for( const auto& r : j["references"] )
            {
                if( r.is_string() )
                    b.references.push_back( r.get<std::string>() );
            }
        }

        b.quantity = j.value( "quantity", 0 );
        b.value = j.value( "value", "" );
        b.lib_id = j.value( "lib_id", "" );
        b.footprint = j.value( "footprint", "" );
        return b;
    }
};

/// Headline counts for the design
struct DesignStats
{
    int parts = 0;
    int unique_parts = 0;
    int nets = 0;
    int no_connects = 0;

    static DesignStats fromJson( const nlohmann::json& j )
    {
        DesignStats s;
        s.parts = j.value( "parts", 0 );
        s.unique_parts = j.value( "unique_parts", 0 );
        s.nets = j.value( "nets", 0 );
        s.no_connects = j.value( "no_connects", 0 );
        return s;
    }
};

/// Optional, additive human-readable design narration (PROTOCOL.md
/// §design_summary). Present on newer backends' generate `done` payload;
/// absent on older backends / the module-IR path. Parsed tolerantly — every
/// field defaults — so a partial or evolving shape never breaks parsing.
struct DesignSummary
{
    std::string                  board_name;
    std::string                  board_description;
    std::string                  overview;
    std::vector<DesignSection>   sections;
    std::vector<DesignPowerRail> power;
    std::vector<DesignBomRow>    bom;
    DesignStats                  stats;
    std::string                  notes;

    static DesignSummary fromJson( const nlohmann::json& j )
    {
        DesignSummary d;

        if( j.contains( "board" ) && j["board"].is_object() )
        {
            d.board_name = j["board"].value( "name", "" );
            d.board_description = j["board"].value( "description", "" );
        }

        d.overview = j.value( "overview", "" );
        d.notes = j.value( "notes", "" );

        if( j.contains( "sections" ) && j["sections"].is_array() )
        {
            for( const auto& s : j["sections"] )
            {
                if( s.is_object() )
                    d.sections.push_back( DesignSection::fromJson( s ) );
            }
        }

        if( j.contains( "power" ) && j["power"].is_array() )
        {
            for( const auto& p : j["power"] )
            {
                if( p.is_object() )
                    d.power.push_back( DesignPowerRail::fromJson( p ) );
            }
        }

        if( j.contains( "bom" ) && j["bom"].is_array() )
        {
            for( const auto& b : j["bom"] )
            {
                if( b.is_object() )
                    d.bom.push_back( DesignBomRow::fromJson( b ) );
            }
        }

        if( j.contains( "stats" ) && j["stats"].is_object() )
            d.stats = DesignStats::fromJson( j["stats"] );

        return d;
    }
};

/// Full response from the Copper cloud API
struct CopperResponse
{
    std::string                  message;
    std::vector<Operation>       operations;
    PlanCard                     plan;
    std::string                  intent;  // "generate", "recommend", "chat"
    bool                         success = true;
    std::string                  error;
    // Optional, additive (PROTOCOL.md §design_summary). Empty when absent.
    std::optional<DesignSummary> design_summary;

    static CopperResponse fromJson( const nlohmann::json& j )
    {
        CopperResponse r;
        r.message = j.value( "message", "" );
        r.intent = j.value( "intent", "chat" );
        r.success = j.value( "success", true );
        r.error = j.value( "error", "" );

        if( j.contains( "operations" ) && j["operations"].is_array() )
        {
            for( const auto& op : j["operations"] )
                r.operations.push_back( Operation::fromJson( op ) );
        }

        if( j.contains( "plan" ) && j["plan"].is_object() )
            r.plan = PlanCard::fromJson( j["plan"] );

        // Tolerate missing/null: only parse when it's a real object.
        if( j.contains( "design_summary" ) && j["design_summary"].is_object() )
            r.design_summary = DesignSummary::fromJson( j["design_summary"] );

        return r;
    }
};

/// An SSE event received during streaming
struct SSEEvent
{
    std::string event;  // "stage", "message", "plan", "operations", "done", "error"
    std::string data;   // JSON string

    nlohmann::json dataAsJson() const
    {
        try { return nlohmann::json::parse( data ); }
        catch( ... ) { return nlohmann::json::object(); }
    }
};

/// Stage progress info from SSE
struct StageInfo
{
    std::string name;
    std::string status;  // "pending", "active", "complete", "error"

    static StageInfo fromJson( const nlohmann::json& j )
    {
        StageInfo s;
        s.name = j.value( "name", "" );
        s.status = j.value( "status", "pending" );
        return s;
    }
};

/// Callback types
using SSECallback = std::function<void( const SSEEvent& )>;
using ResponseCallback = std::function<void( const CopperResponse& )>;
using ErrorCallback = std::function<void( const std::string& )>;

}  // namespace COPPER

#endif // KICAD_COPPER_TYPES_H
