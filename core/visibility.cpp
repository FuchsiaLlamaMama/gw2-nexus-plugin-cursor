#include "core/visibility.h"

namespace cursor_finder {

bool is_in_combat(std::uint32_t ui_state)
{
    return (ui_state & kUiStateInCombat) != 0u;
}

bool MotionLinger::update(bool moving_now, int now_ms, int linger_ms)
{
    if (moving_now)
    {
        last_move_ms = now_ms;
        moved_ever   = true;
        return true;
    }
    // Still this frame: shown only inside the linger window since the last move.
    // A negative elapsed (clock went backward) reads as no recent move.
    const int elapsed = now_ms - last_move_ms;
    return moved_ever && elapsed >= 0 && elapsed < linger_ms;
}

bool should_show_marker(const CursorSettings& s, bool in_gameplay,
                        bool in_combat, bool moving,
                        bool settings_open)
{
    if (!s.enabled) { return false; }
    // Outside gameplay (char select, loading screens) the matrix doesn't apply:
    // the marker draws only while the settings window is open, so edits made
    // there are visible at the pointer (rather than failing open to always-shown).
    if (!in_gameplay) { return settings_open; }

    if (in_combat)
    {
        switch (s.in_combat_mode)
        {
            case InCombatMode::Always: return true;
            case InCombatMode::Never:  return false;
        }
        return true; // defensive default: shown
    }

    switch (s.out_of_combat_mode)
    {
        case OutOfCombatMode::Always:      return true;
        case OutOfCombatMode::WhileMoving: return moving;
        case OutOfCombatMode::Never:       return false;
    }
    return true; // defensive default: shown
}

} // namespace cursor_finder
