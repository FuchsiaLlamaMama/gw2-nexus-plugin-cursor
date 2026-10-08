// cursor-finder-core — the persisted Cursor Finder settings and its durability
// guarantee.
//
// CursorStore owns a JSON file path and is WRITE-THROUGH: every committed
// mutation (set_enabled / set_draw_above_windows) is flushed to disk immediately
// via an atomic write, so durability does NOT depend on Unload firing at
// game exit. It loads on construction, tolerating a missing file (defaults) and a
// corrupt file (recover to defaults without throwing). The JSON carries a
// top-level schema version so later versions add fields and migrate older
// files forward without loss.
//
// Pure C++17 (no Nexus/ImGui/Windows); builds and unit-tests on macOS/clang.
#pragma once

#include <filesystem>
#include <string>

#include "core/cursor_finder_settings.h"

namespace cursor_finder {

// Where the settings live, built from the Nexus addons root
// (Paths_GetAddonDirectory(nullptr)): <addons>/cursor-finder/cursor-finder.json.
// Installs from before the rename keep theirs at <addons>/cursor/cursor.json.
std::filesystem::path settings_file(const std::filesystem::path& addons_root);
std::filesystem::path legacy_settings_file(const std::filesystem::path& addons_root);

// One-time move of the legacy settings file, run before the store loads.
// Returns the path the store should use; never throws.
//   - the new file exists       -> the new path (a legacy file is left alone:
//                                  only an older build could have written it)
//   - no legacy file            -> the new path (defaults)
//   - only the legacy file      -> move it (or copy, then delete it), clear the
//                                  emptied legacy folder, return the new path
//   - the move/copy both failed -> the legacy path, so the real settings still
//                                  load and the next launch tries again
// A new file that cannot be stat'd counts as present (never overwritten); a
// legacy file that cannot be stat'd is left on disk and the new path is used.
std::filesystem::path resolve_settings_path(const std::filesystem::path& new_file,
                                            const std::filesystem::path& legacy_file);

class CursorStore {
public:
    // Load the settings from `path`. A missing file yields the factory defaults;
    // a corrupt/unparseable file also recovers to defaults without throwing. An
    // older/absent-version file is migrated forward (known fields preserved,
    // absent fields defaulted).
    explicit CursorStore(std::filesystem::path path);

    const CursorSettings& settings() const { return settings_; }
    // The schema version this build WRITES (the current on-write version), not the
    // version of whatever file was loaded — an older file is re-stamped to this on
    // the next write-through.
    int schema_version() const { return CursorSettings::kSchemaVersion; }
    const std::filesystem::path& path() const { return path_; }

    // Set the master on/off flag; write-through persists immediately. Returns
    // true if the value changed (a no-op change still persists nothing new).
    bool set_enabled(bool enabled);

    // Set the "show above Nexus windows" draw-order flag; write-through persists.
    // Returns true if the value changed.
    bool set_draw_above_windows(bool above);

    // Replace the whole record; write-through persists. Returns true if anything
    // changed. Used by the panel which edits a working copy.
    bool set(const CursorSettings& next);

    // Serialize the current state to the JSON string form written to disk.
    std::string serialize() const;

    // Best-effort re-write of the current state (belt-and-braces for the
    // Unload path). Durability does not depend on this — writes are already
    // write-through. Returns true on success.
    bool flush() const;

private:
    void load();
    bool persist() const; // atomic write-through; returns success

    std::filesystem::path path_;
    CursorSettings        settings_;
};

} // namespace cursor_finder
