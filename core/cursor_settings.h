// cursor-core — the persisted Cursor Finder settings record. Pure C++17 (no
// Nexus/ImGui/Windows), so it is unit-testable off-game on macOS/clang. The DLL
// glue lives in cursor/src.
//
// The record carries a top-level schema version so later versions add fields and migrate
// older files forward without data loss:
//   v1: { enabled, draw_above_windows }
//   v2: + appearance — preset, colour, size, opacity, outline
//         (toggle + colour), fill (toggle + opacity + colour)
//   v3: + fill_size_pct (fill size as a % of the preset's max extent)
//   v4: + freeze_after_drag (freeze the cursor while dragging — pin the
//         OS pointer at the press point for the duration of the hold; the
//         field was introduced by an earlier draw-only freeze, now superseded)
//   v5: + show_quick_access_icon (whether the Cursor Finder button is
//         shown in the Nexus QuickAccess bar; default on, hideable once the
//         highlight is configured — the settings stay reachable via the
//         Nexus Options page)
//   v6: + out_of_combat_mode / in_combat_mode (the "Show overlay"
//         visibility matrix — when the marker is drawn per combat state;
//         both default Always, preserving the always-shown v1-v5 behaviour)
//   v7: + welcomed (the one-time first-run flag). A fresh
//         install starts NOT welcomed (defaults() -> welcomed == false) so
//         the designed window auto-opens once, then the flag is set +
//         persisted. An older/absent-version file migrates forward as
//         ALREADY welcomed (welcomed = true) so upgrading users get no
//         surprise auto-open.
// See cursor_store.h for the load/save/migrate machinery.
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace cursor {

// The five presets. Pulse Ring is the product default. Adding a preset is data
// (art + this enum + the slug/hue tables below), not new draw code.
enum class Preset {
    PulseRing,
    CornerReticle,
    BeaconCrosshair,
    RadarDash,
    SoftHalo,
};

// An 8-bit-per-channel colour. Alpha is intentionally absent: the colour layers
// are tinted by an RGB hue and their transparency comes from the Opacity control
// (overall marker alpha), not from a per-colour alpha.
struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

inline bool operator==(const Rgb& a, const Rgb& b)
{
    return a.r == b.r && a.g == b.g && a.b == b.b;
}
inline bool operator!=(const Rgb& a, const Rgb& b) { return !(a == b); }

// Control ranges from the reference design. Read-side
// clamping (cursor_store) keeps an out-of-range persisted value in bounds rather
// than trusting the file.
inline constexpr int kSizeMin        = 40;   // px
inline constexpr int kSizeMax        = 100;  // px (the design allowed 180; capped to
                                             // 100 — 180 is oversized for a
                                             // cursor aid, per in-game feedback)
inline constexpr int kOpacityMin     = 20;   // %
inline constexpr int kOpacityMax     = 100;  // %
inline constexpr int kFillOpacityMin = 0;    // %
inline constexpr int kFillOpacityMax = 100;  // %
inline constexpr int kFillSizeMin    = 10;   // % of the preset's max fill extent
inline constexpr int kFillSizeMax    = 100;  // % (100 = as big as the shape)

inline constexpr int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// The signature hue each preset carries by default. Values from the reference
// design — hues GW2's own effects avoid (no red = enemy AoE).
inline constexpr Rgb signature_hue(Preset p)
{
    switch (p)
    {
        case Preset::PulseRing:       return Rgb{0xff, 0x2d, 0x9b}; // magenta
        case Preset::CornerReticle:   return Rgb{0x22, 0xe0, 0xff}; // cyan
        case Preset::BeaconCrosshair: return Rgb{0xf2, 0xf2, 0xf6}; // white
        case Preset::RadarDash:       return Rgb{0xb2, 0x6b, 0xff}; // violet
        case Preset::SoftHalo:        return Rgb{0x3f, 0xd4, 0xc9}; // teal
    }
    return Rgb{0xff, 0x2d, 0x9b};
}

// Stable on-disk slug for a preset (never localised, never reordered — the JSON
// contract). Kept in sync with preset_from_slug below.
inline constexpr const char* preset_to_slug(Preset p)
{
    switch (p)
    {
        case Preset::PulseRing:       return "pulse_ring";
        case Preset::CornerReticle:   return "corner_reticle";
        case Preset::BeaconCrosshair: return "beacon_crosshair";
        case Preset::RadarDash:       return "radar_dash";
        case Preset::SoftHalo:        return "soft_halo";
    }
    return "pulse_ring";
}

// Parse a slug back to a preset; std::nullopt for an unknown value (the caller
// degrades to the default rather than throwing — the corrupt-file contract).
inline std::optional<Preset> preset_from_slug(std::string_view s)
{
    if (s == "pulse_ring")       { return Preset::PulseRing; }
    if (s == "corner_reticle")   { return Preset::CornerReticle; }
    if (s == "beacon_crosshair") { return Preset::BeaconCrosshair; }
    if (s == "radar_dash")       { return Preset::RadarDash; }
    if (s == "soft_halo")        { return Preset::SoftHalo; }
    return std::nullopt;
}

// --- v6 visibility matrix ---------------------------------------------------
// The marker's visibility per game state. Out of combat carries the
// full 3-way choice; In combat is intentionally 2-way (no "While moving"): in
// combat the player is in mouse-look / action-camera where the OS cursor is locked
// to centre and pointer deltas are zero, so a combat "While moving" would hide the
// marker exactly when it is most needed.
enum class OutOfCombatMode { Always, WhileMoving, Never };
enum class InCombatMode { Always, Never };

// Stable on-disk slugs (never localised, never reordered — the JSON contract),
// mirroring preset_to_slug/preset_from_slug.
inline constexpr const char* out_of_combat_to_slug(OutOfCombatMode m)
{
    switch (m)
    {
        case OutOfCombatMode::Always:      return "always";
        case OutOfCombatMode::WhileMoving: return "while_moving";
        case OutOfCombatMode::Never:       return "never";
    }
    return "always";
}

inline std::optional<OutOfCombatMode> out_of_combat_from_slug(std::string_view s)
{
    if (s == "always")       { return OutOfCombatMode::Always; }
    if (s == "while_moving") { return OutOfCombatMode::WhileMoving; }
    if (s == "never")        { return OutOfCombatMode::Never; }
    return std::nullopt;
}

inline constexpr const char* in_combat_to_slug(InCombatMode m)
{
    switch (m)
    {
        case InCombatMode::Always: return "always";
        case InCombatMode::Never:  return "never";
    }
    return "always";
}

inline std::optional<InCombatMode> in_combat_from_slug(std::string_view s)
{
    if (s == "always") { return InCombatMode::Always; }
    if (s == "never")  { return InCombatMode::Never; }
    // "while_moving" is deliberately NOT a valid in-combat value: std::nullopt
    // so the caller coerces it to the In-combat default rather than accepting it.
    return std::nullopt;
}

// The Cursor Finder configuration, one shared profile (not per-character).
struct CursorSettings {
    // Bump when the on-disk shape changes; older/absent-version files migrate
    // forward (see cursor_store.cpp).
    static constexpr int kSchemaVersion = 7;

    // --- v1 ------------------------------------------------------------------
    // Master on/off for the marker. Default ON: enabling the addon at all is an
    // explicit act (the player added cursor.dll and toggled it), and the whole
    // point — a findable cursor — should be visible the first time the panel or
    // hotkey is used rather than silently off.
    bool enabled = true;

    // Draw the marker on the foreground draw list (above the addon's own Nexus
    // windows) when true, else the background list (below windows). Default ON
    // per the design's "Show above Nexus windows" toggle.
    bool draw_above_windows = true;

    // --- v2 appearance ------------------------------------------------------
    // The selected preset style. Default Pulse Ring.
    Preset preset = Preset::PulseRing;

    // The marker's colour-layer tint. Default = the preset's signature hue.
    Rgb colour = signature_hue(Preset::PulseRing);

    // Marker side length in pixels, clamped to [kSizeMin, kSizeMax].
    int size_px = 96;

    // Overall marker alpha as a percentage, clamped to [kOpacityMin, kOpacityMax].
    int opacity_pct = 90;

    // Outline layer (a divergence from the reference design's fixed dark outline).
    // On by default in a dark hue so the shape reads on any background; the
    // player can recolour it or turn it off.
    bool outline = true;
    Rgb  outline_colour = Rgb{0x14, 0x14, 0x18}; // near-black

    // Fill centre. Off by default; when on, fills the interior with a
    // translucent colour at fill_opacity_pct ([kFillOpacityMin, kFillOpacityMax]).
    bool fill = false;
    int  fill_opacity_pct = 35;
    Rgb  fill_colour = Rgb{0xf2, 0xf2, 0xf6}; // near-white (design default)

    // Fill size as a percentage of the preset's max fill extent (v3) — 100 fills
    // the shape (e.g. as big as the Beacon Crosshair), clamped to
    // [kFillSizeMin, kFillSizeMax].
    int  fill_size_pct = 70;

    // --- v4 behaviour -------------------------------------------------------
    // "Freeze cursor while dragging". When on, holding a mouse button (a drag)
    // pins the OS pointer at the press point (Win32 ClipCursor, in the DLL glue)
    // for the duration of the hold, so the cursor can't drift during the drag and
    // can't jump when it reappears on release; letting go of the button unpins it.
    // Sends no game input. Default OFF. (Field introduced by an earlier draw-only
    // freeze, now superseded by the clip.)
    bool freeze_after_drag = false;

    // --- v5 quick-access ---------------------------------------------------
    // Whether the Cursor Finder button appears in the Nexus QuickAccess toolbar.
    // Default ON so the addon is discoverable on first install; the player can
    // turn it off once the highlight is tuned. Hiding it never strands the
    // settings — they stay reachable from the Nexus Options page (Configure),
    // independent of this icon and the (unbound) keybind.
    bool show_quick_access_icon = true;

    // --- v6 visibility matrix ----------------------------------------------
    // When the marker is drawn, chosen independently per combat state ("Show
    // overlay"). Both default Always, so a migrated v1-v5 file keeps the current
    // always-shown behaviour. Combat state comes from the MumbleLink UiState bit
    // (see cursor/core/visibility.h); "While moving" (out-of-combat only) tracks
    // frame-to-frame pointer motion.
    OutOfCombatMode out_of_combat_mode = OutOfCombatMode::Always;
    InCombatMode    in_combat_mode     = InCombatMode::Always;

    // --- v7 first-run -------------------------------------------------------
    // Whether the user has already met the designed settings window. Default
    // FALSE so a brand-new install (no file -> defaults()) triggers the one-time
    // first-run auto-open; the DLL glue then sets this true and
    // persists it immediately so the auto-open never repeats on later launches.
    // An older/absent-version on-disk file migrates forward as ALREADY welcomed
    // (cursor_store.cpp), so upgrading users get no surprise auto-open.
    bool welcomed = false;

    // The factory default record (first run, or recovery from a corrupt file).
    static CursorSettings defaults() { return CursorSettings{}; }
};

// Pure first-run predicate: the designed window auto-opens
// exactly when the user has not yet been welcomed. Header-inline so it is unit-
// testable off-game without new build-file churn.
inline bool should_first_run_open(const CursorSettings& s) { return !s.welcomed; }

// Value equality — handy for tests and change-detection in the panel.
inline bool operator==(const CursorSettings& a, const CursorSettings& b)
{
    return a.enabled == b.enabled &&
           a.draw_above_windows == b.draw_above_windows &&
           a.preset == b.preset &&
           a.colour == b.colour &&
           a.size_px == b.size_px &&
           a.opacity_pct == b.opacity_pct &&
           a.outline == b.outline &&
           a.outline_colour == b.outline_colour &&
           a.fill == b.fill &&
           a.fill_opacity_pct == b.fill_opacity_pct &&
           a.fill_colour == b.fill_colour &&
           a.fill_size_pct == b.fill_size_pct &&
           a.freeze_after_drag == b.freeze_after_drag &&
           a.show_quick_access_icon == b.show_quick_access_icon &&
           a.out_of_combat_mode == b.out_of_combat_mode &&
           a.in_combat_mode == b.in_combat_mode &&
           a.welcomed == b.welcomed;
}
inline bool operator!=(const CursorSettings& a, const CursorSettings& b)
{
    return !(a == b);
}

} // namespace cursor
