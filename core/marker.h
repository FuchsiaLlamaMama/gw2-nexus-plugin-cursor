// cursor-finder-core — pointer-marker geometry. Pure C++17, no ImGui: the DLL feeds the
// returned floats straight into ImGui draw calls (AddImage / AddCircle), but the
// math stays testable off-game.
#pragma once

namespace cursor_finder {

// An axis-aligned draw rectangle in screen pixels. Plain floats — deliberately
// NOT ImVec2, to keep ImGui out of cursor-finder-core. The DLL converts to ImVec2 at
// the draw site.
struct MarkerRect {
    float min_x = 0.0f;
    float min_y = 0.0f;
    float max_x = 0.0f;
    float max_y = 0.0f;

    float width()  const { return max_x - min_x; }
    float height() const { return max_y - min_y; }
    float center_x() const { return (min_x + max_x) * 0.5f; }
    float center_y() const { return (min_y + max_y) * 0.5f; }
};

// The square draw rectangle of side `size`, centered on (center_x, center_y).
// The marker is centered on the true click
// point (the OS cursor hotspot ImGui::GetMousePos() reports), never offset to a
// corner. `size` is the full width/height in pixels; a non-positive size yields a
// degenerate zero-area rect at the point (nothing to draw) rather than an
// inverted rectangle.
MarkerRect centered_marker_rect(float center_x, float center_y, float size);

// The core-circle radius for the procedural Pulse Ring default, derived
// from the marker size. Returned so both the live render and the panel preview
// size the ring identically. Half the marker side.
float pulse_ring_radius(float size);

} // namespace cursor_finder
