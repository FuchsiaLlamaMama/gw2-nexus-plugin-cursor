// cursor-core — "clip cursor while dragging" state machine.
//
// Pins the OS pointer in place for the duration of a mouse-button hold and
// releases it the instant the button comes up. The DLL applies the decision with
// Win32 ClipCursor() at the returned position (re-applied every frame, since the
// game re-asserts its own clip otherwise); the load-bearing lifetime (release on
// focus loss / unload) also lives in the DLL glue.
//
// Pure C++17, no ImGui / Windows / clock — unit-testable off-game with synthetic
// (button, pointer) sequences.
//
// Behaviour:
//   * The pointer FREEZES while a mouse button is held AND the OS cursor is hidden
//     — an in-world camera / character drag. GW2 hides its own cursor for exactly
//     those drags (the case where the pointer would otherwise jump on release).
//     While frozen the pointer stays pinned, so the cursor cannot drift during the
//     drag and cannot jump when it reappears.
//   * This is evaluated EVERY frame, NOT latched at the press: GW2 hides the cursor
//     a frame or two AFTER the button goes down, once it registers the drag — so a
//     press-edge-only check misses the freeze entirely. The freeze
//     engages the moment the cursor hides and releases when it shows again or the
//     button comes up.
//   * When the cursor is SHOWN the press is over a window — a GW2 native title bar /
//     resize corner or an addon ImGui window — so it does NOT
//     freeze and the window drags/resizes normally. Addon UI (WantCaptureMouse) is
//     excluded explicitly as belt-and-braces.
//   * On button-UP the freeze RELEASES — no hotkey, the pointer is never trapped.
//   * With the toggle off it is a pass-through (returns the live pointer) and holds
//     no state, so turning it off can never leave the pointer pinned.
#pragma once

namespace cursor {

// A screen-space point in pixels. Plain floats — deliberately not ImVec2, to keep
// ImGui out of cursor-core (the DLL converts at the draw site).
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

class ClipFreeze {
public:
    // Advance one frame; returns where the marker should be drawn this frame (the
    // frozen press point while held, otherwise the live pointer).
    //   enabled       — the freeze toggle
    //   button_down   — any drag-capable mouse button (left OR right) held this frame
    //   pointer       — the live pointer (OS cursor hotspot) this frame
    //   ui_capturing  — addon UI wants the mouse this frame (ImGui WantCaptureMouse);
    //                   while true the press is NOT frozen so addon-UI drags stay
    //                   free. Sampled every frame. Defaults false.
    //   cursor_hidden — the OS cursor is hidden this frame. Only an in-world camera /
    //                   character drag hides it; window chrome (native or addon) keeps
    //                   it shown. The freeze engages only when it is hidden, so window
    //                   moves/resizes are never blocked. Defaults true so the
    //                   plain 3-arg call still freezes an in-world press.
    Vec2 update(bool enabled, bool button_down, Vec2 pointer,
                bool ui_capturing = false, bool cursor_hidden = true);

    // True when the OS pointer should be clipped at frozen_pos() this frame.
    bool frozen() const { return frozen_; }
    Vec2 frozen_pos() const { return frozen_pos_; }

    // Clear all state — called when the toggle is off, or by the DLL on
    // focus-loss / unload after it drops the Win32 clip.
    void reset();

private:
    bool frozen_    = false;
    Vec2 frozen_pos_{};      // where the pointer was pinned when the freeze engaged
};

} // namespace cursor
