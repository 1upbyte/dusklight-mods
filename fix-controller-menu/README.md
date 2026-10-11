# Selected Controller Menu Input

This Dusklight mod lets the menu respond to the keyboard and the controller
assigned to player one (port 0). Other connected controllers cannot navigate,
confirm, cancel, or open the menu. Mouse and touch input keep their normal UI
behavior. The selected controller can still be changed in Controller Settings;
the filter follows that change immediately.

## Build

Requires CMake 3.25 or newer, a C++20 compiler, and the Dusklight Mod SDK.

```sh
cmake -S . -B build -DDUSKLIGHT_DIR=/path/to/dusklight
cmake --build build --target fix_controller_menu_package
```

Without `DUSKLIGHT_DIR`, CMake fetches Dusklight v2.0.1. On macOS, the SDK
also needs a Dusklight game binary or link stub. Supply its path with
`-DDUSK_GAME_EXE=/path/to/dusklight` if the stub cannot be downloaded.

Install `build/mods/fix_controller_menu.dusk` in Dusklight's user mods
directory. On macOS this is
`~/Library/Application Support/TwilitRealm/Dusklight/mods`.
