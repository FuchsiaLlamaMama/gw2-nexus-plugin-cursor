#include "core/clip_freeze.h"

namespace cursor {

void ClipFreeze::reset()
{
    frozen_     = false;
    frozen_pos_ = Vec2{}; // clear all state (re-captured when the freeze next engages)
}

Vec2 ClipFreeze::update(bool enabled, bool button_down, Vec2 pointer,
                        bool ui_capturing, bool cursor_hidden)
{
    // Toggle off: pass through the live pointer and hold no state, so turning the
    // feature off can never leave the pointer pinned.
    if (!enabled)
    {
        reset();
        return pointer;
    }

    // Freeze while a button is held AND the OS cursor is hidden — an in-world
    // camera/character drag (GW2 hides its cursor for those; the case where the
    // pointer would jump on release). This is evaluated EVERY frame, not latched at
    // the press: GW2 hides the cursor a frame or two AFTER the button goes down,
    // once it registers the drag, so a press-edge check misses it entirely.
    // A window drag — GW2 native title bar / resize corner, or an addon
    // ImGui window — keeps the cursor SHOWN the whole time, so it never freezes and
    // the window moves. ui_capturing excludes addon UI explicitly.
    const bool want_freeze = button_down && cursor_hidden && !ui_capturing;

    if (want_freeze && !frozen_)
    {
        // Off -> on: pin where the cursor is as GW2 hides/locks it for the drag.
        frozen_     = true;
        frozen_pos_ = pointer;
    }
    else if (!want_freeze)
    {
        // Cursor shown again, button released, or over addon UI: release the pin.
        frozen_ = false;
    }

    return frozen_ ? frozen_pos_ : pointer;
}

} // namespace cursor
