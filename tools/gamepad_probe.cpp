// Diagnostic: compares raylib's mapped gamepad view against GLFW's raw
// joystick view for the same device.
//
// raylib reports availability via glfwJoystickPresent but reads input via
// glfwGetGamepadState, which needs an SDL_GameControllerDB mapping entry. That
// creates two very different failure modes which look identical from raylib:
//
//   raw axes move, isGamepad false  -> missing mapping, fixable via
//                                      SetGamepadMappings() with the GUID
//   raw axes also dead              -> the OS is not delivering HID data at
//                                      all; a mapping would change nothing
//
// The GUID printed here is what an SDL_GameControllerDB entry is keyed on.

#include "../src/gamepad_mappings.hpp"

#include <raylib.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>

int main()
{
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(860, 460, "Gamepad Probe");

    const int added = SetGamepadMappings(kExtraGamepadMappings);
    std::printf("SetGamepadMappings -> %d\n", added);
    std::fflush(stdout);

    SetTargetFPS(60);

    int frame = 0;

    while (!WindowShouldClose())
    {
        frame++;
        const bool logNow = (frame % 30 == 0);

        BeginDrawing();
        ClearBackground(BLACK);

        int y = 16;
        DrawText("GAMEPAD PROBE - move BOTH sticks fully, squeeze triggers, press buttons",
                 16, y, 15, YELLOW);
        y += 30;

        // Enumerate every slot first: this is what shows whether two identical
        // controllers are seen as distinct joysticks (slots 0 and 1) or are
        // colliding on one slot — the difference between working P1/P2 and both
        // driving the same ship.
        {
            int sy = y;
            DrawText("--- all joystick slots ---", 16, sy, 15, YELLOW);
            sy += 20;
            for (int s = 0; s < 4; s++)
            {
                char slotLine[256];
                if (glfwJoystickPresent(s))
                {
                    const char* nm = glfwGetJoystickName(s);
                    std::snprintf(slotLine, sizeof(slotLine), "slot %d: PRESENT  %s  mapped=%s",
                                  s, nm ? nm : "(null)",
                                  glfwJoystickIsGamepad(s) ? "YES" : "NO");
                    DrawText(slotLine, 16, sy, 15, GREEN);
                    if (logNow) std::printf("slot %d present name=\"%s\" mapped=%d\n",
                                            s, nm ? nm : "?", glfwJoystickIsGamepad(s));
                }
                else
                {
                    std::snprintf(slotLine, sizeof(slotLine), "slot %d: empty", s);
                    DrawText(slotLine, 16, sy, 15, DARKGRAY);
                }
                sy += 18;
            }
            y = sy + 12;
        }

        const int jid = GLFW_JOYSTICK_1;

        if (glfwJoystickPresent(jid) == GLFW_FALSE)
        {
            DrawText("GLFW: no joystick on slot 1", 16, y, 18, RED);
            if (logNow) { std::printf("glfw: no joystick\n"); std::fflush(stdout); }
            EndDrawing();
            continue;
        }

        const char* jname   = glfwGetJoystickName(jid);
        const char* guid    = glfwGetJoystickGUID(jid);
        const bool  isPad   = glfwJoystickIsGamepad(jid) == GLFW_TRUE;
        const char* padName = isPad ? glfwGetGamepadName(jid) : "(not a mapped gamepad)";

        char line[320];
        std::snprintf(line, sizeof(line), "name  : %s", jname ? jname : "(null)");
        DrawText(line, 16, y, 17, RAYWHITE); y += 24;

        std::snprintf(line, sizeof(line), "GUID  : %s", guid ? guid : "(null)");
        DrawText(line, 16, y, 17, ORANGE); y += 24;

        std::snprintf(line, sizeof(line), "mapped: %s   -> %s",
                      isPad ? "YES" : "NO", padName ? padName : "(null)");
        DrawText(line, 16, y, 17, isPad ? GREEN : RED); y += 30;

        // --- RAW joystick data: works with or without a mapping ---
        int          rawAxisCount = 0;
        const float* rawAxes      = glfwGetJoystickAxes(jid, &rawAxisCount);

        char axisLine[320] = "RAW axes  : ";
        for (int a = 0; a < rawAxisCount && a < 8; a++)
        {
            char part[32];
            std::snprintf(part, sizeof(part), "%+.2f ", rawAxes[a]);
            std::strcat(axisLine, part);
        }
        if (rawAxisCount == 0) std::strcat(axisLine, "(none reported)");
        DrawText(axisLine, 16, y, 17, SKYBLUE); y += 24;

        int                  rawBtnCount = 0;
        const unsigned char* rawBtns     = glfwGetJoystickButtons(jid, &rawBtnCount);

        char btnLine[320] = "RAW buttons held: ";
        bool anyRaw = false;
        for (int b = 0; b < rawBtnCount; b++)
        {
            if (rawBtns[b] == GLFW_PRESS)
            {
                char part[16];
                std::snprintf(part, sizeof(part), "%d ", b);
                std::strcat(btnLine, part);
                anyRaw = true;
            }
        }
        if (!anyRaw) std::strcat(btnLine, "(none)");
        DrawText(btnLine, 16, y, 17, anyRaw ? GREEN : GRAY); y += 30;

        // --- what raylib exposes, for comparison ---
        char rlLine[320] = "raylib axes: ";
        for (int a = 0; a < 6; a++)
        {
            char part[32];
            std::snprintf(part, sizeof(part), "%+.2f ", GetGamepadAxisMovement(0, a));
            std::strcat(rlLine, part);
        }
        DrawText(rlLine, 16, y, 17, VIOLET); y += 24;

        // Which raylib SEMANTIC button is held — this is what the game reads,
        // so it names exactly what a physical button currently does.
        static const char* kBtnNames[] = {
            "-", "L_UP", "L_RIGHT", "L_DOWN", "L_LEFT",
            "R_UP(Y/X-top)", "R_RIGHT(B)", "R_DOWN(A)", "R_LEFT(X/Y-left)",
            "L_BUMPER", "L_TRIG", "R_BUMPER", "R_TRIG",
            "SELECT", "GUIDE", "START", "L_STICK", "R_STICK",
        };
        char semLine[320] = "raylib button: ";
        bool anySem = false;
        for (int b = 1; b <= 17; b++)
        {
            if (IsGamepadButtonDown(0, b))
            {
                std::strcat(semLine, kBtnNames[b]);
                std::strcat(semLine, " ");
                anySem = true;
            }
        }
        if (!anySem) std::strcat(semLine, "(none)");
        DrawText(semLine, 16, y, 17, anySem ? LIME : GRAY); y += 30;

        std::snprintf(line, sizeof(line), "raw axis count=%d  raw button count=%d",
                      rawAxisCount, rawBtnCount);
        DrawText(line, 16, y, 15, GRAY);

        if (logNow)
        {
            std::printf("name=\"%s\" guid=%s mapped=%s rawAxes=%d rawBtns=%d | ",
                        jname ? jname : "?", guid ? guid : "?",
                        isPad ? "YES" : "NO", rawAxisCount, rawBtnCount);
            for (int a = 0; a < rawAxisCount && a < 8; a++)
                std::printf("%+.2f ", rawAxes[a]);
            std::printf("| raw:%s | sem:%s\n",
                        anyRaw ? btnLine + 18 : "none",
                        anySem ? semLine + 15 : "none");
            std::fflush(stdout);
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
