#include "Core.h"
#include "Console.h"
#include "Export.h"
#include "PluginManager.h"
#include "modules/Gui.h"
#include "modules/Screen.h"
#include "modules/Units.h"
#include "modules/Maps.h"
#include "df/unit.h"
#include "df/world.h"
#include "df/viewscreen.h"
#include <unordered_map>
#include <chrono>

using namespace DFHack;
using namespace df::enums;

DFHACK_PLUGIN("smooth-movement");
REQUIRE_GLOBAL(world);
REQUIRE_GLOBAL(gps);

// Configuration structure
struct SmoothMovementConfig {
    bool enabled = true;
    bool ghost_trail = true;
    float ghost_alpha = 0.5f;
    float interpolation_speed = 1.0f;
    int max_tracked_units = 500;
};

// Per-unit interpolation state
struct UnitRenderState {
    int32_t unit_id;
    float x_prev, y_prev, z_prev;
    float x_curr, y_curr, z_curr;
    float progress; // 0.0 to 1.0
    bool active;
    std::chrono::steady_clock::time_point start_time;
    
    UnitRenderState() : unit_id(-1), x_prev(0), y_prev(0), z_prev(0),
                        x_curr(0), y_curr(0), z_curr(0), 
                        progress(0.0f), active(false) {}
};

// Global state
static SmoothMovementConfig config;
static std::unordered_map<int32_t, UnitRenderState> unit_states;
static std::chrono::steady_clock::time_point last_tick_time;
static const float TICK_DURATION_MS = 100.0f; // Adjust based on game speed

// Forward declarations
DFhackCExport command_result plugin_init(color_ostream &out, std::vector<PluginCommand> &commands);
DFhackCExport command_result plugin_shutdown(color_ostream &out);
DFhackCExport command_result plugin_onupdate(color_ostream &out);

// Command handlers
command_result smooth_movement_cmd(color_ostream &out, std::vector<std::string> &parameters);

// Plugin initialization
DFhackCExport command_result plugin_init(color_ostream &out, std::vector<PluginCommand> &commands) {
    commands.push_back(PluginCommand(
        "smooth-movement",
        "Toggle smooth unit movement rendering",
        smooth_movement_cmd,
        false,
        "Usage:\n"
        "  smooth-movement enable/disable - Toggle smooth movement\n"
        "  smooth-movement ghost on/off - Toggle ghost trail\n"
        "  smooth-movement speed <0.5-2.0> - Adjust interpolation speed\n"
        "  smooth-movement status - Show current settings\n"
    ));
    
    last_tick_time = std::chrono::steady_clock::now();
    out.print("Smooth Movement plugin loaded. Type 'smooth-movement status' for info.\n");
    return CR_OK;
}

// Plugin shutdown
DFhackCExport command_result plugin_shutdown(color_ostream &out) {
    unit_states.clear();
    return CR_OK;
}

// Check if unit is visible in current viewport
bool is_unit_visible(df::unit *unit) {
    if (!unit || !Gui::getViewscreenByType<df::viewscreen_dwarfmodest>(0)) {
        return false;
    }
    
    int32_t view_x, view_y, view_z;
    Gui::getViewCoords(view_x, view_y, view_z);
    
    // Check if unit is on current z-level
    if (unit->pos.z != view_z) {
        return false;
    }
    
    // Check if within viewport bounds (approximate)
    int viewport_width = gps->dimx;
    int viewport_height = gps->dimy;
    
    int rel_x = unit->pos.x - view_x;
    int rel_y = unit->pos.y - view_y;
    
    return (rel_x >= -2 && rel_x < viewport_width + 2 &&
            rel_y >= -2 && rel_y < viewport_height + 2);
}

// Update unit positions and track changes
void update_unit_positions(color_ostream &out) {
    if (!config.enabled || !world || !world->units.active) {
        return;
    }
    
    auto current_time = std::chrono::steady_clock::now();
    
    // Track units that still exist
    std::unordered_map<int32_t, bool> existing_units;
    
    // Iterate through active units
    int tracked_count = 0;
    for (auto unit : world->units.active) {
        if (!unit || !is_unit_visible(unit)) {
            continue;
        }
        
        if (tracked_count >= config.max_tracked_units) {
            break;
        }
        
        int32_t uid = unit->id;
        existing_units[uid] = true;
        
        auto it = unit_states.find(uid);
        if (it == unit_states.end()) {
            // New unit - initialize
            UnitRenderState state;
            state.unit_id = uid;
            state.x_prev = state.x_curr = unit->pos.x;
            state.y_prev = state.y_curr = unit->pos.y;
            state.z_prev = state.z_curr = unit->pos.z;
            state.progress = 1.0f;
            state.active = false;
            unit_states[uid] = state;
        } else {
            UnitRenderState &state = it->second;
            
            // Check if position changed (new simulation tick)
            if (state.x_curr != unit->pos.x || 
                state.y_curr != unit->pos.y ||
                state.z_curr != unit->pos.z) {
                
                // Start new interpolation
                state.x_prev = state.x_curr;
                state.y_prev = state.y_curr;
                state.z_prev = state.z_curr;
                
                state.x_curr = unit->pos.x;
                state.y_curr = unit->pos.y;
                state.z_curr = unit->pos.z;
                
                state.progress = 0.0f;
                state.active = true;
                state.start_time = current_time;
            }
        }
        
        tracked_count++;
    }
    
    // Remove units that no longer exist or are off-screen
    for (auto it = unit_states.begin(); it != unit_states.end();) {
        if (existing_units.find(it->first) == existing_units.end()) {
            it = unit_states.erase(it);
        } else {
            ++it;
        }
    }
}

// Update interpolation progress
void update_interpolation(color_ostream &out) {
    if (!config.enabled) {
        return;
    }
    
    auto current_time = std::chrono::steady_clock::now();
    
    for (auto &pair : unit_states) {
        UnitRenderState &state = pair.second;
        
        if (!state.active) {
            continue;
        }
        
        // Calculate elapsed time since interpolation started
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            current_time - state.start_time).count();
        
        // Update progress
        float adjusted_duration = TICK_DURATION_MS / config.interpolation_speed;
        state.progress = std::min(1.0f, elapsed / adjusted_duration);
        
        // Deactivate if complete
        if (state.progress >= 1.0f) {
            state.active = false;
            state.progress = 1.0f;
        }
    }
}

// Main update hook
DFhackCExport command_result plugin_onupdate(color_ostream &out) {
    if (!config.enabled) {
        return CR_OK;
    }
    
    static int update_counter = 0;
    update_counter++;
    
    // Update positions every 10 frames to detect simulation ticks
    if (update_counter % 10 == 0) {
        update_unit_positions(out);
    }
    
    // Update interpolation every frame
    update_interpolation(out);
    
    return CR_OK;
}

// Command handler
command_result smooth_movement_cmd(color_ostream &out, std::vector<std::string> &parameters) {
    if (parameters.empty()) {
        out.print("Usage: smooth-movement [enable|disable|ghost|speed|status]\n");
        return CR_WRONG_USAGE;
    }
    
    std::string cmd = parameters[0];
    
    if (cmd == "enable") {
        config.enabled = true;
        out.print("Smooth movement enabled.\n");
    } else if (cmd == "disable") {
        config.enabled = false;
        unit_states.clear();
        out.print("Smooth movement disabled.\n");
    } else if (cmd == "ghost") {
        if (parameters.size() < 2) {
            out.print("Usage: smooth-movement ghost [on|off]\n");
            return CR_WRONG_USAGE;
        }
        config.ghost_trail = (parameters[1] == "on");
        out.print("Ghost trail %s.\n", config.ghost_trail ? "enabled" : "disabled");
    } else if (cmd == "speed") {
        if (parameters.size() < 2) {
            out.print("Usage: smooth-movement speed <0.5-2.0>\n");
            return CR_WRONG_USAGE;
        }
        float speed = std::stof(parameters[1]);
        config.interpolation_speed = std::max(0.5f, std::min(2.0f, speed));
        out.print("Interpolation speed set to %.1f\n", config.interpolation_speed);
    } else if (cmd == "status") {
        out.print("=== Smooth Movement Status ===\n");
        out.print("Enabled: %s\n", config.enabled ? "Yes" : "No");
        out.print("Ghost trail: %s\n", config.ghost_trail ? "Yes" : "No");
        out.print("Interpolation speed: %.1f\n", config.interpolation_speed);
        out.print("Tracked units: %d\n", unit_states.size());
    } else {
        out.print("Unknown command: %s\n", cmd.c_str());
        return CR_WRONG_USAGE;
    }
    
    return CR_OK;
}

// Export interpolation data for rendering (called by graphics hook)
extern "C" DFHACK_EXPORT bool get_unit_interpolation_data(
    int32_t unit_id, 
    float &x_prev, float &y_prev,
    float &x_curr, float &y_curr,
    float &progress) {
    
    if (!config.enabled) {
        return false;
    }
    
    auto it = unit_states.find(unit_id);
    if (it == unit_states.end() || !it->second.active) {
        return false;
    }
    
    const UnitRenderState &state = it->second;
    x_prev = state.x_prev;
    y_prev = state.y_prev;
    x_curr = state.x_curr;
    y_curr = state.y_curr;
    progress = state.progress;
    
    return true;
}

extern "C" DFHACK_EXPORT bool should_render_ghost() {
    return config.enabled && config.ghost_trail;
}

extern "C" DFHACK_EXPORT float get_ghost_alpha() {
    return config.ghost_alpha;
}