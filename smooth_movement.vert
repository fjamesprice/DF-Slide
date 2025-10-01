#version 330 core

// Input vertex attributes
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec2 in_texcoord;
layout(location = 2) in vec4 in_color;

// Interpolation uniforms
uniform vec2 u_prev_pos;      // Previous tile position
uniform vec2 u_curr_pos;      // Current tile position
uniform float u_progress;     // Interpolation progress (0.0 to 1.0)
uniform bool u_is_ghost;      // Whether this is a ghost render
uniform float u_ghost_alpha;  // Ghost transparency

// Projection uniforms
uniform mat4 u_projection;
uniform mat4 u_view;
uniform float u_tile_size;    // Size of one tile in world units

// Output to fragment shader
out vec2 v_texcoord;
out vec4 v_color;

void main() {
    // Calculate interpolated position
    vec2 interpolated_pos = mix(u_prev_pos, u_curr_pos, u_progress);
    
    // If rendering ghost, use final position
    if (u_is_ghost) {
        interpolated_pos = u_curr_pos;
    }
    
    // Convert tile coordinates to world space
    vec2 world_pos = interpolated_pos * u_tile_size;
    
    // Apply vertex offset (for sprite rendering)
    vec3 final_pos = vec3(world_pos + in_position.xy, in_position.z);
    
    // Transform to screen space
    gl_Position = u_projection * u_view * vec4(final_pos, 1.0);
    
    // Pass through texture coordinates
    v_texcoord = in_texcoord;
    
    // Apply color and alpha
    v_color = in_color;
    if (u_is_ghost) {
        v_color.a *= u_ghost_alpha;
    }
}