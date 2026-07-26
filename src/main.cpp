#include "entities.hpp"
#include "game.hpp"
#include "gamepad_mappings.hpp"
#include "postfx.hpp"

#include <raylib.h>

#include <algorithm>

namespace
{
// Borderless rather than true fullscreen, deliberately.
//
// raylib's ToggleFullscreen "resizes monitor to match window resolution" — it
// forces a video mode change. On this display that produced screen=1512x855
// against render=1920x1200: a non-uniform scale whose two axes disagree on
// aspect ratio, which no amount of letterboxing can correct.
//
// ToggleBorderlessWindowed "resizes window to match monitor resolution"
// instead. No mode change, so the Retina backing scale stays uniform and the
// reported dimensions stay consistent, exactly as in windowed mode.
void ToggleFullscreenBorderless()
{
    ToggleBorderlessWindowed();

    TraceLog(LOG_INFO, "DISPLAY: borderless=%d screen=%dx%d render=%dx%d",
             IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) ? 1 : 0,
             GetScreenWidth(), GetScreenHeight(),
             GetRenderWidth(), GetRenderHeight());
}
}  // namespace

int main()
{
    // Resizable is safe because Present letterboxes the fixed-size scene into
    // whatever the window becomes.
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(kScreenW, kScreenH, "Neon Asteroids");

    // Must follow InitWindow: this forwards to GLFW, which needs to be up.
    SetGamepadMappings(kExtraGamepadMappings);

    InitAudioDevice();
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);   // Esc is handled below, not a hard quit

    PostFX fx;
    fx.Load(kScreenW, kScreenH);

    Game game;
    game.Init();

    // The simulation runs on a fixed 60 Hz step, decoupled from render rate.
    // This gives the sim a stable cadence — the foundation the networked host
    // will tick against — and makes physics independent of framerate. Rendering
    // still happens once per real frame. At the usual vsync-locked 60 fps this
    // is exactly one tick per frame; the accumulator only matters when a frame
    // runs long or short.
    constexpr float kFixedDt   = 1.0f / 60.0f;
    constexpr float kMaxCatchUp = 0.25f;   // cap to avoid a death spiral after a hitch
    float accumulator = 0.0f;

    while (!WindowShouldClose())
    {
        const float frameDt = GetFrameTime();

        if (IsKeyPressed(KEY_ESCAPE)) break;

        // macOS frequently binds F11 to Show Desktop, so it may never reach
        // the app. Alt+Enter and Cmd+F are offered as alternatives.
        const bool altEnter = (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT))
                           && IsKeyPressed(KEY_ENTER);
        const bool cmdF = (IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER))
                       && IsKeyPressed(KEY_F);

        if (IsKeyPressed(KEY_F11) || altEnter || cmdF) ToggleFullscreenBorderless();

        if (game.IsPlaying())
        {
            accumulator = std::min(accumulator + frameDt, kMaxCatchUp);

            // Sample this frame's intent once, then feed it to every tick. Edge
            // actions (hyperspace) are cleared after the first tick so a
            // multi-tick frame can't fire them twice.
            ShipControls controls[kMaxPlayers];
            for (int p = 0; p < kMaxPlayers; p++) controls[p] = game.SampleControls(p);

            while (accumulator >= kFixedDt)
            {
                game.Update(kFixedDt, controls);
                for (int p = 0; p < kMaxPlayers; p++) controls[p].hyperspace = false;
                accumulator -= kFixedDt;

                // A tick may end the run (all players dead); stop stepping the
                // moment we leave Playing so game-over input isn't run here.
                if (!game.IsPlaying()) { accumulator = 0.0f; break; }
            }
        }
        else
        {
            // Menus, the claim step and game-over are UI: run once per real
            // frame, reading their own input. No fixed stepping needed.
            game.Update(frameDt, nullptr);
            accumulator = 0.0f;
        }

        fx.BeginScene();
            game.Draw();
        fx.EndScene();

        BeginDrawing();
            ClearBackground(BLACK);
            fx.Present(game.ShakeOffset());
        EndDrawing();
    }

    game.Shutdown();
    fx.Unload();
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
