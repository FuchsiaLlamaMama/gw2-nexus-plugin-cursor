// cursor-core unit tests (doctest) — the off-game, pure-logic surface of the
// Cursor Finder addon.
//
// These cover: settings defaults, JSON round-trip + write-through durability,
// schema versioning + forward migration (v1->v7), field clamping, colour hex
// round-trip, Reset-to-defaults, pointer geometry centering, the ClipFreeze
// (freeze-while-dragging) state machine, the combat/movement
// visibility matrix (is_in_combat, pointer-motion predicate, should_show_marker,
// mode slug round-trip + migration), and the first-run `welcomed` flag
// (should_first_run_open, v6->v7 migration as already-welcomed for existing
// installs, Reset-preserves-welcomed). The Windows/ImGui surface (rendering,
// keybind, QuickAccess, panel, live preview, textures, the Win32 ClipCursor
// lifetime) is the manual in-game portion and is NOT asserted here.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <type_traits>

#include <nlohmann/json.hpp>

#include "core/cursor_store.h"
#include "core/marker.h"
#include "core/clip_freeze.h"
#include "core/visibility.h"
#include "persistence/atomic_file.h"
#include "theme/theme.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// A unique temp path per test, cleaned up on destruction.
struct TempStorePath {
    fs::path path;
    TempStorePath()
    {
        static int counter = 0;
        path = fs::temp_directory_path() /
               ("cursor-core-test-" + std::to_string(counter++) + ".json");
        std::error_code ec;
        fs::remove(path, ec);
    }
    ~TempStorePath()
    {
        std::error_code ec;
        fs::remove(path, ec);
    }
};

std::string read_disk(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

} // namespace

// --- defaults on first run ----------------------------------------------------

TEST_CASE("a missing file yields the factory-default settings")
{
    TempStorePath tmp; // path does not exist
    REQUIRE_FALSE(fs::exists(tmp.path));

    cursor::CursorStore store(tmp.path); // must not throw
    // Design defaults: finder ON, drawn above Nexus windows.
    CHECK(store.settings().enabled);
    CHECK(store.settings().draw_above_windows);
    CHECK(store.settings() == cursor::CursorSettings::defaults());
}

// --- serialize -> write -> read -> deserialize round-trip ---------------------

TEST_CASE("settings round-trip through disk unchanged")
{
    TempStorePath tmp;
    {
        cursor::CursorStore store(tmp.path);
        store.set_enabled(false);
        store.set_draw_above_windows(false);
    }
    // Fresh store on the same path == a next-session reload.
    cursor::CursorStore reloaded(tmp.path);
    CHECK_FALSE(reloaded.settings().enabled);
    CHECK_FALSE(reloaded.settings().draw_above_windows);
}

// --- write-through — disk reflects a change with NO explicit flush ------------

TEST_CASE("mutations are written through to disk without an explicit flush")
{
    TempStorePath tmp;
    cursor::CursorStore store(tmp.path);

    CHECK(store.set_enabled(false)); // returns true: value changed, no flush() call

    REQUIRE(fs::exists(tmp.path));
    json on_disk = json::parse(read_disk(tmp.path));
    CHECK(on_disk["enabled"] == false);

    CHECK(store.set_draw_above_windows(false));
    on_disk = json::parse(read_disk(tmp.path));
    CHECK(on_disk["draw_above_windows"] == false);

    // A no-op set (same value) reports no change.
    CHECK_FALSE(store.set_enabled(false));
}

// --- corrupt file -> graceful recovery to defaults (no throw) -----------------

TEST_CASE("a corrupt/unparseable file recovers to defaults without throwing")
{
    TempStorePath tmp;
    {
        std::ofstream out(tmp.path, std::ios::binary);
        out << "{ not valid json ]] :::";
    }

    cursor::CursorStore store(tmp.path); // a throw would fail the run
    CHECK(store.settings() == cursor::CursorSettings::defaults());

    // And the store is usable afterwards: a new write overwrites the garbage.
    store.set_enabled(false);
    cursor::CursorStore reloaded(tmp.path);
    CHECK_FALSE(reloaded.settings().enabled);
}

// --- schema version present --------------------------------------------------

TEST_CASE("the persisted record carries a top-level schema version")
{
    TempStorePath tmp;
    cursor::CursorStore store(tmp.path);
    store.set_enabled(false); // force a write

    json on_disk = json::parse(read_disk(tmp.path));
    REQUIRE(on_disk.contains("schema_version"));
    CHECK(on_disk["schema_version"] == cursor::CursorSettings::kSchemaVersion);
}

// --- forward migration — unknown/extra fields load, absent default ------------

TEST_CASE("a later-schema file with extra fields loads forward-compatibly")
{
    TempStorePath tmp;
    // Simulate a file written by a *later* version: a bumped version, a field this
    // build has never heard of, and only `enabled`
    // set. The unknown field must be ignored, `enabled` honoured, and every
    // absent field fall back to its default — no data loss, no rejection.
    {
        json doc;
        doc["schema_version"]  = cursor::CursorSettings::kSchemaVersion + 5;
        doc["enabled"]         = false;
        doc["future_matrix"]   = json::array();    // unknown here
        std::ofstream out(tmp.path, std::ios::binary);
        out << doc.dump(2);
    }

    cursor::CursorStore store(tmp.path);
    CHECK_FALSE(store.settings().enabled);           // honoured
    CHECK(store.settings().draw_above_windows);      // absent -> default (true)
}

TEST_CASE("a pre-versioned file (no schema_version) migrates forward")
{
    TempStorePath tmp;
    // A hypothetical unversioned record: known fields present, no schema_version.
    // It must load its fields and be re-stamped at the current version on save.
    {
        std::ofstream out(tmp.path, std::ios::binary);
        out << R"({"enabled":false,"draw_above_windows":true})";
    }

    cursor::CursorStore store(tmp.path);
    CHECK_FALSE(store.settings().enabled);
    CHECK(store.settings().draw_above_windows);

    // Any mutation re-stamps the file at the current schema version.
    store.set_enabled(true);
    json on_disk = json::parse(read_disk(tmp.path));
    CHECK(on_disk["schema_version"] == cursor::CursorSettings::kSchemaVersion);
}

TEST_CASE("a malformed field degrades to its default, the rest loads")
{
    TempStorePath tmp;
    {
        // `enabled` wrong-typed (string) must degrade to the default, not throw;
        // `draw_above_windows` is a valid bool and is honoured.
        std::ofstream out(tmp.path, std::ios::binary);
        out << R"({"schema_version":1,"enabled":"yes","draw_above_windows":false})";
    }
    cursor::CursorStore store(tmp.path);
    CHECK(store.settings().enabled);                 // malformed -> default (true)
    CHECK_FALSE(store.settings().draw_above_windows); // valid -> honoured
}

// --- appearance defaults ------------------------------------------------------

TEST_CASE("appearance factory defaults match the design")
{
    const cursor::CursorSettings s = cursor::CursorSettings::defaults();
    CHECK(s.preset == cursor::Preset::PulseRing);          // default
    CHECK(s.colour == cursor::signature_hue(cursor::Preset::PulseRing));
    CHECK(s.colour == cursor::Rgb{0xff, 0x2d, 0x9b});      // magenta
    CHECK(s.size_px == 96);                                 // design default
    CHECK(s.opacity_pct == 90);                             // design default
    CHECK(s.outline);                                       // on by default
    CHECK(s.outline_colour == cursor::Rgb{0x14, 0x14, 0x18}); // dark default
    CHECK_FALSE(s.fill);                                    // off by default
    CHECK(s.fill_opacity_pct == 35);                        // design default
    CHECK(s.fill_colour == cursor::Rgb{0xf2, 0xf2, 0xf6});  // near-white
    CHECK(s.fill_size_pct == 70);                           // v3: fill size %
    CHECK(s.show_quick_access_icon);                        // v5: on by default
    CHECK(s.out_of_combat_mode == cursor::OutOfCombatMode::Always); // v6: shown by default
    CHECK(s.in_combat_mode == cursor::InCombatMode::Always);        // v6: shown by default
    CHECK_FALSE(s.welcomed);                                        // v7: a fresh install is NOT yet welcomed
    // Schema bumped for the first-run welcomed flag (v7 added welcomed).
    CHECK(cursor::CursorSettings::kSchemaVersion == 7);
}

// --- every appearance field round-trips through disk ---------------------------

TEST_CASE("every appearance field round-trips through disk")
{
    TempStorePath tmp;
    cursor::CursorSettings edited = cursor::CursorSettings::defaults();
    edited.preset           = cursor::Preset::SoftHalo;
    edited.colour           = cursor::Rgb{0x12, 0x34, 0x56};
    edited.size_px          = 88;  // within [kSizeMin, kSizeMax]
    edited.opacity_pct      = 55;
    edited.outline          = false;
    edited.outline_colour   = cursor::Rgb{0xab, 0xcd, 0xef};
    edited.fill             = true;
    edited.fill_opacity_pct = 80;
    edited.fill_colour      = cursor::Rgb{0x00, 0xff, 0x00};
    edited.fill_size_pct    = 45;
    edited.freeze_after_drag = true; // v4 field
    edited.show_quick_access_icon = false; // v5 field
    {
        cursor::CursorStore store(tmp.path);
        CHECK(store.set(edited)); // write-through, value changed
    }
    cursor::CursorStore reloaded(tmp.path);
    CHECK(reloaded.settings() == edited); // all fields preserved next session
}

TEST_CASE("appearance mutations are written through with no explicit flush")
{
    TempStorePath tmp;
    cursor::CursorStore store(tmp.path);

    cursor::CursorSettings next = store.settings();
    next.preset = cursor::Preset::RadarDash;
    next.colour = cursor::Rgb{0x0a, 0x0b, 0x0c};
    CHECK(store.set(next));

    json on_disk = json::parse(read_disk(tmp.path));
    CHECK(on_disk["preset"] == "radar_dash");
    CHECK(on_disk["colour"] == "#0a0b0c");
    CHECK(on_disk["schema_version"] == 7);
}

// --- the v1 -> v2 migration ---------------------------------------------------

TEST_CASE("a v1 file loads with appearance defaulted, then re-stamps v2")
{
    TempStorePath tmp;
    // Exactly what a v1 build wrote: schema 1, only the two v1 fields.
    {
        std::ofstream out(tmp.path, std::ios::binary);
        out << R"({"schema_version":1,"enabled":false,"draw_above_windows":false})";
    }
    cursor::CursorStore store(tmp.path);
    // v1 fields honoured...
    CHECK_FALSE(store.settings().enabled);
    CHECK_FALSE(store.settings().draw_above_windows);
    // ...and every absent appearance field falls back to its default (no loss).
    CHECK(store.settings().preset == cursor::Preset::PulseRing);
    CHECK(store.settings().colour == cursor::Rgb{0xff, 0x2d, 0x9b});
    CHECK(store.settings().size_px == 96);
    CHECK(store.settings().outline);
    CHECK_FALSE(store.settings().fill);

    // Any mutation re-stamps the file at v2 with the appearance keys present.
    store.set_enabled(true);
    json on_disk = json::parse(read_disk(tmp.path));
    CHECK(on_disk["schema_version"] == 7);
    REQUIRE(on_disk.contains("preset"));
    CHECK(on_disk["preset"] == "pulse_ring");
    CHECK(on_disk["outline"] == true);
}

// --- malformed appearance fields degrade to defaults --------------------------

TEST_CASE("malformed appearance fields degrade to defaults, valid ones honoured")
{
    TempStorePath tmp;
    {
        // unknown preset slug, non-hex colour, wrong-typed size, valid opacity.
        std::ofstream out(tmp.path, std::ios::binary);
        out << R"({"schema_version":2,"preset":"laser_dorito","colour":"not-a-hex",)"
               R"("size_px":"big","opacity_pct":40})";
    }
    cursor::CursorStore store(tmp.path);
    CHECK(store.settings().preset == cursor::Preset::PulseRing);       // unknown -> default
    CHECK(store.settings().colour == cursor::Rgb{0xff, 0x2d, 0x9b});   // bad hex -> default
    CHECK(store.settings().size_px == 96);                             // wrong type -> default
    CHECK(store.settings().opacity_pct == 40);                         // valid -> honoured
}

// --- out-of-range numbers are clamped on read --------------------------------

TEST_CASE("out-of-range size/opacity/fill-opacity are clamped on read")
{
    TempStorePath tmp;
    {
        std::ofstream out(tmp.path, std::ios::binary);
        out << R"({"schema_version":2,"size_px":5,"opacity_pct":0,"fill_opacity_pct":250,"fill_size_pct":3})";
    }
    cursor::CursorStore lo(tmp.path);
    CHECK(lo.settings().size_px == cursor::kSizeMin);          // 5 -> 40
    CHECK(lo.settings().opacity_pct == cursor::kOpacityMin);   // 0 -> 20
    CHECK(lo.settings().fill_opacity_pct == cursor::kFillOpacityMax); // 250 -> 100
    CHECK(lo.settings().fill_size_pct == cursor::kFillSizeMin); // 3 -> 10

    {
        std::ofstream out(tmp.path, std::ios::binary);
        out << R"({"schema_version":2,"size_px":9999})";
    }
    cursor::CursorStore hi(tmp.path);
    CHECK(hi.settings().size_px == cursor::kSizeMax);          // 9999 -> 100
}

// --- Reset to defaults --------------------------------------------------------

TEST_CASE("reset to defaults restores the default appearance")
{
    TempStorePath tmp;
    cursor::CursorStore store(tmp.path);

    cursor::CursorSettings messed = store.settings();
    messed.preset = cursor::Preset::BeaconCrosshair;
    messed.size_px = 100;
    messed.outline = false;
    messed.fill = true;
    CHECK(store.set(messed));

    // Reset == writing the factory defaults through the store (what the panel's
    // "Reset to defaults" button does).
    CHECK(store.set(cursor::CursorSettings::defaults()));
    CHECK(store.settings() == cursor::CursorSettings::defaults());

    cursor::CursorStore reloaded(tmp.path); // and it persisted
    CHECK(reloaded.settings() == cursor::CursorSettings::defaults());
}

// --- preset slug <-> enum round-trip ------------------------------------------

TEST_CASE("every preset slug round-trips through the enum")
{
    for (const cursor::Preset p : {cursor::Preset::PulseRing,
                                   cursor::Preset::CornerReticle,
                                   cursor::Preset::BeaconCrosshair,
                                   cursor::Preset::RadarDash,
                                   cursor::Preset::SoftHalo})
    {
        const auto back = cursor::preset_from_slug(cursor::preset_to_slug(p));
        REQUIRE(back.has_value());
        CHECK(*back == p);
    }
    CHECK_FALSE(cursor::preset_from_slug("nope").has_value());
}

// --- pointer geometry centering ----------------------------------------------

TEST_CASE("the marker rect is centered on the point at several sizes")
{
    struct Case { float cx, cy, size; };
    for (const Case c : {Case{0, 0, 10}, Case{100, 200, 50},
                         Case{960, 540, 200}, Case{-30, 15, 64}})
    {
        const cursor::MarkerRect r =
            cursor::centered_marker_rect(c.cx, c.cy, c.size);
        // Centered on the click point.
        CHECK(r.center_x() == doctest::Approx(c.cx));
        CHECK(r.center_y() == doctest::Approx(c.cy));
        // Full side == size, square.
        CHECK(r.width()  == doctest::Approx(c.size));
        CHECK(r.height() == doctest::Approx(c.size));
        // Symmetric about the centre.
        CHECK(r.min_x == doctest::Approx(c.cx - c.size / 2));
        CHECK(r.max_x == doctest::Approx(c.cx + c.size / 2));
    }
}

TEST_CASE("a non-positive marker size yields a degenerate rect at the point")
{
    const cursor::MarkerRect r = cursor::centered_marker_rect(50, 60, 0);
    CHECK(r.width()  == doctest::Approx(0.0f));
    CHECK(r.height() == doctest::Approx(0.0f));
    CHECK(r.center_x() == doctest::Approx(50));
    CHECK(r.center_y() == doctest::Approx(60));
}

TEST_CASE("the pulse-ring radius is half the marker size")
{
    CHECK(cursor::pulse_ring_radius(200) == doctest::Approx(100.0f));
    CHECK(cursor::pulse_ring_radius(0)   == doctest::Approx(0.0f));
    CHECK(cursor::pulse_ring_radius(-5)  == doctest::Approx(0.0f));
}

// --- clip state machine test helper -------------------------------------------

namespace {
// Assert a returned draw position.
void expect_at(cursor::Vec2 got, float x, float y)
{
    CHECK(got.x == doctest::Approx(x));
    CHECK(got.y == doctest::Approx(y));
}
} // namespace

TEST_CASE("a v3 file (no freeze_after_drag) migrates forward to v4 with the default")
{
    TempStorePath tmp;
    {
        std::ofstream out(tmp.path);
        out << R"({"schema_version":3,"enabled":true,"draw_above_windows":true,)"
               R"("preset":"pulse_ring","colour":"#ff2d9b","size_px":96,)"
               R"("opacity_pct":90,"outline":true,"outline_colour":"#141418",)"
               R"("fill":false,"fill_opacity_pct":35,"fill_colour":"#f2f2f6",)"
               R"("fill_size_pct":70})";
    }
    cursor::CursorStore store(tmp.path);
    CHECK(store.settings().freeze_after_drag == false); // absent -> default

    // Re-stamped at v4 with the field present on the next write-through.
    cursor::CursorSettings next = store.settings();
    next.freeze_after_drag = true;
    CHECK(store.set(next));
    json on_disk = json::parse(read_disk(tmp.path));
    CHECK(on_disk["schema_version"] == 7); // re-stamped at the current version
    CHECK(on_disk["freeze_after_drag"] == true);
}

TEST_CASE("a v4 file (no show_quick_access_icon) migrates forward to v5 with the default")
{
    TempStorePath tmp;
    {
        // Exactly what a v4 build wrote: schema 4, every field up to freeze_after_drag,
        // but no show_quick_access_icon.
        std::ofstream out(tmp.path);
        out << R"({"schema_version":4,"enabled":true,"draw_above_windows":true,)"
               R"("preset":"pulse_ring","colour":"#ff2d9b","size_px":96,)"
               R"("opacity_pct":90,"outline":true,"outline_colour":"#141418",)"
               R"("fill":false,"fill_opacity_pct":35,"fill_colour":"#f2f2f6",)"
               R"("fill_size_pct":70,"freeze_after_drag":false})";
    }
    cursor::CursorStore store(tmp.path);
    CHECK(store.settings().show_quick_access_icon == true); // absent -> default on

    // Re-stamped at v5 with the field present on the next write-through, and a
    // toggle to false survives a reload (the change survives a restart).
    cursor::CursorSettings next = store.settings();
    next.show_quick_access_icon = false;
    CHECK(store.set(next));
    json on_disk = json::parse(read_disk(tmp.path));
    CHECK(on_disk["schema_version"] == 7);
    CHECK(on_disk["show_quick_access_icon"] == false);

    cursor::CursorStore reloaded(tmp.path);
    CHECK(reloaded.settings().show_quick_access_icon == false);
}

// --- clip-cursor-while-dragging state machine ---------------------------------
//
// The freeze is level-based: it pins the pointer while a button is held AND the OS
// cursor is hidden (an in-world drag) AND addon UI isn't capturing — re-evaluated
// every frame, released when any of those stops or the button lifts. The
// default cursor_hidden=true in these basic cases models an in-world press.
// Letting go of the drag IS the release — no hotkey, no resume-on-movement.

TEST_CASE("clip: toggle OFF is a pass-through: never freezes")
{
    cursor::ClipFreeze f;
    expect_at(f.update(false, true,  {100, 100}), 100, 100);
    expect_at(f.update(false, true,  {140, 100}), 140, 100);
    expect_at(f.update(false, false, {140, 100}), 140, 100);
    CHECK_FALSE(f.frozen());
}

TEST_CASE("clip: button-down freezes the pointer at the press point")
{
    cursor::ClipFreeze f;
    expect_at(f.update(true, true, {100, 100}), 100, 100); // press => freeze here
    CHECK(f.frozen());
    CHECK(f.frozen_pos().x == doctest::Approx(100));
    CHECK(f.frozen_pos().y == doctest::Approx(100));
}

TEST_CASE("clip: the pointer stays pinned at the press point for the whole hold")
{
    cursor::ClipFreeze f;
    f.update(true, true, {100, 100}); // press at (100,100)
    REQUIRE(f.frozen());
    // The live pointer moving while held must NOT move the marker: it stays pinned
    // at the press point (mirrors the OS clip holding the cursor there).
    expect_at(f.update(true, true, {130, 100}), 100, 100);
    expect_at(f.update(true, true, {400, 400}), 100, 100);
    CHECK(f.frozen());
}

TEST_CASE("clip: button-up releases the freeze — letting go IS the release")
{
    cursor::ClipFreeze f;
    f.update(true, true, {100, 100}); // press
    f.update(true, true, {130, 100}); // hold
    REQUIRE(f.frozen());
    // Button up: unfreeze and track the live pointer again.
    expect_at(f.update(true, false, {130, 100}), 130, 100);
    CHECK_FALSE(f.frozen());
}

TEST_CASE("clip: a quick click freezes only for the held frame(s)")
{
    cursor::ClipFreeze f;
    expect_at(f.update(true, true,  {100, 100}), 100, 100); // press (frozen)
    CHECK(f.frozen());
    expect_at(f.update(true, false, {100, 100}), 100, 100); // release
    CHECK_FALSE(f.frozen());
}

TEST_CASE("clip: a fresh press after release re-freezes at the NEW press point")
{
    cursor::ClipFreeze f;
    f.update(true, true,  {100, 100});
    f.update(true, false, {100, 100}); // released
    REQUIRE_FALSE(f.frozen());
    expect_at(f.update(true, true, {250, 260}), 250, 260); // new press
    CHECK(f.frozen());
    CHECK(f.frozen_pos().x == doctest::Approx(250));
    CHECK(f.frozen_pos().y == doctest::Approx(260));
}

TEST_CASE("clip: turning the toggle off mid-hold releases the pointer")
{
    cursor::ClipFreeze f;
    f.update(true, true, {100, 100});
    f.update(true, true, {130, 100});
    REQUIRE(f.frozen());
    expect_at(f.update(false, true, {130, 100}), 130, 100); // toggle off, still held
    CHECK_FALSE(f.frozen());
}

// A press captured by another addon's ImGui window must NOT freeze the
// pointer, or that window can't be dragged (moved, sliders, scrollbars). The
// freeze is for in-world clicks only.
TEST_CASE("clip: a press captured by addon UI does NOT freeze — UI drags stay free")
{
    cursor::ClipFreeze f;
    // Press lands on addon UI (WantCaptureMouse == true): no freeze.
    expect_at(f.update(true, true, {100, 100}, /*ui_capturing=*/true), 100, 100);
    CHECK_FALSE(f.frozen());
    // Dragging the window: the live pointer is tracked, never pinned.
    expect_at(f.update(true, true, {140, 120}, /*ui_capturing=*/true), 140, 120);
    CHECK_FALSE(f.frozen());
    expect_at(f.update(true, true, {180, 160}, /*ui_capturing=*/true), 180, 160);
    CHECK_FALSE(f.frozen());
    // Release stays free too.
    expect_at(f.update(true, false, {180, 160}, /*ui_capturing=*/true), 180, 160);
    CHECK_FALSE(f.frozen());
}

// Addon UI is excluded even when the cursor is hidden — the belt-and-
// braces WantCaptureMouse guard wins, so an addon window drag never freezes.
TEST_CASE("clip: addon UI is never frozen, even with the cursor hidden")
{
    cursor::ClipFreeze f;
    expect_at(f.update(true, true, {100, 100}, /*ui_capturing=*/true, /*cursor_hidden=*/true),
              100, 100);
    CHECK_FALSE(f.frozen());
    expect_at(f.update(true, true, {140, 120}, /*ui_capturing=*/true, /*cursor_hidden=*/true),
              140, 120);
    CHECK_FALSE(f.frozen());
}

// The freeze must only engage for an in-world camera/character drag,
// detected by the OS cursor being hidden. A press on a GW2 native window (title
// bar / resize corner) keeps the cursor SHOWN, so it must NOT freeze — otherwise
// the window can't be moved or resized.
TEST_CASE("clip: an in-world drag (cursor hidden) still freezes")
{
    cursor::ClipFreeze f;
    // Cursor hidden = GW2 in-world camera/character drag: freeze as designed.
    expect_at(f.update(true, true, {100, 100}, /*ui_capturing=*/false, /*cursor_hidden=*/true),
              100, 100);
    CHECK(f.frozen());
    expect_at(f.update(true, true, {140, 130}, false, /*cursor_hidden=*/true), 100, 100);
    CHECK(f.frozen());
}

TEST_CASE("clip: a native-window press (cursor shown) does NOT freeze — windows drag")
{
    cursor::ClipFreeze f;
    // Cursor shown = press is over window chrome (native title bar / resize corner):
    // never freeze, so the window follows the pointer.
    expect_at(f.update(true, true, {100, 100}, /*ui_capturing=*/false, /*cursor_hidden=*/false),
              100, 100);
    CHECK_FALSE(f.frozen());
    expect_at(f.update(true, true, {160, 140}, false, /*cursor_hidden=*/false), 160, 140);
    CHECK_FALSE(f.frozen());
    expect_at(f.update(true, false, {160, 140}, false, /*cursor_hidden=*/false), 160, 140);
    CHECK_FALSE(f.frozen());
}

// The core regression: GW2 hides the cursor a frame or two AFTER the
// press, once it registers the drag — so the freeze must be re-checked every frame,
// not latched at the press. A left-drag that starts with the cursor shown and hides
// mid-hold MUST freeze the moment it hides. (A press-edge-only check saw "shown" on
// frame 1 and never froze, so a character turn was never frozen.)
TEST_CASE("clip: left-drag freezes once the cursor hides mid-hold, not just at press")
{
    cursor::ClipFreeze f;
    // Frame 1: button just pressed, GW2 hasn't hidden the cursor yet.
    expect_at(f.update(true, true, {100, 100}, false, /*cursor_hidden=*/false), 100, 100);
    CHECK_FALSE(f.frozen());
    // Frame 2: GW2 registers the drag and hides the cursor -> freeze engages now.
    expect_at(f.update(true, true, {120, 110}, false, /*cursor_hidden=*/true), 120, 110);
    CHECK(f.frozen());
    // Stays pinned for the rest of the hidden hold.
    expect_at(f.update(true, true, {400, 400}, false, /*cursor_hidden=*/true), 120, 110);
    CHECK(f.frozen());
    // Button up releases.
    expect_at(f.update(true, false, {400, 400}, false, /*cursor_hidden=*/true), 400, 400);
    CHECK_FALSE(f.frozen());
}

// --- combat & movement visibility --------------------------------------------
//
// The pure decision logic behind the "Show overlay" matrix: the combat-bit
// predicate over MumbleContext.UiState, the out-of-combat "While moving" pointer
// tracker, and the should_show_marker matrix (five cells + master-off + out-of-gameplay).
// The MumbleLink read + per-frame ImGui plumbing is the Windows/in-game surface,
// not asserted here.

TEST_CASE("is_in_combat reads UiState bit 6 / 0x40")
{
    CHECK(cursor::is_in_combat(0x40u));           // the combat bit alone
    CHECK_FALSE(cursor::is_in_combat(0x00u));      // nothing set
    // Neighbouring bits must not false-trigger combat.
    CHECK_FALSE(cursor::is_in_combat(0x01u));      // IsMapOpen (bit 0)
    CHECK_FALSE(cursor::is_in_combat(0x10u));      // IsInCompetitiveGamemode
    CHECK_FALSE(cursor::is_in_combat(0xFFFFFFFFu & ~0x40u)); // every bit but combat
    // The combat bit reads set even alongside other flags.
    CHECK(cursor::is_in_combat(0x41u));            // combat + map-open
    CHECK(cursor::is_in_combat(0xFFFFFFFFu));      // all bits
}

TEST_CASE("pointer_active: motion above threshold shows; stillness past the linger hides")
{
    const float thr    = 3.0f;
    const int   linger = 1000;
    // Moving this frame (delta above threshold) -> active regardless of the timer.
    CHECK(cursor::pointer_active(10.0f, 0.0f, 5000, thr, linger));
    CHECK(cursor::pointer_active(0.0f, 10.0f, 5000, thr, linger));
    // Still this frame, but within the linger window since the last move -> active.
    CHECK(cursor::pointer_active(0.0f, 0.0f, 500, thr, linger));
    // Still this frame, past the linger window -> inactive.
    CHECK_FALSE(cursor::pointer_active(0.0f, 0.0f, 1500, thr, linger));
    // A sub-threshold jitter is not a move; past linger -> inactive.
    CHECK_FALSE(cursor::pointer_active(2.0f, 0.0f, 1500, thr, linger));
}

TEST_CASE("PointerMotion tracks synthetic position/time sequences")
{
    cursor::PointerMotion m; // defaults: 3 px threshold, 1000 ms linger
    // First sample: no prior position, nothing has moved yet -> inactive.
    CHECK_FALSE(m.update(100.0f, 100.0f, 0));
    // A jump above threshold -> active.
    CHECK(m.update(140.0f, 100.0f, 16));
    // Holding still shortly after the move -> still active (within the linger).
    CHECK(m.update(140.0f, 100.0f, 300));
    // Holding still well past the linger window -> inactive (fades when still).
    CHECK_FALSE(m.update(140.0f, 100.0f, 2016));
    // Moving again re-activates.
    CHECK(m.update(200.0f, 160.0f, 2032));
    // A sub-threshold jitter does not keep it alive past the linger.
    CHECK_FALSE(m.update(201.0f, 160.0f, 2032 + 1500));
}

TEST_CASE("should_show_marker: the master-off switch hides regardless")
{
    cursor::CursorSettings s;
    s.enabled = false;
    CHECK_FALSE(cursor::should_show_marker(s, true,  false, true));
    CHECK_FALSE(cursor::should_show_marker(s, true,  true,  true));
    // Master-off hides outside gameplay too.
    CHECK_FALSE(cursor::should_show_marker(s, false, false, false));
}

TEST_CASE("should_show_marker: outside gameplay hides the marker with the settings window closed")
{
    // Char select / loading screens: Nexus reports !IsGameplay. The marker must
    // stay hidden whatever the matrix says — even the always-show cells — while
    // the settings window is closed (the open-window carve-out is tested below).
    cursor::CursorSettings s;
    s.enabled            = true;
    s.out_of_combat_mode = cursor::OutOfCombatMode::Always;
    s.in_combat_mode     = cursor::InCombatMode::Always;
    CHECK_FALSE(cursor::should_show_marker(s, /*in_gameplay=*/false, false, false, /*settings_open=*/false));
    CHECK_FALSE(cursor::should_show_marker(s, /*in_gameplay=*/false, false, true, /*settings_open=*/false));
    CHECK_FALSE(cursor::should_show_marker(s, /*in_gameplay=*/false, true,  false, /*settings_open=*/false));
    // Back in gameplay, the same settings show again.
    CHECK(cursor::should_show_marker(s, /*in_gameplay=*/true, false, false));
    CHECK(cursor::should_show_marker(s, /*in_gameplay=*/true, true,  false));
}

TEST_CASE("should_show_marker: outside gameplay shows only while the settings window is open")
{
    // The first-run settings window usually opens at char select: while it is
    // open the pointer marker shows there, even with both columns on Never, so
    // edits are visible at the pointer. Closing it hides the marker again.
    cursor::CursorSettings s;
    s.enabled            = true;
    s.out_of_combat_mode = cursor::OutOfCombatMode::Never;
    s.in_combat_mode     = cursor::InCombatMode::Never;
    CHECK(cursor::should_show_marker(s, /*in_gameplay=*/false, false, false, /*settings_open=*/true));
    CHECK_FALSE(cursor::should_show_marker(s, /*in_gameplay=*/false, false, false, /*settings_open=*/false));
    // Master-off still wins.
    s.enabled = false;
    CHECK_FALSE(cursor::should_show_marker(s, /*in_gameplay=*/false, false, false, /*settings_open=*/true));
    // In gameplay the matrix decides as before; an open window doesn't override it.
    s.enabled = true;
    CHECK_FALSE(cursor::should_show_marker(s, /*in_gameplay=*/true, false, false, /*settings_open=*/true));
}

TEST_CASE("should_show_marker: out-of-combat column Always/WhileMoving/Never")
{
    cursor::CursorSettings s;
    s.enabled            = true;
    const bool in_combat = false;

    s.out_of_combat_mode = cursor::OutOfCombatMode::Always;
    CHECK(cursor::should_show_marker(s, true, in_combat, false));
    CHECK(cursor::should_show_marker(s, true, in_combat, true));

    s.out_of_combat_mode = cursor::OutOfCombatMode::WhileMoving;
    CHECK_FALSE(cursor::should_show_marker(s, true, in_combat, false)); // still -> hidden
    CHECK(cursor::should_show_marker(s, true, in_combat, true));        // moving -> shown

    s.out_of_combat_mode = cursor::OutOfCombatMode::Never;
    CHECK_FALSE(cursor::should_show_marker(s, true, in_combat, false));
    CHECK_FALSE(cursor::should_show_marker(s, true, in_combat, true));
}

TEST_CASE("should_show_marker: in-combat column Always/Never, motion ignored")
{
    cursor::CursorSettings s;
    s.enabled            = true;
    const bool in_combat = true;

    s.in_combat_mode = cursor::InCombatMode::Always;
    CHECK(cursor::should_show_marker(s, true, in_combat, false));
    CHECK(cursor::should_show_marker(s, true, in_combat, true)); // pointer motion irrelevant

    s.in_combat_mode = cursor::InCombatMode::Never;
    CHECK_FALSE(cursor::should_show_marker(s, true, in_combat, false));
    CHECK_FALSE(cursor::should_show_marker(s, true, in_combat, true));
}

// --- schema v6 persistence + migration ---------------------------------------

TEST_CASE("every visibility slug round-trips through the enums")
{
    for (const cursor::OutOfCombatMode m : {cursor::OutOfCombatMode::Always,
                                            cursor::OutOfCombatMode::WhileMoving,
                                            cursor::OutOfCombatMode::Never})
    {
        const auto back = cursor::out_of_combat_from_slug(cursor::out_of_combat_to_slug(m));
        REQUIRE(back.has_value());
        CHECK(*back == m);
    }
    for (const cursor::InCombatMode m : {cursor::InCombatMode::Always,
                                         cursor::InCombatMode::Never})
    {
        const auto back = cursor::in_combat_from_slug(cursor::in_combat_to_slug(m));
        REQUIRE(back.has_value());
        CHECK(*back == m);
    }
    // "while_moving" is a valid OUT-of-combat value but NOT a valid in-combat one.
    CHECK(cursor::out_of_combat_from_slug("while_moving").has_value());
    CHECK_FALSE(cursor::in_combat_from_slug("while_moving").has_value());
}

TEST_CASE("a v5 file (no visibility modes) migrates forward to v6 with the defaults")
{
    TempStorePath tmp;
    {
        // Exactly what a v5 build wrote: schema 5, every field up to the QA icon, but
        // no visibility modes.
        std::ofstream out(tmp.path);
        out << R"({"schema_version":5,"enabled":true,"draw_above_windows":true,)"
               R"("preset":"pulse_ring","colour":"#ff2d9b","size_px":96,)"
               R"("opacity_pct":90,"outline":true,"outline_colour":"#141418",)"
               R"("fill":false,"fill_opacity_pct":35,"fill_colour":"#f2f2f6",)"
               R"("fill_size_pct":70,"freeze_after_drag":false,)"
               R"("show_quick_access_icon":true})";
    }
    cursor::CursorStore store(tmp.path);
    CHECK(store.settings().out_of_combat_mode == cursor::OutOfCombatMode::Always); // absent -> default
    CHECK(store.settings().in_combat_mode == cursor::InCombatMode::Always);        // absent -> default

    // Re-stamped at v6 with the modes present; a change survives a reload.
    cursor::CursorSettings next = store.settings();
    next.out_of_combat_mode = cursor::OutOfCombatMode::WhileMoving;
    next.in_combat_mode     = cursor::InCombatMode::Never;
    CHECK(store.set(next));
    json on_disk = json::parse(read_disk(tmp.path));
    CHECK(on_disk["schema_version"] == 7);
    CHECK(on_disk["out_of_combat_mode"] == "while_moving");
    CHECK(on_disk["in_combat_mode"] == "never");

    cursor::CursorStore reloaded(tmp.path);
    CHECK(reloaded.settings().out_of_combat_mode == cursor::OutOfCombatMode::WhileMoving);
    CHECK(reloaded.settings().in_combat_mode == cursor::InCombatMode::Never);
}

TEST_CASE("an invalid in-combat 'while_moving' slug coerces to the default")
{
    TempStorePath tmp;
    {
        // while_moving is valid out-of-combat but must not be a valid in-combat value.
        std::ofstream out(tmp.path);
        out << R"({"schema_version":6,"in_combat_mode":"while_moving",)"
               R"("out_of_combat_mode":"never"})";
    }
    cursor::CursorStore store(tmp.path);
    CHECK(store.settings().in_combat_mode == cursor::InCombatMode::Always);        // coerced to default
    CHECK(store.settings().out_of_combat_mode == cursor::OutOfCombatMode::Never);  // valid, honoured
}

TEST_CASE("an unknown visibility slug degrades to its default")
{
    TempStorePath tmp;
    {
        std::ofstream out(tmp.path);
        out << R"({"schema_version":6,"out_of_combat_mode":"sometimes",)"
               R"("in_combat_mode":"maybe"})";
    }
    cursor::CursorStore store(tmp.path);
    CHECK(store.settings().out_of_combat_mode == cursor::OutOfCombatMode::Always);
    CHECK(store.settings().in_combat_mode == cursor::InCombatMode::Always);
}

// --- schema v7 — the first-run `welcomed` flag -------------------------------
//
// The one-time first-run auto-open. A brand-new install has NO file, so
// load() returns defaults() with welcomed == false -> should_first_run_open is true
// -> the DLL glue opens the designed window once and persists welcomed = true. An
// EXISTING install (any on-disk file predating this key) migrates forward as
// already-welcomed, so upgrading users get no surprise auto-open. The
// window-open + immediate persist is the Windows/in-game surface (AddonLoad glue).

TEST_CASE("a fresh default record is not yet welcomed; should_first_run_open is true")
{
    const cursor::CursorSettings s = cursor::CursorSettings::defaults();
    CHECK_FALSE(s.welcomed);                    // brand-new install: not welcomed
    CHECK(cursor::should_first_run_open(s));    // ...so the first-run open fires
}

TEST_CASE("should_first_run_open is false once welcomed")
{
    cursor::CursorSettings s = cursor::CursorSettings::defaults();
    s.welcomed = true;
    CHECK_FALSE(cursor::should_first_run_open(s)); // never re-opens on later launches
}

TEST_CASE("a missing file (fresh install) loads not-welcomed so the first-run open fires")
{
    TempStorePath tmp;
    REQUIRE_FALSE(fs::exists(tmp.path));
    cursor::CursorStore store(tmp.path);
    CHECK_FALSE(store.settings().welcomed);          // only a brand-new install auto-opens
    CHECK(cursor::should_first_run_open(store.settings()));
}

TEST_CASE("an existing v6 file (no welcomed key) migrates forward as ALREADY welcomed")
{
    TempStorePath tmp;
    {
        // Exactly what a v6 build wrote: schema 6, the visibility modes, but no welcomed.
        std::ofstream out(tmp.path);
        out << R"({"schema_version":6,"enabled":true,"draw_above_windows":true,)"
               R"("preset":"pulse_ring","colour":"#ff2d9b","size_px":96,)"
               R"("opacity_pct":90,"outline":true,"outline_colour":"#141418",)"
               R"("fill":false,"fill_opacity_pct":35,"fill_colour":"#f2f2f6",)"
               R"("fill_size_pct":70,"freeze_after_drag":false,)"
               R"("show_quick_access_icon":true,"out_of_combat_mode":"always",)"
               R"("in_combat_mode":"always"})";
    }
    cursor::CursorStore store(tmp.path);
    CHECK(store.settings().welcomed);                          // upgrade: no surprise auto-open
    CHECK_FALSE(cursor::should_first_run_open(store.settings()));

    // Re-stamped at v7 with the welcomed key present on the next write-through.
    cursor::CursorSettings next = store.settings();
    next.enabled = false;
    CHECK(store.set(next));
    json on_disk = json::parse(read_disk(tmp.path));
    CHECK(on_disk["schema_version"] == 7);
    CHECK(on_disk["welcomed"] == true);
}

TEST_CASE("the welcomed flag round-trips through disk at v7")
{
    TempStorePath tmp;
    {
        cursor::CursorStore store(tmp.path);
        cursor::CursorSettings next = store.settings();
        next.welcomed = true;               // the first-run open just fired + persisted it
        CHECK(store.set(next));
        json on_disk = json::parse(read_disk(tmp.path));
        CHECK(on_disk["welcomed"] == true);
    }
    cursor::CursorStore reloaded(tmp.path);
    CHECK(reloaded.settings().welcomed);     // survives a restart -> never re-opens
    CHECK_FALSE(cursor::should_first_run_open(reloaded.settings()));
}

// The Reset-to-defaults edge: Reset restores visual defaults but must NOT
// clear welcomed, or the auto-open re-triggers next launch. The preserve-welcomed
// rule lives inline in entry.cpp's Reset button glue (capture w, reset, restore w);
// this asserts the pure logic pattern that glue implements.
TEST_CASE("reset-to-defaults preserves welcomed, resets visuals")
{
    cursor::CursorSettings s = cursor::CursorSettings::defaults();
    s.welcomed = true;                    // already welcomed
    s.preset   = cursor::Preset::SoftHalo; // ...with customised visuals
    s.size_px  = 100;

    // The Reset glue: capture welcomed, reset to visual defaults, restore welcomed.
    const bool w = s.welcomed;
    s = cursor::CursorSettings::defaults();
    s.welcomed = w;

    CHECK(s.preset == cursor::Preset::PulseRing); // visuals reset
    CHECK(s.size_px == 96);                       // visuals reset
    CHECK(s.welcomed);                            // welcomed preserved -> no re-open
    CHECK_FALSE(cursor::should_first_run_open(s));
}

// The gold L-shaped corner ornaments are removed from the shared theme
// everywhere. DrawThemedFrame's
// drawing is ImGui-only (outside this harness), so this guards the data that
// drove it: the palette no longer carries a `corner` ornament entry. Detection
// idiom, evaluated at compile time and checked at run time so the suite goes
// red (not a build break) while the member exists.
namespace detail {
template <class T, class = void>
struct has_corner_ornament : std::false_type {};
template <class T>
struct has_corner_ornament<T, std::void_t<decltype(std::declval<T&>().corner)>>
    : std::true_type {};
} // namespace detail

TEST_CASE("theme: no corner ornament in the shared palette")
{
    CHECK_FALSE(detail::has_corner_ornament<shared::theme::Palette>::value);
}
