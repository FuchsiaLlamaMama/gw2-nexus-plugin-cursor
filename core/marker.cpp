#include "core/marker.h"

namespace cursor {

MarkerRect centered_marker_rect(float center_x, float center_y, float size)
{
    if (size <= 0.0f)
    {
        // Degenerate: a zero-area rect at the point. Callers draw nothing; this
        // avoids an inverted (max < min) rectangle from a bad size.
        return MarkerRect{center_x, center_y, center_x, center_y};
    }
    const float half = size * 0.5f;
    return MarkerRect{center_x - half, center_y - half,
                      center_x + half, center_y + half};
}

float pulse_ring_radius(float size)
{
    return size > 0.0f ? size * 0.5f : 0.0f;
}

} // namespace cursor
