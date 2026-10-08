// Cursor Finder — a Nexus addon for Guild Wars 2.
//
// The Nexus/ImGui glue around cursor-finder-core: a QuickAccess toolbar button + a
// keybind (unbound by default) that toggle a findability marker centered on the mouse
// pointer, plus a settings panel with a live preview and the full APPEARANCE
// block — preset picker, colour, size, opacity, outline (toggle + colour), fill
// (toggle + opacity + colour), and reset-to-defaults. State persists to
// versioned JSON in the addon directory (write-through — durability does not
// depend on Unload; see core/cursor_finder_store.h).
//
// Presets are drawn from layered white/alpha mask textures: a
// tintable outline layer + a tintable colour layer, embedded as C byte arrays
// (assets/preset_textures_data.h) and loaded via Textures_GetOrCreateFromMemory.
// Each layer is tinted at draw time with AddImage, so one PNG serves any colour.
// Draw order is outline UNDER colour (the outline reads as a dark halo behind the
// brighter core). If a texture is not ready yet, the frame falls back to the
// procedural ring so the marker never blanks.
//
// This file is Windows/MSVC-only (Nexus.h + ImGui + Windows.h). The testable
// logic (settings, migration, geometry) lives in cursor-finder-core, which builds and
// is unit-tested on macOS/clang. NOT built on macOS (needs the sdk/ +
// vendor/imgui submodules); compiled by CI on Windows/MSVC.

#include <Windows.h>

#include <atomic>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <filesystem>

#include "imgui.h"
#include "Nexus.h"

#include "core/cursor_finder_store.h"
#include "core/marker.h"
#include "core/clip_freeze.h"
#include "core/visibility.h"
#include "mumble_link.h"
#include "../assets/preset_textures_data.h"
#include "../assets/window_bg_data.h"
#include "../assets/icon_data.h"
#include "../assets/pointer_data.h"
#include "../assets/icon_textures_data.h"  // embedded QuickAccess icon PNG bytes

// Native-look theme. The cursor-finder DLL links shared-core, whose include
// root is shared/, so these resolve directly. Applied stack-scoped around our
// window only (see AddonRender) — never mutating the global ImGui style.
#include "theme/theme.h"
#include "theme/theme_imgui.h"

namespace {

// Identifiers Nexus keys registrations by; must be stable across load/unload.
constexpr const char* kKeybindId     = "KB_CURSOR_FINDER_TOGGLE";
constexpr const char* kQuickAccessId = "QA_CURSOR_FINDER";
constexpr const char* kWindowName    = "Cursor Finder";
// Registered UNBOUND. "(null)" is Nexus's sentinel for "no default combo": the
// keybind identifier still exists (so the quick-access toolbar icon, which
// invokes this identifier, keeps working, and the bind shows up in Nexus's
// keybind settings), but nothing is bound out of the box. The original design's plain
// "C" is a bad default — GW2 players remap the keyboard freely, so "C" almost
// certainly collides with a game action. Users who want a hotkey assign one
// themselves in the Nexus keybind UI.
constexpr const char* kDefaultBind   = "(null)";

// Branded QuickAccess toolbar icon: a custom parchment ring-and-pointer,
// replacing the borrowed built-in ICON_NEXUS glyph. The two PNGs (normal +
// brightened cream hover) are embedded as raw byte arrays in
// ../assets/icon_textures_data.h and registered under these addon-owned
// identifiers via Textures_GetOrCreateFromMemory (see EnsureQuickAccessIcon) — the
// same memory path the preset masks use. NOT the .rc/RCDATA + FindResource path:
// FindResource does not resolve embedded resources in this Nexus/CrossOver build.
// Registered from the render loop, not AddonLoad: the
// textures resolve only after the render device is up, and QuickAccess_Add binds
// the icon at call time.
constexpr const char* kIconId      = "ICON_CURSOR_FINDER";
constexpr const char* kIconHoverId = "ICON_CURSOR_FINDER_HOVER";

AddonDefinition_t g_AddonDef{};
AddonAPI_t*        g_API    = nullptr;
cursor_finder::CursorStore* g_Store = nullptr;
bool               g_PanelOpen = false;

// No bundled panel font: Nexus's Fonts_AddFromResource/RCDATA loader
// fails under CrossOver/Wine, so the panel renders in ImGui's default font —
// which is what already drew in-game.

// Whether the branded QuickAccess toolbar button is currently registered.
// The Add/Remove is driven from the render loop (EnsureQuickAccessIcon), not
// AddonLoad — both because the icon textures only resolve after the render device
// is up and because the "Show quick-access icon" setting can add/remove it live.
// Reset on Unload so a reload re-registers once the icon re-resolves.
bool               g_QuickAccessAdded = false;

// Clip-cursor-after-drag state. Persists across frames; a pass-through
// when the toggle is off. Only the render thread touches the state machine.
cursor_finder::ClipFreeze g_Clip;

// Character-motion linger for the out-of-combat "While moving" visibility mode.
// Persists across frames (last-move timestamp); only the render thread touches
// it. The linger logic is pure cursor-finder-core (visibility.h).
cursor_finder::MotionLinger g_MotionLinger;

// True while a Win32 ClipCursor() confinement is currently applied by us. Render
// thread owns it; the WndProc/unload paths call ClipCursor(nullptr) directly
// (idempotent) as a belt-and-braces release without touching this flag.
bool g_ClipActive = false;

// The freeze rectangle in SCREEN coords, captured once at the freeze transition
// and re-applied every frame while frozen (see ApplyClip).
RECT g_ClipRect{};

// Safety-release signal set from the WndProc callback (game/window thread) and
// drained by the render thread each frame. Atomic because the WndProc callback
// runs on the game/window thread, not the render one. The normal release is the
// mouse button coming up (handled in the state machine); this only covers the
// case where focus is lost mid-hold so the button-up is never seen.
std::atomic<bool> g_FocusLost{false}; // game lost focus / alt-tab / kill-focus

// --- preset layer texture table ----------------------------------------------

// One entry per preset (indexed by (int)cursor_finder::Preset): the Nexus texture
// identifier + embedded PNG bytes (from preset_textures_data.h) for each layer.
// Loaded via Textures_GetOrCreateFromMemory — no Windows resource lookup.
struct LayerTex {
    const char*          id;
    const unsigned char* data;
    unsigned int         size;
};
struct PresetTex {
    LayerTex outline;
    LayerTex colour;
};
const PresetTex kPresetTex[] = {
    { {"TEX_CURSOR_FINDER_PULSE_RING_OUTLINE",       cursor_finder_art::kPulseRingOutline,       cursor_finder_art::kPulseRingOutline_len},
      {"TEX_CURSOR_FINDER_PULSE_RING_COLOUR",        cursor_finder_art::kPulseRingColour,        cursor_finder_art::kPulseRingColour_len} },
    { {"TEX_CURSOR_FINDER_CORNER_RETICLE_OUTLINE",   cursor_finder_art::kCornerReticleOutline,   cursor_finder_art::kCornerReticleOutline_len},
      {"TEX_CURSOR_FINDER_CORNER_RETICLE_COLOUR",    cursor_finder_art::kCornerReticleColour,    cursor_finder_art::kCornerReticleColour_len} },
    { {"TEX_CURSOR_FINDER_BEACON_CROSSHAIR_OUTLINE", cursor_finder_art::kBeaconCrosshairOutline, cursor_finder_art::kBeaconCrosshairOutline_len},
      {"TEX_CURSOR_FINDER_BEACON_CROSSHAIR_COLOUR",  cursor_finder_art::kBeaconCrosshairColour,  cursor_finder_art::kBeaconCrosshairColour_len} },
    { {"TEX_CURSOR_FINDER_RADAR_DASH_OUTLINE",       cursor_finder_art::kRadarDashOutline,       cursor_finder_art::kRadarDashOutline_len},
      {"TEX_CURSOR_FINDER_RADAR_DASH_COLOUR",        cursor_finder_art::kRadarDashColour,        cursor_finder_art::kRadarDashColour_len} },
    { {"TEX_CURSOR_FINDER_SOFT_HALO_OUTLINE",        cursor_finder_art::kSoftHaloOutline,        cursor_finder_art::kSoftHaloOutline_len},
      {"TEX_CURSOR_FINDER_SOFT_HALO_COLOUR",         cursor_finder_art::kSoftHaloColour,         cursor_finder_art::kSoftHaloColour_len} },
};

// Per-preset capabilities — the presets are geometrically different, so outline
// and fill do not apply uniformly. Soft Halo is a soft graded glow: ONE colour,
// no outline (the design's accent ring reads wrong as a tinted, fixed-size inner
// outline) and no separate fill (the halo IS the fill; a second fill disc just
// double-counts opacity with a different colour — confusing). The fill extent is
// a fraction of the marker size, tuned so the fill sits inside each preset's
// shape (the reticle's fill is a SQUARE, not an oversized disc).
enum class FillShape { None, Disc, Square };
struct PresetCaps {
    bool      has_outline;
    FillShape fill_shape;
    float     fill_extent_max; // fraction of size at Fill size 100% (fills the
                               // shape); scaled down by fill_size_pct
};
constexpr PresetCaps kPresetCaps[] = {
    /* PulseRing       */ { true,  FillShape::Disc,   0.37f },
    /* CornerReticle   */ { true,  FillShape::Square, 0.30f },
    /* BeaconCrosshair */ { true,  FillShape::Disc,   0.44f },
    /* RadarDash       */ { true,  FillShape::Disc,   0.35f },
    /* SoftHalo        */ { false, FillShape::None,   0.00f },
};

// Cached preset textures, filled lazily by Textures_GetOrCreateFromMemory from
// the embedded PNG bytes. Indexed by (int)Preset.
struct PresetTexCache {
    Texture_t* outline = nullptr;
    Texture_t* colour  = nullptr;
};
PresetTexCache g_Tex[5];

// Resolve a preset layer's texture: cache once, else decode the embedded PNG
// bytes via Textures_GetOrCreateFromMemory (no resource lookup / module handle).
// Returns nullptr (or a texture whose Resource is not ready yet) — the caller
// falls back to the procedural ring for that frame.
Texture_t* ResolveTex(int idx, bool colour_layer)
{
    Texture_t*& slot = colour_layer ? g_Tex[idx].colour : g_Tex[idx].outline;
    if (slot && slot->Resource) { return slot; }
    const LayerTex& lt = colour_layer ? kPresetTex[idx].colour : kPresetTex[idx].outline;
    if (g_API && g_API->Textures_GetOrCreateFromMemory)
    {
        if (Texture_t* t = g_API->Textures_GetOrCreateFromMemory(
                lt.id, const_cast<unsigned char*>(lt.data), lt.size))
        {
            slot = t;
        }
    }
    return slot;
}

// Resolve the window-background texture: the mottled parchment/rust card fill
// (assets/textures). Cached in a
// single static and decoded once from the embedded PNG via
// Textures_GetOrCreateFromMemory — the SAME memory loader as the preset layers;
// RCDATA / TryGetBundledTexture failed for this DLL. Returns nullptr (or a
// texture whose Resource is not ready yet) until the upload completes; the caller
// falls back to the primitive DrawThemedFrame for that frame.
Texture_t* ResolveWindowTex()
{
    static Texture_t* slot = nullptr;
    if (slot && slot->Resource) { return slot; }
    if (g_API && g_API->Textures_GetOrCreateFromMemory)
    {
        if (Texture_t* t = g_API->Textures_GetOrCreateFromMemory(
                "TEX_CURSOR_FINDER_WINDOW_SQUARE",
                const_cast<unsigned char*>(kWindowSquareBg),
                static_cast<int>(kWindowSquareBgLen)))
        {
            slot = t;
        }
    }
    return slot;
}

// Resolve the title-chip icon texture: the branded
// Cursor Finder icon (gold ring + pointer), embedded as C bytes (icon_data.h).
// Same cached memory loader as the window-background/preset layers —
// NOT RCDATA. Returns nullptr (or a texture whose Resource is not ready yet)
// until the upload completes; the caller falls back to the drawn pointer glyph
// for that frame.
Texture_t* ResolveIconTex()
{
    static Texture_t* slot = nullptr;
    if (slot && slot->Resource) { return slot; }
    if (g_API && g_API->Textures_GetOrCreateFromMemory)
    {
        if (Texture_t* t = g_API->Textures_GetOrCreateFromMemory(
                "TEX_CURSOR_FINDER_ICON",
                const_cast<unsigned char*>(kCursorIcon),
                static_cast<int>(kCursorIconLen)))
        {
            slot = t;
        }
    }
    return slot;
}

// Resolve the preview pointer texture: the branded gold
// arrow (pointer_data.h, 93×128 PNG, transparent bg), embedded as C bytes and
// loaded via the SAME cached memory loader as the icon/window/preset layers —
// NOT RCDATA. Returns nullptr (or a texture whose Resource is not ready yet)
// until the upload completes; the caller falls back to the drawn pointer glyph
// for that frame.
Texture_t* ResolvePointerTex()
{
    static Texture_t* slot = nullptr;
    if (slot && slot->Resource) { return slot; }
    if (g_API && g_API->Textures_GetOrCreateFromMemory)
    {
        if (Texture_t* t = g_API->Textures_GetOrCreateFromMemory(
                "TEX_CURSOR_FINDER_POINTER",
                const_cast<unsigned char*>(kPointerIcon),
                static_cast<int>(kPointerIconLen)))
        {
            slot = t;
        }
    }
    return slot;
}

// Partial window transparency: the whole card is drawn slightly
// see-through so the game shows through, matching the in-game look. Applied as an
// alpha tint on the window-background texture draw and mirrored on the ImGui
// window-background fill so the effect is consistent. One knob, tunable.
// 235/255 ≈ 0.92. TODO: tune in-game.
constexpr int kWindowBgAlpha = 235;

// The title-band geometry the shared TitleBar draws into (theme_imgui.h): a 3 au
// edge inset and a 58 au band height (passed to TitleBar's optional bar_height so
// the title + subtitle block fits with padding). Mirrored here so the
// textured strip lands exactly under the icon/title/close TitleBar draws on top.
constexpr float kChromeEdge     = 3.0f;
constexpr float kTitleBarHeight = 58.0f;

// --- branded QuickAccess toolbar icon ----------------------------------------

// A texture is "resolved" — safe to hand to the one-shot QuickAccess_Add — once
// Nexus has created its backing resource AND reported real dimensions. Stricter
// than the preset draw path's Resource-only check: QuickAccess_Add binds the icon
// once at call time, so registering against a half-created texture leaves the
// toolbar button on Nexus's fallback glyph (observed in-game).
bool IconResolved(const Texture_t* t)
{
    return t && t->Resource && t->Width > 0 && t->Height > 0;
}

// Cached icon textures, resolved lazily from the embedded PNG bytes
// (icon_textures_data.h) via Textures_GetOrCreateFromMemory. Null until Nexus
// finishes creating them; the caller retries next frame.
Texture_t* g_IconTex      = nullptr;
Texture_t* g_IconHoverTex = nullptr;

Texture_t* TryGetIconTexture(const char* id, const unsigned char* data,
                             unsigned int size, Texture_t*& slot)
{
    if (IconResolved(slot)) { return slot; }
    if (!g_API || !g_API->Textures_GetOrCreateFromMemory) { return nullptr; }
    Texture_t* t = g_API->Textures_GetOrCreateFromMemory(
        id, const_cast<unsigned char*>(data), size);
    if (!IconResolved(t)) { return nullptr; }
    slot = t;
    return slot;
}

// Keep the branded QuickAccess toolbar button in sync with the "Show quick-access
// icon" setting, driven every frame from the render loop. Two reasons
// it isn't a one-shot in AddonLoad: (1) the icon textures only finish resolving
// after the render device is up (later than load), and QuickAccess_Add binds the
// icon at call time — adding it while the texture is empty leaves the button on
// Nexus's fallback glyph; and (2) the setting can flip live, so hiding /
// showing must add / remove the shortcut without a restart. Cheap: it early-returns
// once the registered state already matches the setting.
void EnsureQuickAccessIcon()
{
    if (!g_API || !g_Store) { return; }
    const bool want = g_Store->settings().show_quick_access_icon;

    if (want && !g_QuickAccessAdded)
    {
        if (!g_API->QuickAccess_Add) { return; }
        // Register only once BOTH icon textures report resolved (&& short-circuit,
        // so no partial/blank registration).
        const bool ready =
            TryGetIconTexture(kIconId, cursor_finder_art::kIconCursor,
                              cursor_finder_art::kIconCursor_len, g_IconTex) &&
            TryGetIconTexture(kIconHoverId, cursor_finder_art::kIconCursorHover,
                              cursor_finder_art::kIconCursorHover_len, g_IconHoverTex);
        if (!ready) { return; } // textures not created yet — retry next frame
        // kWindowName doubles as the tooltip so the toolbar label can't drift from
        // the addon/panel name.
        g_API->QuickAccess_Add(kQuickAccessId, kIconId, kIconHoverId,
                               kKeybindId, kWindowName);
        g_QuickAccessAdded = true;
        if (g_API->Log)
        {
            g_API->Log(LOGL_INFO, "Cursor Finder",
                       "cursor-finder: branded quick-access icon registered");
        }
    }
    else if (!want && g_QuickAccessAdded)
    {
        if (g_API->QuickAccess_Remove) { g_API->QuickAccess_Remove(kQuickAccessId); }
        g_QuickAccessAdded = false; // live hide; re-adds if toggled back on
    }
}

// --- colour helpers ----------------------------------------------------------

// A layer tint: the layer's RGB at the overall marker alpha (opacity_pct).
ImU32 LayerTint(const cursor_finder::Rgb& c, int opacity_pct)
{
    const int a = 255 * cursor_finder::clamp_int(opacity_pct, 0, 100) / 100;
    return IM_COL32(c.r, c.g, c.b, a);
}

// The fill tint: fill colour at fill_opacity, further scaled by overall opacity
// so the Opacity control fades the whole marker uniformly.
ImU32 FillTint(const cursor_finder::Rgb& c, int fill_opacity_pct, int opacity_pct)
{
    const int a = 255
        * cursor_finder::clamp_int(fill_opacity_pct, 0, 100) / 100
        * cursor_finder::clamp_int(opacity_pct, 0, 100) / 100;
    return IM_COL32(c.r, c.g, c.b, a);
}

// The pointer position to anchor the marker on. Prefer the OS-instantaneous
// cursor (GetCursorPos -> client space) — it matches the hardware arrow the
// compositor draws — over ImGui's event-driven io.MousePos, which can visibly
// TRAIL the pointer under CrossOver/Wine input delivery (the marker eases behind
// the arrow, then catches up). On native Windows the two agree, so this is a
// no-op there; it only helps the delayed-event path. Falls back to ImGui's mouse
// if the window or mapping is unavailable.
ImVec2 CurrentPointer()
{
    HWND hwnd = ::GetForegroundWindow();
    POINT p;
    if (hwnd && ::GetCursorPos(&p) && ::ScreenToClient(hwnd, &p))
    {
        return ImVec2(static_cast<float>(p.x), static_cast<float>(p.y));
    }
    return ImGui::GetMousePos();
}

// --- marker drawing ----------------------------------------------------------

// Fallback procedural ring when a preset texture is not ready yet:
// a dark outline stroke with a coloured core stroke on top. Keeps the marker
// visible on the first few frames after load while textures upload.
void DrawProceduralRing(ImDrawList* dl, const ImVec2& center, float size,
                        const cursor_finder::CursorSettings& s, bool draw_outline)
{
    const float radius = cursor_finder::pulse_ring_radius(size);
    if (radius <= 0.0f) { return; }
    constexpr int   kSegments   = 48;
    constexpr float kCoreStroke = 3.0f;
    if (draw_outline)
    {
        dl->AddCircle(center, radius, LayerTint(s.outline_colour, s.opacity_pct),
                      kSegments, kCoreStroke + 2.0f);
    }
    dl->AddCircle(center, radius, LayerTint(s.colour, s.opacity_pct),
                  kSegments, kCoreStroke);
}

// Draw the selected preset centered on `center` at the settings' size: an
// optional translucent fill disc, then the outline layer (if enabled), then the
// colour layer on top — each a white mask tinted at draw time.
// Falls back to the procedural ring for any frame the layer textures are not
// ready. Drawn OVER the pointer — never replacing it.
void DrawMarker(ImDrawList* dl, const ImVec2& center, const cursor_finder::CursorSettings& s)
{
    const float size = static_cast<float>(s.size_px);
    const cursor_finder::MarkerRect rect =
        cursor_finder::centered_marker_rect(center.x, center.y, size);
    if (rect.width() <= 0.0f) { return; }
    const ImVec2 p_min(rect.min_x, rect.min_y);
    const ImVec2 p_max(rect.max_x, rect.max_y);

    const int idx = static_cast<int>(s.preset);
    const PresetCaps& caps = kPresetCaps[idx];
    const bool draw_outline = s.outline && caps.has_outline;

    // Fill centre: a procedural translucent shape under the marker, sized
    // and shaped per preset so it sits inside the art (disc for rings, square for
    // the reticle). Presets that ARE a fill (Soft Halo) carry FillShape::None.
    if (s.fill && caps.fill_shape != FillShape::None)
    {
        const ImU32 fc = FillTint(s.fill_colour, s.fill_opacity_pct, s.opacity_pct);
        const float frac = cursor_finder::clamp_int(s.fill_size_pct,
                               cursor_finder::kFillSizeMin, cursor_finder::kFillSizeMax) / 100.0f;
        const float e = size * caps.fill_extent_max * frac;
        if (caps.fill_shape == FillShape::Square)
        {
            dl->AddRectFilled(ImVec2(center.x - e, center.y - e),
                              ImVec2(center.x + e, center.y + e), fc, size * 0.05f);
        }
        else
        {
            dl->AddCircleFilled(center, e, fc, 48);
        }
    }

    Texture_t* colour_tex  = ResolveTex(idx, /*colour_layer=*/true);
    Texture_t* outline_tex = draw_outline ? ResolveTex(idx, /*colour_layer=*/false) : nullptr;

    // Until the colour layer is ready, keep the marker visible procedurally.
    if (!colour_tex || !colour_tex->Resource)
    {
        DrawProceduralRing(dl, center, size, s, draw_outline);
        return;
    }

    // Outline UNDER colour (the outline reads as a dark halo behind the core).
    if (draw_outline && outline_tex && outline_tex->Resource)
    {
        dl->AddImage(static_cast<ImTextureID>(outline_tex->Resource), p_min, p_max,
                     ImVec2(0, 0), ImVec2(1, 1),
                     LayerTint(s.outline_colour, s.opacity_pct));
    }
    dl->AddImage(static_cast<ImTextureID>(colour_tex->Resource), p_min, p_max,
                 ImVec2(0, 0), ImVec2(1, 1), LayerTint(s.colour, s.opacity_pct));
}

// The classic pointer arrow from the reference design's `pointer-glyph`, drawn
// EXACTLY from its polygon (source viewbox 24×34), transformed to screen space as
// p = origin + pt * scale (origin is where viewbox 0,0 lands). The polygon is
// star-shaped from the tip (points[0]), so ImGui's AddConvexPolyFilled — a fan
// from vertex 0 — fills it without artifacts despite the tail notch. glyph-fill
// (#F4F4F7) / glyph-stroke (#15151C) are literal DATA (the design's glyph
// tokens), not chrome from the palette. A subtle offset drop shadow approximates
// the design's 0/1/1.5 black@0.7 (ImGui has no blur).
void DrawPointerGlyph(ImDrawList* dl, const ImVec2& origin, float scale)
{
    static const ImVec2 kPts[7] = {
        {2.0f, 2.0f}, {2.0f, 27.0f}, {8.5f, 20.5f}, {13.0f, 30.0f},
        {16.5f, 28.5f}, {12.0f, 19.0f}, {20.0f, 19.0f},
    };
    ImVec2 pts[7];
    for (int i = 0; i < 7; ++i)
    {
        pts[i] = ImVec2(origin.x + kPts[i].x * scale, origin.y + kPts[i].y * scale);
    }
    const ImU32 fill   = IM_COL32(0xF4, 0xF4, 0xF7, 255); // glyph-fill
    const ImU32 stroke = IM_COL32(0x15, 0x15, 0x1C, 255); // glyph-stroke

    // Drop shadow: an offset copy of the fill at soft black (design 0/1/1.5 @0.7).
    ImVec2 sh[7];
    for (int i = 0; i < 7; ++i)
    {
        sh[i] = ImVec2(pts[i].x, pts[i].y + 1.5f * scale);
    }
    dl->AddConvexPolyFilled(sh, 7, IM_COL32(0, 0, 0, 178));
    dl->AddConvexPolyFilled(pts, 7, fill);
    dl->AddPolyline(pts, 7, stroke, true, 1.4f * scale);
}

// --- panel -------------------------------------------------------------------

// ImGui colour widgets work in float[3]; convert to/from the stored 8-bit Rgb.
void ToFloat3(const cursor_finder::Rgb& c, float out[3])
{
    out[0] = c.r / 255.0f; out[1] = c.g / 255.0f; out[2] = c.b / 255.0f;
}
cursor_finder::Rgb FromFloat3(const float in[3])
{
    auto ch = [](float f) {
        const int v = static_cast<int>(f * 255.0f + 0.5f);
        return static_cast<std::uint8_t>(cursor_finder::clamp_int(v, 0, 255));
    };
    return cursor_finder::Rgb{ch(in[0]), ch(in[1]), ch(in[2])};
}

// --- preview backdrop grounds ------------------------------------------------

// The preview-box needs a swappable background so the player can judge marker
// contrast against different in-game scenes (the design's `preview-bg-row`). This is a
// CONTRAST AID, not a persisted setting — the selection lives in a local static
// (see RenderPanel), never in CursorSettings / cursor_finder_settings.h. The grounds are
// literal tints (a preview aid, not chrome), so they may use hardcoded colours by
// design; they are NOT part of the shared palette.
// Backdrop set matches the reference design: Grid / Desert / Water / Bright
// (the Bright ground is labelled "Light").
// Grid is the default resting look (first item -> local static default 0).
enum class PreviewGround { Grid, Desert, Water, Bright };
constexpr int kPreviewGroundCount = 4;
const char* const kPreviewGroundLabels[kPreviewGroundCount] = {
    "Grid", "Desert", "Water", "Light",
};

// Fill the preview-box rect with the selected contrast ground on the draw list.
void DrawPreviewGround(ImDrawList* dl, const ImVec2& p_min, const ImVec2& p_max,
                       PreviewGround ground)
{
    switch (ground)
    {
        case PreviewGround::Grid: // neutral checkerboard (default)
        {
            dl->AddRectFilled(p_min, p_max, IM_COL32(58, 58, 64, 255));
            const float cell = 18.0f;
            int row = 0;
            for (float y = p_min.y; y < p_max.y; y += cell, ++row)
            {
                for (int col = (row & 1); ; col += 2)
                {
                    const float x = p_min.x + col * cell;
                    if (x >= p_max.x) { break; }
                    dl->AddRectFilled(ImVec2(x, y),
                        ImVec2((x + cell < p_max.x) ? x + cell : p_max.x,
                               (y + cell < p_max.y) ? y + cell : p_max.y),
                        IM_COL32(78, 78, 86, 255));
                }
            }
            break;
        }
        case PreviewGround::Desert: // warm sandy gradient
            dl->AddRectFilledMultiColor(p_min, p_max,
                IM_COL32(150, 120, 78, 255), IM_COL32(150, 120, 78, 255),
                IM_COL32(92, 68, 40, 255),   IM_COL32(92, 68, 40, 255));
            break;
        case PreviewGround::Water: // cool blue gradient
            dl->AddRectFilledMultiColor(p_min, p_max,
                IM_COL32(58, 120, 168, 255), IM_COL32(58, 120, 168, 255),
                IM_COL32(20, 54, 96, 255),   IM_COL32(20, 54, 96, 255));
            break;
        case PreviewGround::Bright: // bright near-white fill
            dl->AddRectFilled(p_min, p_max, IM_COL32(236, 238, 242, 255));
            break;
    }
}

// The settings panel body. Each control edits a working copy and
// writes it through cursor-finder-core on change, so durability never depends on Unload
// and the live preview + live marker reflect edits instantly.
void RenderPanel()
{
    if (!g_Store) { return; }
    cursor_finder::CursorSettings s = g_Store->settings(); // working copy

    const shared::theme::Palette pal = shared::theme::gw2_palette();

    // The whole panel renders in ImGui's default font (no bundled font).

    // A themed section head — gold, matching the design's type scale — replacing
    // default-grey TextUnformatted labels. Renders in the current (default) font.
    auto SectionHead = [&pal](const char* label) {
        ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_gold));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();
    };

    // The wrapped analogue of ImGui::TextDisabled: help sentences must flow to
    // the next line at any panel width instead of clipping at the window edge.
    // TextDisabled/TextUnformatted lay a whole string on one line; a
    // wrap position lets ImGui break it within the content region.
    auto WrappedDisabled = [](const char* text) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    };

    // A 3-part slider row (design size-row/opacity-row): a muted label in a
    // fixed left column (96 au), the slider filling the middle, and the value
    // (gold) right-aligned in a fixed trailing column (44 au). BEHAVIOUR is
    // identical to a plain SliderInt — the caller still clamps and writes the
    // same field; this only relays the affordance. Returns true on change.
    auto SliderRow = [&pal](const char* label, const char* id, int* value,
                            int vmin, int vmax, const char* value_fmt) -> bool {
        const float label_w  = 96.0f; // design label-col
        const float value_w  = 44.0f; // design value-col
        const float gap      = 12.0f; // design gap-col
        const float row_w    = ImGui::GetContentRegionAvail().x;
        // Row start (window-local): honours ImGui::Indent, which SameLine's
        // absolute offsets otherwise ignore — so an indented row shifts as a whole
        // and still ends flush with the column's right edge.
        const float x0       = ImGui::GetCursorPosX();
        float slider_w = row_w - label_w - value_w - gap * 2.0f;
        if (slider_w < 1.0f) { slider_w = 1.0f; }

        // Left label (muted), baseline-aligned to the slider frame.
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_muted));
        ImGui::TextUnformatted(label);
        ImGui::PopStyleColor();

        // Slider fills the middle; hidden ##id label, no in-bar text. The grab
        // reads GOLD (design: gold value track). ImGui 1.80's SliderInt has no
        // separate filled-track colour, so we tint the grab from the theme gold
        // (no hardcoded hex).
        ImGui::SameLine(x0 + label_w);
        ImGui::SetNextItemWidth(slider_w);
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, shared::theme::to_vec4(pal.text_gold));
        ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, shared::theme::to_vec4(pal.button_text));
        const bool changed = ImGui::SliderInt(id, value, vmin, vmax, "");
        ImGui::PopStyleColor(2);

        // Right value (gold), right-aligned within the 44-au trailing column.
        char buf[32];
        std::snprintf(buf, sizeof(buf), value_fmt, *value);
        const float tw = ImGui::CalcTextSize(buf).x;
        ImGui::SameLine(x0 + label_w + slider_w + gap * 2.0f + (value_w - tw));
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_gold));
        ImGui::TextUnformatted(buf);
        ImGui::PopStyleColor();
        return changed;
    };

    // Two-column layout (per the design): title bar (drawn by the caller) →
    // two-column body [preview 246 au | settings fill] → footer. The body fills
    // the child minus a reserved footer band; the settings column scrolls.
    const float preview_col_w = 246.0f; // design preview-col-width
    const float preview_box   = 214.0f; // design preview-box (246 − 32 padding)
    const float footer_h      = 40.0f;
    const float col_gap       = 12.0f;  // design gap-col
    const float avail_h       = ImGui::GetContentRegionAvail().y;
    const float body_h        = avail_h - footer_h;

    const ImVec2 body_origin  = ImGui::GetCursorScreenPos();
    const float  body_w       = ImGui::GetContentRegionAvail().x;
    ImDrawList*  dl           = ImGui::GetWindowDrawList(); // ##cursor-body list

    // Stable inner width of the settings column, derived ONLY from fixed outer
    // geometry + style metrics — never from GetContentRegionAvail() inside the
    // child. The square overlay tiles are sized from this, so their HEIGHT is
    // constant across frames. Basing them on the live avail width instead let a
    // vertical scrollbar shrink avail → shorter tiles → less overflow → no
    // scrollbar → wider avail → taller tiles → overflow again: a per-frame
    // flicker. Reserving the scrollbar width every frame breaks that loop while
    // still letting the scrollbar appear only on genuine overflow.
    const float stable_settings_w =
        body_w - preview_col_w - col_gap
        - ImGui::GetStyle().WindowPadding.x * 2.0f // settings-col child padding (both sides)
        - ImGui::GetStyle().ScrollbarSize;         // reserve the scrollbar every frame

    // Local-only backdrop contrast selection — NOT persisted, NOT in the schema.
    static int s_backdrop = 0; // default = Grid (reference default)

    // Draw order note: the settings column and footer are rendered FIRST (so they
    // edit the working copy `s`), then the preview column is rendered LAST at the
    // left position, so its live marker draws on the preview child's own draw list
    // (reliably on top) and reflects this frame's edits instantly. Explicit
    // SetCursorScreenPos decouples draw order from on-screen position.

    // --- right: settings column (fill, vertically scrollable) ----------------
    ImGui::SetCursorScreenPos(
        ImVec2(body_origin.x + preview_col_w + col_gap, body_origin.y));
    ImGui::BeginChild("##settings-col", ImVec2(0.0f, body_h), false);
    {
        bool enabled = s.enabled;
        if (ImGui::Checkbox("Enable Cursor Finder", &enabled)) { s.enabled = enabled; }

        bool above = s.draw_above_windows;
        if (ImGui::Checkbox("Show over Nexus windows", &above)) { s.draw_above_windows = above; }

        // Quick-access toolbar icon. Lives in the shared panel body so it
        // renders in BOTH surfaces — the floating window and the Nexus Options page —
        // supporting the flow: icon on at install -> tune the highlight -> hide the icon,
        // settings still reachable via Configure.
        bool showQa = s.show_quick_access_icon;
        if (ImGui::Checkbox("Show quick-access icon", &showQa)) { s.show_quick_access_icon = showQa; }
        WrappedDisabled(
            "Adds Cursor Finder to the Nexus quick-access bar. You can hide the icon "
            "after setup and reopen these settings from Addons -> Cursor Finder -> "
            "Configure.");

        ImGui::Separator();
        SectionHead("MARKER");

        // Preset picker: a 5-column grid of compact icon tiles (design
        // style-tile-row). Text buttons overflowed the ~440 au settings column at
        // the fixed 712 au card, clipping "Soft Halo" off the right edge; the
        // design uses square icon tiles instead. Picking a preset also adopts its
        // signature hue, matching the reference design's behaviour.
        const struct { cursor_finder::Preset p; const char* label; } kPresets[] = {
            {cursor_finder::Preset::PulseRing,       "Ring"},
            {cursor_finder::Preset::CornerReticle,   "Reticle"},
            {cursor_finder::Preset::BeaconCrosshair, "Cross"},
            {cursor_finder::Preset::RadarDash,       "Dash"},
            {cursor_finder::Preset::SoftHalo,        "Halo"},
        };

        // Field label above the grid (the design puts tiles below an "Overlay
        // style" label; renamed "Style" under the MARKER head), styled like the other muted field labels (label-mute).
        ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_muted));
        ImGui::TextUnformatted("Style");
        ImGui::PopStyleColor();

        // 5 equal square tiles filling the settings-column width (design: grid
        // 5 cols, gap 7). tile_w = (avail − 4·gap)/5 so all five fit with no
        // horizontal scroll or clipping.
        {
            ImDrawList*  sdl   = ImGui::GetWindowDrawList();
            ImFont*      font  = ImGui::GetFont();
            const float  fbase = ImGui::GetFontSize();
            const float  lbl_sz = fbase * 0.82f; // tile-labels: small (10.5 au)
            const float  gap   = 7.0f;
            // Size from the scrollbar-independent stable width (not live avail),
            // so tile height never oscillates with the scrollbar (flicker fix).
            float tile_w = (stable_settings_w - gap * 4.0f) / 5.0f;
            if (tile_w < 1.0f) { tile_w = 1.0f; }

            for (int i = 0; i < 5; ++i)
            {
                if (i > 0) { ImGui::SameLine(0.0f, gap); }
                const bool active = (s.preset == kPresets[i].p);

                // Themed cell: square button. Active tile gets the gold
                // active-fill + gold border (design: active tile border = gold),
                // matching the previous picker's active treatment — no hardcoded
                // colour.
                if (active)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, shared::theme::to_vec4(pal.button_active));
                    ImGui::PushStyleColor(ImGuiCol_Border, shared::theme::to_vec4(pal.button_border));
                }
                const ImVec2 tile_pos = ImGui::GetCursorScreenPos();
                ImGui::PushID(i);
                const bool clicked =
                    ImGui::Button("##styletile", ImVec2(tile_w, tile_w));
                ImGui::PopID();
                if (active) { ImGui::PopStyleColor(2); }
                if (clicked)
                {
                    s.preset = kPresets[i].p;
                    s.colour = cursor_finder::signature_hue(kPresets[i].p); // signature hue
                }

                // Icon (upper region) + label (lower region), both centered in the
                // tile. The icon is the REAL embedded preset artwork — the same
                // colour-layer texture DrawMarker uses — tinted by the preset's own
                // signature hue so each tile shows in its signature colour (Ring
                // magenta, Reticle cyan, Cross white, Dash violet, Halo teal). If
                // the texture's Resource isn't uploaded yet, the icon area is left
                // blank (no hand-drawn fallback).
                const float cx = tile_pos.x + tile_w * 0.5f;
                const float icon_half = tile_w * 0.26f; // 0.52 wide, centered
                const float icon_cy   = tile_pos.y + tile_w * 0.40f;
                const ImVec2 icon_min(cx - icon_half, icon_cy - icon_half);
                const ImVec2 icon_max(cx + icon_half, icon_cy + icon_half);
                const cursor_finder::Rgb hue = cursor_finder::signature_hue(kPresets[i].p);
                const ImU32 tint = IM_COL32(hue.r, hue.g, hue.b, 255);
                if (Texture_t* t = ResolveTex(i, /*colour_layer=*/true);
                    t && t->Resource)
                {
                    sdl->AddImage(static_cast<ImTextureID>(t->Resource),
                                  icon_min, icon_max, ImVec2(0, 0), ImVec2(1, 1), tint);
                }

                const ImU32 lbl_col = active
                    ? shared::theme::to_u32(pal.text_gold)
                    : shared::theme::to_u32(pal.text_muted);
                const float lbl_w =
                    font->CalcTextSizeA(lbl_sz, FLT_MAX, 0.0f, kPresets[i].label).x;
                sdl->AddText(font, lbl_sz,
                             ImVec2(cx - lbl_w * 0.5f,
                                    tile_pos.y + tile_w - lbl_sz - 5.0f),
                             lbl_col, kPresets[i].label);
            }
        }

        // Colour: tints the colour layer. The design's `color-row` is 6 preset
        // swatches + a trailing custom picker. The swatch set is the design's
        // accent tokens (cyan/magenta/white/violet/teal) plus a warm gold-active
        // (#DCB96E) 6th so the row is the 6 the design calls for. Swatch hexes
        // are literal DATA (like the preview grounds), not chrome from the
        // palette. Clicking a swatch writes the SAME s.colour field the picker
        // does — no new state, no schema change.
        // Order matches the reference design: magenta first, then cyan.
        static const cursor_finder::Rgb kColourSwatches[6] = {
            {0xFF, 0x2D, 0x9B}, // accent-magenta
            {0x22, 0xE0, 0xFF}, // accent-cyan
            {0xF2, 0xF2, 0xF6}, // accent-white
            {0xB2, 0x6B, 0xFF}, // accent-violet
            {0x3F, 0xD4, 0xC9}, // accent-teal
            {0xDC, 0xB9, 0x6E}, // gold-active (6th)
        };
        // Field label above the row (label-mute), matching the other fields.
        ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_muted));
        ImGui::TextUnformatted("Colour");
        ImGui::PopStyleColor();
        {
            ImDrawList* cdl = ImGui::GetWindowDrawList();
            const float sw  = 30.0f; // design swatch
            const float gap = 8.0f;  // design color-row gap
            for (int i = 0; i < 6; ++i)
            {
                if (i > 0) { ImGui::SameLine(0.0f, gap); }
                const cursor_finder::Rgb& sc = kColourSwatches[i];
                const bool active = (s.colour.r == sc.r && s.colour.g == sc.g &&
                                     s.colour.b == sc.b);
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                ImGui::PushID(1000 + i);
                const bool clicked = ImGui::Button("##swatch", ImVec2(sw, sw));
                ImGui::PopID();
                if (clicked) { s.colour = sc; }
                // Fill with the literal swatch colour, then trim: the active
                // swatch gets the themed gold border (same treatment as the
                // preset tiles); others a faint hairline.
                cdl->AddRectFilled(pos, ImVec2(pos.x + sw, pos.y + sw),
                                   IM_COL32(sc.r, sc.g, sc.b, 255), 3.0f);
                cdl->AddRect(pos, ImVec2(pos.x + sw, pos.y + sw),
                             active ? shared::theme::to_u32(pal.button_border)
                                    : shared::theme::to_u32(pal.trim_line),
                             3.0f, 0, active ? 2.0f : 1.0f);
            }
            // Trailing custom picker: the existing ColorEdit3 popup (behaviour
            // unchanged — clicking opens the full picker and writes s.colour). Its
            // swatch is over-painted with a multi-hue rainbow strip so it reads as
            // "open the full picker" (design: multicolour swatch), not just another
            // solid swatch. The rainbow is literal DATA (like the preview grounds),
            // not chrome from the palette. The real ColorEdit3 button stays live
            // underneath the paint, so the click/popup are untouched.
            ImGui::SameLine(0.0f, gap);
            const ImVec2 cust_pos = ImGui::GetCursorScreenPos();
            const float  cust_sz  = ImGui::GetFrameHeight(); // ColorEdit3 swatch = square
            float colour[3]; ToFloat3(s.colour, colour);
            if (ImGui::ColorEdit3("##colour-custom", colour, ImGuiColorEditFlags_NoInputs))
            {
                s.colour = FromFloat3(colour);
            }
            {
                static const ImU32 kRainbow[7] = {
                    IM_COL32(255,  40,  40, 255), // red
                    IM_COL32(255, 190,  40, 255), // amber
                    IM_COL32( 60, 220,  70, 255), // green
                    IM_COL32( 40, 200, 255, 255), // cyan
                    IM_COL32( 60,  90, 255, 255), // blue
                    IM_COL32(190,  70, 255, 255), // violet
                    IM_COL32(255,  60, 170, 255), // magenta
                };
                const int   bands = 6;
                const float bw    = cust_sz / bands;
                for (int b = 0; b < bands; ++b)
                {
                    const ImVec2 bmin(cust_pos.x + b * bw, cust_pos.y);
                    const ImVec2 bmax(cust_pos.x + (b + 1) * bw, cust_pos.y + cust_sz);
                    cdl->AddRectFilledMultiColor(bmin, bmax,
                        kRainbow[b], kRainbow[b + 1], kRainbow[b + 1], kRainbow[b]);
                }
                cdl->AddRect(cust_pos,
                             ImVec2(cust_pos.x + cust_sz, cust_pos.y + cust_sz),
                             shared::theme::to_u32(pal.trim_line), 3.0f, 0, 1.0f);
            }
        }

        // Size and Opacity — ranges from the reference design, relaid as
        // 3-part rows (design size-row/opacity-row). Behaviour is identical.
        int size = s.size_px;
        if (SliderRow("Size", "##size", &size, cursor_finder::kSizeMin, cursor_finder::kSizeMax, "%d px"))
        {
            s.size_px = cursor_finder::clamp_int(size, cursor_finder::kSizeMin, cursor_finder::kSizeMax);
        }
        int opacity = s.opacity_pct;
        if (SliderRow("Opacity", "##opacity", &opacity, cursor_finder::kOpacityMin, cursor_finder::kOpacityMax, "%d%%"))
        {
            s.opacity_pct = cursor_finder::clamp_int(opacity, cursor_finder::kOpacityMin, cursor_finder::kOpacityMax);
        }

        // Outline and Fill apply only to presets that support them — Soft Halo is a
        // single-colour glow, so its outline/fill controls are hidden (the stored
        // values are kept for when the player switches back to a ring preset).
        const PresetCaps& caps = kPresetCaps[static_cast<int>(s.preset)];

        // Fill centre: toggle + opacity + colour, under its own FILL head.
        // The three dependent rows are indented so
        // they read as belonging to the Fill centre toggle.
        if (caps.fill_shape != FillShape::None)
        {
            ImGui::Separator();
            SectionHead("FILL");
            bool fill = s.fill;
            if (ImGui::Checkbox("Fill centre", &fill)) { s.fill = fill; }
            ImGui::Indent();

            // The three fill sub-rows are ALWAYS drawn (design fill-opacity-row:
            // "Dimmed to 0.4, range disabled") — dimmed when Fill is off, not
            // hidden. ImGui 1.80 has no BeginDisabled, and ImGuiItemFlags_Disabled
            // is an imgui_internal.h symbol (forbidden here), so we dim via the
            // public ImGuiStyleVar_Alpha and gate the write-backs on Fill being on.
            // The stored values therefore can't change while Fill is off — the same
            // effective behaviour as the old hidden rows, no schema change.
            const bool fill_on = s.fill;
            if (!fill_on) { ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f); }

            int fs = s.fill_size_pct;
            if (SliderRow("Size", "##fill-size", &fs,
                          cursor_finder::kFillSizeMin, cursor_finder::kFillSizeMax, "%d%%") && fill_on)
            {
                s.fill_size_pct =
                    cursor_finder::clamp_int(fs, cursor_finder::kFillSizeMin, cursor_finder::kFillSizeMax);
            }
            int fo = s.fill_opacity_pct;
            if (SliderRow("Opacity", "##fill-opacity", &fo,
                          cursor_finder::kFillOpacityMin, cursor_finder::kFillOpacityMax, "%d%%") && fill_on)
            {
                s.fill_opacity_pct =
                    cursor_finder::clamp_int(fo, cursor_finder::kFillOpacityMin, cursor_finder::kFillOpacityMax);
            }
            float fc[3]; ToFloat3(s.fill_colour, fc);
            if (ImGui::ColorEdit3("Colour##fill-colour", fc, ImGuiColorEditFlags_NoInputs) && fill_on)
            {
                s.fill_colour = FromFloat3(fc);
            }

            if (!fill_on) { ImGui::PopStyleVar(); }
            ImGui::Unindent();
        } // caps.fill_shape != None

        // Hairline divider (design divider-1) before the outline controls.
        ImGui::Separator();

        // Outline: toggle + colour. (An outline-WIDTH segmented control is
        // deliberately not built here.)
        if (caps.has_outline)
        {
            bool outline = s.outline;
            if (ImGui::Checkbox("Outline", &outline)) { s.outline = outline; }

            // Muted note (design outline-note). The design styles this italic;
            // ImGui's default font has no italic weight, so it renders upright in
            // the muted caption colour — the closest match.
            ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_muted));
            ImGui::TextWrapped("Adds contrast to keep the marker visible on different backgrounds.");
            ImGui::PopStyleColor();

            if (s.outline)
            {
                // Outline-colour swatches (design outline-color-row): 4 preset
                // swatches then the trailing custom picker. Each click writes the
                // SAME s.outline_colour field the ColorEdit3 does — no new state,
                // no schema change. Hexes are literal DATA (like the colour
                // row), not chrome from the palette.
                static const cursor_finder::Rgb kOutlineSwatches[4] = {
                    {0x0A, 0x0A, 0x0E}, // outline-ink (default)
                    {0xF2, 0xF2, 0xF6}, // white
                    {0x96, 0x78, 0x4A}, // gold-line brown
                    {0xFF, 0x2D, 0x9B}, // accent-magenta
                };
                ImDrawList* odl = ImGui::GetWindowDrawList();
                const float osw  = 30.0f; // design swatch
                const float ogap = 8.0f;  // design swatch gap
                for (int i = 0; i < 4; ++i)
                {
                    if (i > 0) { ImGui::SameLine(0.0f, ogap); }
                    const cursor_finder::Rgb& sc = kOutlineSwatches[i];
                    const bool active = (s.outline_colour.r == sc.r &&
                                         s.outline_colour.g == sc.g &&
                                         s.outline_colour.b == sc.b);
                    const ImVec2 pos = ImGui::GetCursorScreenPos();
                    ImGui::PushID(2000 + i);
                    const bool clicked = ImGui::Button("##oswatch", ImVec2(osw, osw));
                    ImGui::PopID();
                    if (clicked) { s.outline_colour = sc; }
                    // Fill with the literal swatch colour; active swatch gets the
                    // themed gold border (same treatment as the colour row).
                    odl->AddRectFilled(pos, ImVec2(pos.x + osw, pos.y + osw),
                                       IM_COL32(sc.r, sc.g, sc.b, 255), 3.0f);
                    odl->AddRect(pos, ImVec2(pos.x + osw, pos.y + osw),
                                 active ? shared::theme::to_u32(pal.button_border)
                                        : shared::theme::to_u32(pal.trim_line),
                                 3.0f, 0, active ? 2.0f : 1.0f);
                }
                // Trailing custom picker: the existing ColorEdit3 (behaviour
                // unchanged — opens the full picker, writes s.outline_colour).
                ImGui::SameLine(0.0f, ogap);
                float oc[3]; ToFloat3(s.outline_colour, oc);
                if (ImGui::ColorEdit3("##outline-custom", oc, ImGuiColorEditFlags_NoInputs))
                {
                    s.outline_colour = FromFloat3(oc);
                }
            }
        }

        // Behaviour. While a mouse button is held (a drag), pin the OS
        // pointer at the press point (Win32 ClipCursor) so the cursor can't drift
        // and can't jump when it reappears; releasing the button unpins it. Kept
        // as a plain checkbox rather than the design's freeze-row.
        ImGui::Separator();
        SectionHead("BEHAVIOUR");
        bool freeze = s.freeze_after_drag;
        if (ImGui::Checkbox("Lock marker while dragging", &freeze))
        {
            s.freeze_after_drag = freeze;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_muted));
        ImGui::TextWrapped(
            "Keeps the marker in place while a mouse button is held. Release the "
            "button to unlock it.");
        ImGui::PopStyleColor();

        // Show overlay: independent visibility per combat state. Out of
        // combat carries the full 3-way choice (Always / While moving / Never),
        // where "While moving" follows the character's movement. In combat is
        // 2-way (Always / Never), so no movement rule can hide the marker during
        // a fight, when it is most needed. Each radio edits the working
        // copy `s`, persisted instantly by the single write-through below.
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_muted));
        ImGui::TextUnformatted("When to show marker");
        ImGui::PopStyleColor();

        const float mode_label_w = 108.0f; // muted label column for the two rows

        // Out of combat: Always / While moving / Never.
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_muted));
        ImGui::TextUnformatted("Out of combat");
        ImGui::PopStyleColor();
        ImGui::SameLine(mode_label_w);
        {
            int oc = static_cast<int>(s.out_of_combat_mode);
            ImGui::RadioButton("Always##oc", &oc,
                               static_cast<int>(cursor_finder::OutOfCombatMode::Always));
            ImGui::SameLine();
            ImGui::RadioButton("While moving##oc", &oc,
                               static_cast<int>(cursor_finder::OutOfCombatMode::WhileMoving));
            ImGui::SameLine();
            ImGui::RadioButton("Never##oc", &oc,
                               static_cast<int>(cursor_finder::OutOfCombatMode::Never));
            s.out_of_combat_mode = static_cast<cursor_finder::OutOfCombatMode>(oc);
        }

        // In combat: Always / Never (no While moving — see the note above).
        ImGui::AlignTextToFramePadding();
        ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_muted));
        ImGui::TextUnformatted("In combat");
        ImGui::PopStyleColor();
        ImGui::SameLine(mode_label_w);
        {
            int ic = static_cast<int>(s.in_combat_mode);
            ImGui::RadioButton("Always##ic", &ic,
                               static_cast<int>(cursor_finder::InCombatMode::Always));
            ImGui::SameLine();
            ImGui::RadioButton("Never##ic", &ic,
                               static_cast<int>(cursor_finder::InCombatMode::Never));
            s.in_combat_mode = static_cast<cursor_finder::InCombatMode>(ic);
        }
    }
    ImGui::EndChild();

    // --- footer: top hairline, caption (left), Reset (right) -------------------
    // Positioned at the bottom of the card. Rendered here (before the preview
    // column) so a Reset click edits `s` before the live preview draws.
    const float footer_y = body_origin.y + body_h;
    dl->AddLine(ImVec2(body_origin.x, footer_y),
                ImVec2(body_origin.x + body_w, footer_y),
                shared::theme::to_u32(pal.trim_line), 1.0f);

    const float reset_w = ImGui::CalcTextSize("Reset to defaults").x
                        + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorScreenPos(ImVec2(body_origin.x, footer_y + 10.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_muted));
    ImGui::TextUnformatted("Changes are applied and saved automatically.");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(
        ImVec2(body_origin.x + body_w - reset_w, footer_y + 8.0f));
    if (ImGui::Button("Reset to defaults"))
    {
        // Reset restores the visual defaults but must PRESERVE welcomed:
        // defaults() has welcomed == false, so a naive reset would re-arm the
        // first-run auto-open on the next launch. Capture it, reset, restore it.
        const bool wasWelcomed = s.welcomed;
        s = cursor_finder::CursorSettings::defaults();
        s.welcomed = wasWelcomed;
    }

    // One write-through per frame: persists only if anything actually changed.
    // Committed AFTER the footer so a Reset edits the same working copy and the
    // preview column below reflects it this frame.
    g_Store->set(s);

    // --- left: preview column (fixed 246 au), rendered LAST -------------------
    // Rendered after the settings/footer edits so its live marker draws on the
    // preview child's own draw list (reliably on top) and reflects this frame's
    // edits instantly. DrawMarker is reused verbatim — pixel-identical.
    ImGui::SetCursorScreenPos(body_origin);
    ImGui::BeginChild("##preview-col", ImVec2(preview_col_w, body_h), false);
    {
        ImDrawList* pdl = ImGui::GetWindowDrawList(); // preview child list (on top)

        SectionHead("LIVE PREVIEW");
        ImGui::Spacing();

        // The square preview canvas: draw the contrast ground, reserve the layout
        // space with a Dummy, then draw the marker on top.
        const ImVec2 box_min = ImGui::GetCursorScreenPos();
        const ImVec2 box_max(box_min.x + preview_box, box_min.y + preview_box);
        DrawPreviewGround(pdl, box_min, box_max,
                          static_cast<PreviewGround>(s_backdrop));
        ImGui::Dummy(ImVec2(preview_box, preview_box));
        DrawMarker(pdl, ImVec2(box_min.x + preview_box * 0.5f,
                               box_min.y + preview_box * 0.5f), s);

        // Pointer glyph: the branded gold arrow near the reticle
        // centre, drawn ON TOP of the marker. The design places its tip at the
        // preview-box centre with a (−3,−3) au offset. Prefer the
        // embedded arrow bitmap (TEX_CURSOR_POINTER, 93×128 PNG, transparent
        // bg); fall back to the hand-drawn glyph until the texture uploads.
        {
            const float box_ratio = preview_box / 214.0f; // au → preview px
            const ImVec2 gtip(box_min.x + preview_box * 0.5f - 3.0f * box_ratio,
                              box_min.y + preview_box * 0.5f - 3.0f * box_ratio);
            if (Texture_t* ptr = ResolvePointerTex(); ptr && ptr->Resource)
            {
                // Comparable to the old ~22×31 au glyph footprint: height ~31 au,
                // width preserves the PNG's 93:128 aspect (w = h * 93/128 ≈ 22.5
                // au). Both scale with the box. Anchor the arrow's top-left corner
                // at the preview-box centre offset by (−3,−3) au, image extending
                // down-right (the original bitmap placement).
                const float  h = 31.0f * box_ratio;
                const float  w = h * (93.0f / 128.0f);
                const ImVec2 center(box_min.x + preview_box * 0.5f,
                                    box_min.y + preview_box * 0.5f);
                const ImVec2 p_min(center.x - 2.0f * box_ratio, center.y - 3.0f * box_ratio);
                const ImVec2 p_max(p_min.x + w, p_min.y + h);
                pdl->AddImage(static_cast<ImTextureID>(ptr->Resource),
                              p_min, p_max);
            }
            else
            {
                // Texture not uploaded yet: fall back to the drawn glyph at the
                // design's rendered 22×31 size (viewbox 24×34 × 22/24).
                const float  gscale = box_ratio * (22.0f / 24.0f);
                const ImVec2 gorigin(gtip.x - 2.0f * gscale, gtip.y - 2.0f * gscale);
                DrawPointerGlyph(pdl, gorigin, gscale);
            }
        }

        ImGui::Spacing();

        // Backdrop toggle strip (design preview-bg-row): 4 equal buttons that
        // swap the preview ground to test marker contrast. A local contrast aid.
        const float strip_gap = 6.0f;
        const float btn_w =
            (ImGui::GetContentRegionAvail().x - strip_gap * (kPreviewGroundCount - 1))
            / kPreviewGroundCount;
        for (int i = 0; i < kPreviewGroundCount; ++i)
        {
            if (i > 0) { ImGui::SameLine(0.0f, strip_gap); }
            const bool active = (s_backdrop == i);
            if (active)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, shared::theme::to_vec4(pal.button_active));
                ImGui::PushStyleColor(ImGuiCol_Border, shared::theme::to_vec4(pal.button_border));
                ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.button_text));
            }
            if (ImGui::Button(kPreviewGroundLabels[i], ImVec2(btn_w, 0.0f)))
            {
                s_backdrop = i;
            }
            if (active) { ImGui::PopStyleColor(3); }
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, shared::theme::to_vec4(pal.text_muted));
        ImGui::TextWrapped(
            "Try different backgrounds to check visibility in-game.");
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    // Right hairline on the preview column's own right edge (design
    // preview-col "Right hairline gold-line @0.2" = the 246-au edge). Drawn on
    // the ##cursor-body list; themed border colour — no per-addon palette fork.
    const float divider_x = body_origin.x + preview_col_w;
    dl->AddLine(ImVec2(divider_x, body_origin.y),
                ImVec2(divider_x, body_origin.y + body_h),
                shared::theme::to_u32(pal.trim_line), 1.0f);
}

// True when the OS mouse cursor is hidden this frame. GW2 hides its own cursor
// for in-world camera / character drags (confirmed in-game)
// and keeps it shown over every window — its own native title bars / resize
// corners and addon ImGui windows alike. So "cursor hidden" is the signal that a
// press is an in-world drag that should freeze, versus a window drag that must not.
// Cheap: one GetCursorInfo call, sampled fresh every frame (the freeze
// is level-based — GW2 hides the cursor a frame or two AFTER the press).
bool CursorIsHidden()
{
    CURSORINFO ci{}; ci.cbSize = sizeof(ci);
    if (!::GetCursorInfo(&ci)) { return false; } // unknown -> treat as shown (don't freeze)
    return (ci.flags & CURSOR_SHOWING) == 0;
}

// One-shot texture-readiness diagnostic: after a few seconds, report how many
// preset layers actually have a live Resource. If this logs 0 ready, the load
// path itself is the problem (not our draw code).
void MaybeLogTexReadiness()
{
    static int  frames = 0;
    static bool logged = false;
    if (logged || !g_API || !g_API->Log) { return; }
    if (++frames < 180) { return; } // ~3s at 60fps
    int colour_ready = 0, outline_ready = 0;
    for (int i = 0; i < 5; ++i)
    {
        if (g_Tex[i].colour  && g_Tex[i].colour->Resource)  { ++colour_ready; }
        if (g_Tex[i].outline && g_Tex[i].outline->Resource) { ++outline_ready; }
    }
    char msg[160];
    std::snprintf(msg, sizeof(msg),
        "cursor-finder: preset textures ready colour=%d/5 outline=%d/5", colour_ready, outline_ready);
    g_API->Log(LOGL_INFO, "Cursor Finder", msg);
    logged = true;
}

// Apply or release the Win32 pointer clip to match the freeze state. On the
// transition into frozen we capture the current screen position — the press
// point — and confine the OS pointer to a 1x1 rect there so it cannot drift for
// the duration of the hold; on the transition out we drop the clip.
// ClipCursor(nullptr) fully unconfines (confirmed OS semantics).
void ApplyClip(bool want_clip)
{
    if (want_clip)
    {
        if (!g_ClipActive)
        {
            // Capture the freeze point ONCE (screen coords) at the transition.
            POINT p;
            if (!::GetCursorPos(&p)) { return; }
            g_ClipRect   = RECT{ p.x, p.y, p.x + 1, p.y + 1 };
            g_ClipActive = true;
            ::ClipCursor(&g_ClipRect);
        }
        else
        {
            // RE-APPLY every frame while frozen. The game re-asserts its own
            // cursor clip each frame (for mouselook), which stomps a one-shot
            // clip on the very next frame — re-clipping to the stored freeze
            // point each frame is what actually holds the pointer.
            ::ClipCursor(&g_ClipRect);
        }
    }
    else if (g_ClipActive)
    {
        ::ClipCursor(nullptr);
        g_ClipActive = false;
    }
}

// WndProc hook (WNDPROC_CALLBACK): the ONLY way Nexus surfaces focus/alt-tab, so
// the load-bearing "never leave the pointer trapped" release rides on it. On any
// loss-of-activation message we drop the clip immediately (ClipCursor is process-
// global and safe to call from this thread) AND flag the render thread to reset
// the state machine so we re-arm cleanly on return. Returns uMsg unchanged so the
// message is never consumed — we only observe, never swallow game input.
UINT OnWndProc(HWND /*hWnd*/, UINT uMsg, WPARAM wParam, LPARAM /*lParam*/)
{
    bool lost = false;
    switch (uMsg)
    {
        case WM_ACTIVATEAPP: lost = (wParam == FALSE); break;
        case WM_KILLFOCUS:   lost = true;              break;
        case WM_ACTIVATE:    lost = (LOWORD(wParam) == WA_INACTIVE); break;
        default: break;
    }
    if (lost)
    {
        ::ClipCursor(nullptr);   // immediate, belt-and-braces
        g_FocusLost.store(true); // render thread resets the state machine + flag
    }
    return uMsg;
}

// Registered as an RT_Render callback; Nexus calls it every frame.
void AddonRender()
{
    MaybeLogTexReadiness();
    // Register / hide the branded toolbar icon per its setting, before the
    // panel-open gate so the button shows with the panel closed.
    EnsureQuickAccessIcon();

    if (g_Store)
    {
        const cursor_finder::CursorSettings& s = g_Store->settings();
        if (s.enabled)
        {
            // Anchor on the OS cursor hotspot — the true click point.
            // The core geometry keeps the marker centered on it.
            const ImVec2 mouse = CurrentPointer();

            // Clip-cursor-while-dragging: read the physical mouse buttons
            // via Win32 (reliable even while the game has mouse capture, unlike
            // io.MouseDown). A drag is any left- OR right-button hold; the state
            // machine pins the OS pointer at the press point for the duration of
            // the hold and releases it on button-up, so the cursor can't drift
            // during the drag and can't jump when it reappears on release.
            const bool button_down =
                (::GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ||
                (::GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;

            // Safety release: if focus was lost mid-hold (WndProc), the button-up
            // may never be seen — force the state machine to let go this frame.
            if (g_FocusLost.exchange(false)) { g_Clip.reset(); }

            // Two guards, both sampled fresh EVERY frame (the freeze is level-based,
            // not latched at the press):
            //  - ui_capturing: addon UI wants the mouse, so don't freeze over it.
            //  - cursor_hidden: only an in-world camera/character drag hides the OS
            //    cursor; window chrome (native title bar / resize corner) keeps it
            //    shown, so freezing only when hidden lets those windows drag.
            const bool ui_capturing  = ImGui::GetIO().WantCaptureMouse;
            const bool cursor_hidden = CursorIsHidden();

            const cursor_finder::Vec2 draw = g_Clip.update(
                s.freeze_after_drag, button_down, cursor_finder::Vec2{mouse.x, mouse.y},
                ui_capturing, cursor_hidden);
            ApplyClip(g_Clip.frozen());
            const ImVec2 center(draw.x, draw.y);

            // Visibility matrix: Nexus's IsGameplay (false at char select
            // and on loading screens), the MumbleLink combat bit and Nexus's
            // IsMoving (character motion) feed the pure cursor-finder-core decision. A
            // missing Nexus link reads as not-in-gameplay and not moving; outside
            // gameplay the marker shows only while the settings window is open.
            // Only the DrawMarker call is gated — the clip-while-dragging
            // behaviour above is unchanged.
            bool in_gameplay = false;
            bool in_combat   = false;
            bool is_moving   = false;
            if (g_API && g_API->DataLink_Get)
            {
                const auto* nexus = static_cast<const NexusLinkData_t*>(
                    g_API->DataLink_Get(DL_NEXUS_LINK));
                in_gameplay = nexus && nexus->IsGameplay;
                is_moving   = nexus && nexus->IsMoving;
                const auto* link = static_cast<const cursor_finder::MumbleLink*>(
                    g_API->DataLink_Get(DL_MUMBLE_LINK));
                if (in_gameplay && link)
                {
                    in_combat = cursor_finder::is_in_combat(link->ContextData.UiState);
                }
            }
            // "While moving" follows the character, with a 1 s linger so the
            // marker doesn't blink on short pauses. ImGui::GetTime() is a
            // monotonic per-frame clock; milliseconds feed the linger window.
            const int now_ms = static_cast<int>(ImGui::GetTime() * 1000.0);
            const bool moving = g_MotionLinger.update(is_moving, now_ms);

            // Foreground draw list = above the addon's own windows; background =
            // below them ("Show over Nexus windows").
            ImDrawList* dl = s.draw_above_windows ? ImGui::GetForegroundDrawList()
                                                  : ImGui::GetBackgroundDrawList();
            if (cursor_finder::should_show_marker(s, in_gameplay, in_combat, moving, g_PanelOpen))
            {
                DrawMarker(dl, center, s);
            }
        }
        else if (g_ClipActive || g_Clip.frozen())
        {
            // Finder switched off while frozen: never leave the pointer pinned.
            g_Clip.reset();
            ApplyClip(false);
        }
    }

    if (!g_PanelOpen) { return; }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    // Fixed-width card: the design's card is 712 au wide.
    // Add the themed window padding on both sides so the interior columns get the
    // measured 246 au preview + fill settings. Height is a comfortable fixed value
    // the settings column scrolls within (the design's card is 973 au tall, but
    // that includes a larger BEHAVIOUR matrix).
    ImGui::SetNextWindowSizeConstraints(ImVec2(712.0f, 400.0f), ImVec2(712.0f, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(712.0f, 700.0f), ImGuiCond_FirstUseEver);

    // Native-look theme: apply the shared theme's colors + metrics
    // around our window only (stack-scoped, so other addons' style is untouched),
    // draw our own native title bar (NoTitleBar) + the themed frame, then run the
    // existing RenderPanel body. No per-addon palette fork.
    const shared::theme::Palette pal = shared::theme::gw2_palette();
    const shared::theme::Metrics met = shared::theme::gw2_metrics();
    const shared::theme::ThemeScope scope =
        shared::theme::PushPanelStyle(pal, met); // before Begin: window vars apply

    // Partial window transparency: drop the ImGui window-bg fill
    // alpha a touch so the card reads consistently see-through with the tinted
    // texture draw below (the game shows through). Paired with the PopStyleColor
    // just before PopPanelStyle.
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
        shared::theme::to_vec4(shared::theme::with_alpha(pal.panel_bg,
                                                         kWindowBgAlpha / 255.0)));

    // NoScrollbar on the outer window: the body scrolls inside its own child
    // (below), so the outer scrollbar never draws over the fixed native title bar
    // / close X.
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoScrollbar;
    if (ImGui::Begin(kWindowName, &g_PanelOpen, flags))
    {
        const ImVec2 w_min  = ImGui::GetWindowPos();
        const ImVec2 w_size = ImGui::GetWindowSize();
        const ImVec2 w_max(w_min.x + w_size.x, w_min.y + w_size.y);
        ImDrawList*  dl = ImGui::GetWindowDrawList();

        // Card background: the reference design's card is a mottled
        // brown->black parchment/rust texture, not a flat fill. If the window
        // texture has uploaded, paint it across the whole card rect, then still
        // draw the themed frame so the gold border rings read on top
        // of it (DrawThemedFrame is border/decoration only — no fill — so it does
        // not paint over the texture). If the texture is not ready (or the texture
        // API is absent), fall back to the primitive DrawThemedFrame exactly as
        // before, so nothing regresses.
        if (Texture_t* bg = ResolveWindowTex(); bg && bg->Resource)
        {
            // Tinted white at kWindowBgAlpha so the textured card is
            // slightly see-through, matching the in-game look.
            dl->AddImage(static_cast<ImTextureID>(bg->Resource), w_min, w_max,
                         ImVec2(0, 0), ImVec2(1, 1),
                         IM_COL32(255, 255, 255, kWindowBgAlpha));
            shared::theme::DrawThemedFrame(dl, w_min, w_max, pal, met);
        }
        else
        {
            shared::theme::DrawThemedFrame(dl, w_min, w_max, pal, met);
        }

        // Title bar: let the window card texture continue
        // seamlessly through the title band. TitleBar normally paints a flat
        // titlebar_bg over the top band, which would hide the parchment drawn
        // above. Pass it a palette whose bar bg is fully transparent, so the
        // textured card shows through; TitleBar still draws its bottom hairline
        // rule, title/subtitle text, and close button on top. (Temporary look —
        // a dedicated title-bar image may replace it later.)

        // Native title bar replaces ImGui's default; clicking its close X closes
        // the panel, matching the previous window-close affordance.
        // No hotkey pill: the toggle ships with no default bind (kDefaultBind),
        // so advertising "C" here would imply a binding that does not exist.
        shared::theme::Palette titlePal = pal;
        titlePal.titlebar_bg = shared::theme::with_alpha(pal.titlebar_bg, 0.0);
        if (shared::theme::TitleBar("Cursor Finder", "Make your cursor easier to find", nullptr,
                                    titlePal, met, kTitleBarHeight,
                                    /*icon_chip=*/false))
        {
            g_PanelOpen = false;
        }

        // Icon override: show the NAKED branded icon on the
        // flat themed bar — no chip, no border. TitleBar is called with
        // icon_chip=false, so it paints nothing in its 30px icon slot (an
        // erase-by-overpaint was translucent and let the chip + glyph
        // bleed through). The slot geometry mirrors TitleBar's own constants
        // (theme_imgui.h): edge 3, bar kTitleBarHeight -> mid_y w_min.y+(3+H)*0.5;
        // icon 30.
        {
            const float  ic_edge  = kChromeEdge;
            const float  ic_mid_y = w_min.y + (ic_edge + kTitleBarHeight) * 0.5f;
            const float  ic_size  = 30.0f;
            const ImVec2 ic_min(w_min.x + ic_edge + 9.0f, ic_mid_y - ic_size * 0.5f);
            const ImVec2 ic_max(ic_min.x + ic_size, ic_min.y + ic_size);

            // Branded icon, drawn naked in the empty slot. Enlarged slightly to
            // fill the icon area nicely now that no chip frames it.
            const float  ic_grow = 3.0f;
            const ImVec2 icn_min(ic_min.x - ic_grow, ic_min.y - ic_grow);
            const ImVec2 icn_max(ic_max.x + ic_grow, ic_max.y + ic_grow);
            if (Texture_t* icon = ResolveIconTex(); icon && icon->Resource)
            {
                // Branded icon fills the icon area; its transparent areas let the
                // flat bar colour show through. The PNG carries its own padding.
                dl->AddImage(static_cast<ImTextureID>(icon->Resource),
                             icn_min, icn_max);
            }
            else
            {
                // Texture not uploaded yet: fall back to the drawn pointer glyph
                // so the icon area is never empty. Its viewbox bbox spans x[2,20],
                // y[2,30] (centre 11,16); put that centre on the icon-area centre.
                const float  ic_scale = 0.72f;
                const float  cx = (icn_min.x + icn_max.x) * 0.5f;
                const float  cy = (icn_min.y + icn_max.y) * 0.5f;
                const ImVec2 ic_origin(cx - 11.0f * ic_scale, cy - 16.0f * ic_scale);
                DrawPointerGlyph(dl, ic_origin, ic_scale);
            }
        }

        // Body scrolls in its own child so any scrollbar stays below the title
        // bar (never over the close X). Transparent child bg so the panel fill
        // shows through (PushPanelStyle's ChildBg is the lighter card colour,
        // which would read as a raised card over the whole body).
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        if (ImGui::BeginChild("##cursor-finder-body", ImVec2(0.0f, 0.0f), false))
        {
            RenderPanel();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::End();

    ImGui::PopStyleColor(); // paired WindowBg transparency override
    shared::theme::PopPanelStyle(scope);
}

// Keybind handler (INPUTBINDS_PROCESS): open/close the settings panel on press.
// The keybind (unbound by default) and the QuickAccess button share this identifier so
// both entry points reach the panel. The finder's on/off lives as a
// checkbox inside the panel (RenderPanel), matching the original design's
// "HOTKEY C" badge on the settings window.
void OnKeybind(const char* /*aIdentifier*/, bool aIsRelease)
{
    if (!aIsRelease) { g_PanelOpen = !g_PanelOpen; }
}

// Registered as RT_OptionsRender: Nexus draws this under the addon's "Configure"
// entry in Addons -> Cursor Finder. This is a SIGNPOST, not a duplicate of the
// panel: a single button that opens the designed floating window (sets
// g_PanelOpen = true), plus a one-line caption. It renders NO settings controls —
// the designed window is the single settings surface.
//
// This preserves *reachability*: with the QuickAccess icon hidden AND the keybind
// unbound, this button is still the way back to settings, so no soft-lockout. The
// entry stays REGISTERED — it is the preserved escape hatch. If flipping
// g_PanelOpen from this callback proves not to take effect before the window's own
// gate, the fallback is a deferred-open flag. Captions wrap, never TextDisabled.
void AddonOptions()
{
    if (ImGui::Button("Open Cursor Finder settings"))
    {
        g_PanelOpen = true;
    }
    // One-line caption (wrapped so it never clips at the Nexus pane width).
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", "Opens the Cursor Finder window, where all settings live.");
    ImGui::PopStyleColor();
}

void AddonLoad(AddonAPI_t* aApi)
{
    g_API = aApi;

    // Adopt Nexus's shared ImGui context + allocators (ImGui 1.80 — raw
    // function-pointer casts, no ImGuiMemAllocFunc typedef).
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(aApi->ImguiContext));
    ImGui::SetAllocatorFunctions(
        reinterpret_cast<void* (*)(size_t, void*)>(aApi->ImguiMalloc),
        reinterpret_cast<void  (*)(void*, void*)>(aApi->ImguiFree));

    // Persist under "<GW2>/addons/cursor-finder/cursor-finder.json" (versioned
    // JSON), moving an older install's "<GW2>/addons/cursor/cursor.json" there
    // once. Both paths come from the addons root, so Nexus is never asked for the
    // old folder (which would re-create it).
    const std::filesystem::path root =
        aApi->Paths_GetAddonDirectory ? aApi->Paths_GetAddonDirectory(nullptr)
                                      : std::filesystem::path("addons");
    g_Store = new cursor_finder::CursorStore(cursor_finder::resolve_settings_path(
        cursor_finder::settings_file(root), cursor_finder::legacy_settings_file(root)));

    // One-time first-run auto-open. On a brand-new install the
    // store has no file, so it loaded defaults() with welcomed == false -> open the
    // designed window once so the user's first contact is the designed UI. Then set
    // welcomed and PERSIST IT IMMEDIATELY via the store's write-through (set()) — we
    // must NOT wait for an incidental set() from the user changing another setting,
    // or a first-run user who opens+closes without touching anything would re-trigger
    // the auto-open every launch. This is a load-time
    // one-shot, not a per-frame/keybind auto-open. An existing install migrated
    // forward as already-welcomed (cursor_finder_store), so this never fires on upgrade.
    {
        cursor_finder::CursorSettings s = g_Store->settings();
        if (cursor_finder::should_first_run_open(s))
        {
            g_PanelOpen = true;
            s.welcomed  = true;
            g_Store->set(s); // write-through: welcomed persists before we return
        }
    }

    if (aApi->Log)
    {
        char msg[160];
        std::snprintf(msg, sizeof(msg),
            "cursor-finder: Textures_GetOrCreateFromMemory=%p (loading %u preset layers from memory)",
            reinterpret_cast<void*>(aApi->Textures_GetOrCreateFromMemory), 10u);
        aApi->Log(LOGL_INFO, "Cursor Finder", msg);
    }

    aApi->GUI_Register(RT_Render, AddonRender);
    // Also register the settings under the Nexus Options page so they
    // stay reachable from Addons -> Cursor Finder -> Configure even when the
    // QuickAccess icon is hidden and the keybind is unbound (no soft-lockout).
    aApi->GUI_Register(RT_OptionsRender, AddonOptions);

    // Warm the window-background texture once at Load so the parchment card fill
    // is ready by first render. Same lazy memory loader as the preset
    // layers; if it is not ready yet the first frame draws the primitive frame.
    ResolveWindowTex();
    ResolveIconTex();     // warm the branded title-chip icon
    ResolvePointerTex();  // warm the branded preview arrow

    if (aApi->InputBinds_RegisterWithString)
    {
        aApi->InputBinds_RegisterWithString(kKeybindId, OnKeybind, kDefaultBind);
    }
    // Focus/alt-tab hook — safety release of the pointer clip if the game loses
    // activation mid-hold, so a never-seen button-up can't leave it trapped.
    if (aApi->WndProc_Register) { aApi->WndProc_Register(OnWndProc); }
    // The branded QuickAccess button is deliberately NOT added here. Its
    // icon textures only resolve after the render device is up (later than load),
    // and the "Show quick-access icon" setting governs whether it shows at all —
    // both are handled from the render loop (see EnsureQuickAccessIcon).

    if (aApi->Log) { aApi->Log(LOGL_INFO, "Cursor Finder", "cursor-finder addon loaded"); }
}

void AddonUnload()
{
    // Release any active pointer clip FIRST — the pointer must never be left
    // confined after the addon goes away, even if the deregisters below fail.
    ::ClipCursor(nullptr);
    g_ClipActive = false;
    g_Clip.reset();

    if (g_API)
    {
        if (g_API->GUI_Deregister)        { g_API->GUI_Deregister(AddonRender); }
        if (g_API->GUI_Deregister)        { g_API->GUI_Deregister(AddonOptions); }
        if (g_API->InputBinds_Deregister) { g_API->InputBinds_Deregister(kKeybindId); }
        if (g_API->WndProc_Deregister)    { g_API->WndProc_Deregister(OnWndProc); }
        if (g_API->QuickAccess_Remove)    { g_API->QuickAccess_Remove(kQuickAccessId); }
    }
    // Reset so a reload re-registers the button once the icon re-resolves
    // (QuickAccess_Remove above already dropped any live registration).
    g_QuickAccessAdded = false;
    g_IconTex      = nullptr;
    g_IconHoverTex = nullptr;

    // Best-effort final flush (belt-and-braces). Durability does not depend
    // on this — every edit was already written through.
    if (g_Store) { g_Store->flush(); }

    delete g_Store;
    g_Store = nullptr;
    g_API = nullptr;
}

} // namespace

extern "C" __declspec(dllexport) AddonDefinition_t* GetAddonDef()
{
    g_AddonDef.Signature   = 0x63757273; // "curs" — unique addon signature
    g_AddonDef.APIVersion  = NEXUS_API_VERSION;
    g_AddonDef.Name        = "Cursor Finder";
    g_AddonDef.Version     = AddonVersion_t{ 1, 0, 1, 0 };
    g_AddonDef.Author      = "Fuchsia Llama Mama";
    g_AddonDef.Description = "A customizable highlight centered on the mouse pointer so the cursor stays easy to find in busy scenes.";
    g_AddonDef.Load        = AddonLoad;
    g_AddonDef.Unload      = AddonUnload;
    g_AddonDef.Flags       = AF_None;
    // Nexus auto-update from GitHub releases: Nexus picks the release whose tag
    // (vMAJOR.MINOR.PATCH) is highest and downloads its first .dll asset, so the
    // tag must match Version above.
    g_AddonDef.Provider    = UP_GitHub;
    g_AddonDef.UpdateLink  = "https://github.com/FuchsiaLlamaMama/gw2-nexus-plugin-cursor-finder";
    return &g_AddonDef;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ulReasonForCall, LPVOID /*lpReserved*/)
{
    switch (ulReasonForCall)
    {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH: break;
    }
    return TRUE;
}
