# Mute Music Button

Press **F7** to mute music, then press it again to restore the previous music
level. To change the binding, open **Controller Settings**, select the desired
port and device, then open **Custom Action Bindings** and choose **Mute Music**.
Bind a keyboard key or a gamepad button using Dusklight's normal binding
control. **Esc** clears a binding. Keyboard and gamepad bindings are stored
separately in Dusklight's `config.json`.

Sound effects and other volume settings are untouched. The mute is temporary
and is cleared when the mod is disabled.

## Build

Requires CMake 3.25 or newer, a C++20 compiler, and the Dusklight Mod SDK.

```sh
cmake -S . -B build -DDUSKLIGHT_DIR=/path/to/dusklight
cmake --build build --target mute_music_button_package
```

Without `DUSKLIGHT_DIR`, CMake fetches Dusklight `v2.0.1`. On macOS, the SDK
also needs a Dusklight game binary or link stub. Supply its path with
`-DDUSK_GAME_EXE=/path/to/dusklight` if the stub cannot be downloaded.

Copy the resulting `.dusk` file from `build/mods/` to Dusklight's user mods
directory. On macOS this is
`~/Library/Application Support/TwilitRealm/Dusklight/mods`.
