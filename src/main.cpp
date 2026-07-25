#include "entities.hpp"
#include "game.hpp"
#include "gamepad_mappings.hpp"
#include "postfx.hpp"

#include <raylib.h>

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

    while (!WindowShouldClose())
    {
        const float dt = GetFrameTime();

        if (IsKeyPressed(KEY_ESCAPE)) break;

        // macOS frequently binds F11 to Show Desktop, so it may never reach
        // the app. Alt+Enter and Cmd+F are offered as alternatives.
        const bool altEnter = (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT))
                           && IsKeyPressed(KEY_ENTER);
        const bool cmdF = (IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER))
                       && IsKeyPressed(KEY_F);

        if (IsKeyPressed(KEY_F11) || altEnter || cmdF) ToggleFullscreenBorderless();

        game.Update(dt);

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
