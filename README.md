# Cursor Finder

A customizable highlight around the mouse pointer so you never lose it in busy
fights. An addon for **Guild Wars 2**, built on the
[Nexus](https://raidcore.gg/gw2/nexus) addon host.

- **Five highlight presets:** Ring, Reticle, Cross, Dash, and Halo. Each one sits
  exactly on the click point.
- **Make it yours.** You can change the colour (swatches or a custom picker), size,
  opacity, outline, and an optional filled centre. A live preview shows your
  choices against several backdrops, and one click resets everything to
  defaults.
- **Show it when you need it.** Out of combat, the highlight can show always, only
  while you're moving, or never. In combat, it can show always or never.
- **Lock marker while dragging (optional).** While you hold a mouse button to
  drag the camera in the world, the pointer stays put instead of drifting. It
  never blocks dragging addon or game windows.
- **One settings window.** Open it from the QuickAccess icon or from a keybind
  (unbound by default). You can hide the QuickAccess icon if you don't want it.
  On a fresh install, the settings window opens once.

It only draws on screen. It doesn't automate anything or use the network, and
it reads only the game's official MumbleLink data (to know when you're in
combat), never game memory.

## Install

You need Guild Wars 2 (64-bit) with [Nexus](https://raidcore.gg/gw2/nexus)
installed.

1. Open the latest green run of the **build** workflow under this repository's
   **Actions** tab.
2. Download the `cursor-dll` artifact and unzip it.
3. Put `cursor.dll` in your Guild Wars 2 `addons/` folder.
4. Start the game and load **Cursor Finder** from Nexus's addon list.

Developed and tested in-game under CrossOver on macOS (Apple Silicon). Built as
a standard x64 Windows DLL.

## Your data

Settings are stored locally in `addons/cursor/cursor.json`. Nothing leaves your
computer.

## Building

Clone with submodules:

```bash
git clone --recurse-submodules https://github.com/FuchsiaLlamaMama/gw2-nexus-plugin-cursor
```

**Windows (MSVC, x64).** Builds the DLL and runs the unit tests:

```
cmake -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

**macOS / Linux.** The same CMake commands (without `-A x64`) build and test the
pure-logic library; the DLL itself is Windows-only. Without CMake,
`tests/run_offgame.sh` compiles and runs the tests with `xcrun clang++`.

**CI.** Every push and pull request builds on `windows-latest`, runs the tests,
and uploads `cursor.dll` as an artifact.

## Dependencies

- **[Nexus-API](https://github.com/RaidcoreGG/Nexus-API)** (submodule, MIT): the
  addon API definitions.
- **[Dear ImGui](https://github.com/RaidcoreGG/imgui)** (submodule, MIT):
  RaidcoreGG's v1.80 build, pinned to the version the Nexus host uses, because a
  mismatch crashes.
- **[gw2-nexus-plugin-shared](https://github.com/FuchsiaLlamaMama/gw2-nexus-plugin-shared)**
  (submodule, MIT): shared theme and persistence helpers.
- **[nlohmann/json](https://github.com/nlohmann/json)** 3.11.3 and
  **[doctest](https://github.com/doctest/doctest)** 2.4.11 (both MIT), vendored
  as single headers.

The Nexus host loader itself is proprietary and is **not** a build dependency;
the addon only targets its MIT-licensed public API.

## License

[MIT](LICENSE).

## Notice

This addon is unofficial and fan-made. It is **not affiliated with or endorsed
by ArenaNet or RaidcoreGG**. Guild Wars 2, its user-interface art, and its logos
are trademarks and the property of ArenaNet, LLC. This addon does not bundle or
redistribute any Guild Wars 2 art.
