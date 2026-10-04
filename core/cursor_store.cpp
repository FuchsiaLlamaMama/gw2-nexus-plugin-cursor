#include "core/cursor_store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "persistence/atomic_file.h"

namespace cursor {

using nlohmann::json;

namespace {

// --- colour <-> "#rrggbb" -----------------------------------------------------

std::string to_hex(const Rgb& c)
{
    static const char* d = "0123456789abcdef";
    std::string s = "#______";
    const std::uint8_t ch[3] = {c.r, c.g, c.b};
    for (int i = 0; i < 3; ++i)
    {
        s[1 + i * 2] = d[(ch[i] >> 4) & 0xF];
        s[2 + i * 2] = d[ch[i] & 0xF];
    }
    return s;
}

// Parse "#rrggbb" (case-insensitive). std::nullopt on any malformation so the
// caller keeps the field's default rather than throwing (corrupt-file contract).
std::optional<Rgb> from_hex(std::string_view s)
{
    if (s.size() != 7 || s[0] != '#') { return std::nullopt; }
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') { return c - '0'; }
        if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
        if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i)
    {
        v[i] = nib(s[1 + i]);
        if (v[i] < 0) { return std::nullopt; }
    }
    return Rgb{static_cast<std::uint8_t>(v[0] * 16 + v[1]),
               static_cast<std::uint8_t>(v[2] * 16 + v[3]),
               static_cast<std::uint8_t>(v[4] * 16 + v[5])};
}

// Read a string field guarded by type; std::nullopt if absent or wrong type.
std::optional<std::string> read_str(const json& doc, const char* key)
{
    if (auto it = doc.find(key); it != doc.end() && it->is_string())
    {
        return it->get<std::string>();
    }
    return std::nullopt;
}

// Read an integer field guarded by type, then clamp to [lo, hi]; the fallback
// (an absent/malformed value) is returned unclamped by the caller's default.
std::optional<int> read_clamped_int(const json& doc, const char* key, int lo, int hi)
{
    if (auto it = doc.find(key);
        it != doc.end() && it->is_number_integer())
    {
        return clamp_int(it->get<int>(), lo, hi);
    }
    return std::nullopt;
}

// Read a bool field guarded by type; std::nullopt if absent or wrong type.
std::optional<bool> read_bool(const json& doc, const char* key)
{
    if (auto it = doc.find(key); it != doc.end() && it->is_boolean())
    {
        return it->get<bool>();
    }
    return std::nullopt;
}

// Forward-migrate a parsed settings document to the current schema in memory.
//
// v1 is the first versioned shape, so "migration" today is defensive rather than
// transformative: read the known fields where present, fall back to the factory
// default for any that are absent, and tolerate unknown/extra fields (a file
// written by a LATER version must still LOAD here without throwing). A missing
// `schema_version` (a hypothetical pre-versioned file) is treated the same way —
// the record is re-stamped at the current version on the next write-through.
//
// Forward-compat is READ-side only, not round-trip: serialize() (below) writes
// exactly the keys this version knows, so any unknown/newer-schema field is
// DROPPED on the next write-through and the version re-stamped down. That is
// acceptable under forward-only auto-update (a newer build never has to preserve
// an even-newer file); if a downgrade-preserving guarantee is ever needed, carry
// an unknown-key bag through serialize().
CursorSettings read_settings(const json& doc)
{
    CursorSettings s = CursorSettings::defaults();
    if (!doc.is_object()) { return s; }

    // value() returns the default when the key is absent OR the wrong type would
    // throw — guard type explicitly so a malformed field degrades to the default
    // rather than throwing (consistent with the corrupt-file recovery contract).
    if (auto it = doc.find("enabled"); it != doc.end() && it->is_boolean())
    {
        s.enabled = it->get<bool>();
    }
    if (auto b = read_bool(doc, "draw_above_windows")) { s.draw_above_windows = *b; }

    // v2 appearance. Absent in a v1 file -> each keeps its default:
    // that IS the v1 -> v2 forward migration. A malformed value (bad
    // colour hex, out-of-range size, unknown preset slug) degrades to the default
    // rather than throwing, consistent with the corrupt-file recovery contract.
    if (auto slug = read_str(doc, "preset"))
    {
        if (auto p = preset_from_slug(*slug)) { s.preset = *p; }
    }
    if (auto hex = read_str(doc, "colour"))
    {
        if (auto c = from_hex(*hex)) { s.colour = *c; }
    }
    if (auto n = read_clamped_int(doc, "size_px", kSizeMin, kSizeMax)) { s.size_px = *n; }
    if (auto n = read_clamped_int(doc, "opacity_pct", kOpacityMin, kOpacityMax))
    {
        s.opacity_pct = *n;
    }
    if (auto b = read_bool(doc, "outline")) { s.outline = *b; }
    if (auto hex = read_str(doc, "outline_colour"))
    {
        if (auto c = from_hex(*hex)) { s.outline_colour = *c; }
    }
    if (auto b = read_bool(doc, "fill")) { s.fill = *b; }
    if (auto n = read_clamped_int(doc, "fill_opacity_pct", kFillOpacityMin, kFillOpacityMax))
    {
        s.fill_opacity_pct = *n;
    }
    if (auto hex = read_str(doc, "fill_colour"))
    {
        if (auto c = from_hex(*hex)) { s.fill_colour = *c; }
    }
    if (auto n = read_clamped_int(doc, "fill_size_pct", kFillSizeMin, kFillSizeMax))
    {
        s.fill_size_pct = *n;
    }
    // v4 behaviour. Absent in a v1-v3 file -> keeps its default (false):
    // that IS the v3 -> v4 forward migration.
    if (auto b = read_bool(doc, "freeze_after_drag")) { s.freeze_after_drag = *b; }
    // v5 quick-access. Absent in a v1-v4 file -> keeps its default (true):
    // that IS the v4 -> v5 forward migration.
    if (auto b = read_bool(doc, "show_quick_access_icon")) { s.show_quick_access_icon = *b; }
    // v6 visibility matrix. Absent in a v1-v5 file -> each keeps its
    // default (Always/Always): that IS the v5 -> v6 forward migration (the marker
    // stays always-shown). An unknown slug degrades to the default; an in-combat
    // "while_moving" is invalid so from_slug returns nullopt and the field
    // keeps its default — the defensive corrupt-file contract.
    if (auto slug = read_str(doc, "out_of_combat_mode"))
    {
        if (auto m = out_of_combat_from_slug(*slug)) { s.out_of_combat_mode = *m; }
    }
    if (auto slug = read_str(doc, "in_combat_mode"))
    {
        if (auto m = in_combat_from_slug(*slug)) { s.in_combat_mode = *m; }
    }
    // v7 first-run flag. THE MIGRATION CRUX: any on-disk file that
    // predates this key belongs to an EXISTING install, which is already-welcomed
    // -> welcomed = true, so upgrading users get no surprise auto-open. (A brand-new
    // install has NO file at all: load() never reaches here and returns defaults()
    // with welcomed = false, the only state that triggers the first-run open.) An
    // explicit persisted value is honoured so welcomed round-trips at v7.
    s.welcomed = true; // migration default for a v<7 / absent-key file
    if (auto b = read_bool(doc, "welcomed")) { s.welcomed = *b; }
    return s;
}

} // namespace

CursorStore::CursorStore(std::filesystem::path path)
    : path_(std::move(path)), settings_(CursorSettings::defaults())
{
    load();
}

void CursorStore::load()
{
    settings_ = CursorSettings::defaults();

    const auto raw = shared::persistence::read_file(path_);
    if (!raw) { return; } // missing file -> defaults

    json doc = json::parse(*raw, /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object())
    {
        // Corrupt / unparseable -> recover to defaults without throwing.
        return;
    }

    // Forward-compatible read + migration: known fields are read, absent
    // fields keep their defaults, unknown fields are ignored, and the file is
    // re-stamped at the current schema on the next mutation.
    settings_ = read_settings(doc);
}

std::string CursorStore::serialize() const
{
    json doc;
    doc["schema_version"]     = CursorSettings::kSchemaVersion;
    doc["enabled"]            = settings_.enabled;
    doc["draw_above_windows"] = settings_.draw_above_windows;
    // v2 appearance.
    doc["preset"]             = preset_to_slug(settings_.preset);
    doc["colour"]             = to_hex(settings_.colour);
    doc["size_px"]            = settings_.size_px;
    doc["opacity_pct"]        = settings_.opacity_pct;
    doc["outline"]            = settings_.outline;
    doc["outline_colour"]     = to_hex(settings_.outline_colour);
    doc["fill"]               = settings_.fill;
    doc["fill_opacity_pct"]   = settings_.fill_opacity_pct;
    doc["fill_colour"]        = to_hex(settings_.fill_colour);
    doc["fill_size_pct"]      = settings_.fill_size_pct;
    // v4 behaviour.
    doc["freeze_after_drag"]  = settings_.freeze_after_drag;
    // v5 quick-access.
    doc["show_quick_access_icon"] = settings_.show_quick_access_icon;
    // v6 visibility matrix.
    doc["out_of_combat_mode"] = out_of_combat_to_slug(settings_.out_of_combat_mode);
    doc["in_combat_mode"]     = in_combat_to_slug(settings_.in_combat_mode);
    // v7 first-run flag.
    doc["welcomed"]           = settings_.welcomed;
    return doc.dump(2);
}

bool CursorStore::persist() const
{
    return shared::persistence::atomic_write(path_, serialize());
}

bool CursorStore::flush() const
{
    return persist();
}

bool CursorStore::set_enabled(bool enabled)
{
    if (settings_.enabled == enabled) { return false; }
    settings_.enabled = enabled;
    persist(); // write-through: on disk before we return
    return true;
}

bool CursorStore::set_draw_above_windows(bool above)
{
    if (settings_.draw_above_windows == above) { return false; }
    settings_.draw_above_windows = above;
    persist(); // write-through
    return true;
}

bool CursorStore::set(const CursorSettings& next)
{
    if (settings_ == next) { return false; }
    settings_ = next;
    persist(); // write-through
    return true;
}

} // namespace cursor
