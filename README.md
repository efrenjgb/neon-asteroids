# Neon Asteroids

A neon-vector take on Asteroids, written in C++20 on [raylib](https://www.raylib.com/).
Newtonian flight, procedurally-synthesised audio (no asset files), escalating
waves, hostile saucers, powerups, hyperspace, and 1–2 player local co-op.

![built with raylib](https://img.shields.io/badge/built%20with-raylib-white)

## Download

Grab a build from the [Releases](../../releases) page:

- **macOS** (`neon-asteroids-macos.zip`) — Apple Silicon.
  First launch is blocked by Gatekeeper because the binary is unsigned:
  right-click `neon_asteroids` → **Open**, or run
  `xattr -dr com.apple.quarantine neon_asteroids`.
- **Windows** (`neon-asteroids-windows.zip`) — 64-bit.
  SmartScreen may warn on first run: **More info → Run anyway**.
- **Linux** (`neon-asteroids-linux.zip`) — x86-64. Mark it executable
  (`chmod +x neon_asteroids`) and run it. Needs an X11/OpenGL desktop and ALSA;
  if it complains about missing libraries, install your distro's equivalents of
  `libasound2`, `libx11`, `libgl1`.

Keep the `shaders/` folder next to the executable.

## Controls

**Keyboard — Player 1:** `WASD` / arrows to turn and thrust · `Space` fire ·
`Left Shift` hyperspace
**Keyboard — Player 2** (two-player): arrow keys · `Right Shift` fire · `/` hyperspace
**Gamepad:** left stick steer + thrust · `A` fire · `X`/`B` hyperspace ·
`Start` restart

Other: `F11` / `Alt+Enter` / `Cmd+F` fullscreen · `R` restart · `Esc` quit.
In two-player, a menu step lets each player press a button to claim their controller.

## Gameplay

- **Vacuum physics** — no drag; slow down by turning and burning retrograde.
- **Waves escalate** in count, speed, and composition; there is no plateau.
- **Saucers** arrive from wave 3 — large ones spray, small ones aim.
- **Powerups** drop from downed saucers: extra life, fan (spread) fire, or a
  one-hit shield.
- **Hyperspace** opens a portal ahead along your heading and drops you
  elsewhere, briefly invulnerable.
- **Co-op:** a player who is out of lives rejoins on the next wave if their
  teammate clears it.

## Build from source

Requires CMake ≥ 3.24 and a C++20 compiler. raylib is fetched and built
automatically — no system install needed.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/neon_asteroids          # Windows: build\Release\neon_asteroids.exe
```

Shaders load from a `shaders/` folder next to the executable, falling back to
the source tree for dev builds, so running straight from `build/` just works.

## Controllers

Xbox and 8BitDo pads are supported. If a controller is detected but does
nothing, it likely needs a mapping entry — build the `gamepad_probe` target
(`cmake --build build --target gamepad_probe`) to dump its GUID and raw input,
then add an SDL_GameControllerDB line to `src/gamepad_mappings.hpp`. Note that
gamepad rumble and player-indicator LEDs are not available through the
underlying GLFW backend.
