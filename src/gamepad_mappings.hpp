#pragma once

// Extra SDL_GameControllerDB entries for controllers missing from the table
// GLFW bundles.
//
// GLFW decides a joystick is a "gamepad" by looking its GUID up in that table.
// raylib reports availability from glfwJoystickPresent but reads input from
// glfwGetGamepadState — and ignores that call's failure — so an unlisted
// controller shows up as connected while every axis and button reads zero.
//
// The Xbox Series X|S pad (product 0x0b13) ships firmware revisions faster than
// GLFW's bundled table is regenerated. Revisions 01050000 and 05050000 are
// present; 09050000 is not. Same hardware, same layout, so the entry below
// reuses the existing mapping verbatim with the newer GUID.
//
// SetGamepadMappings accepts several newline-separated entries in one call, so
// each controller we have had to hand-map is listed below.
//
// Layouts are verified against raw GLFW joystick readings from
// tools/gamepad_probe: shoulders on buttons 6/7, triggers on axes 4/5, sticks
// on axes 0-3.
//
// --- Xbox Series X|S (product 0x0b13) ---
// Ships firmware revisions faster than GLFW's bundled table is regenerated.
// Revisions 01050000 and 05050000 are present; 09050000 is not. Same hardware,
// same layout, so this reuses the existing mapping with the newer GUID.
//
// --- 8BitDo 64 BT (vendor 0xc82d, product 0x1930) ---
// Not in the bundled table at all. Based on the modern 8BitDo Bluetooth family
// layout (SN30 Pro / N30 Pro 2), which the probe confirmed: face buttons on
// 0/1, shoulders on 6/7, triggers on axes 4/5. These pads use the Nintendo
// button convention (a:b1,b:b0,x:b4,y:b3), so the mapping normalises them back
// to SDL positions — "fire" stays the bottom face button, as on the Xbox pad,
// even though 8BitDo labels that button B.
inline constexpr const char* kExtraGamepadMappings =
    "030000005e040000130b000009050000,Xbox Wireless Controller,"
    "a:b0,b:b1,x:b3,y:b4,"
    "back:b10,guide:b12,start:b11,"
    "leftstick:b13,rightstick:b14,"
    "leftshoulder:b6,rightshoulder:b7,"
    "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
    "lefttrigger:a5,righttrigger:a4,"
    "platform:Mac OS X,\n"

    // Physical A is raw button 0 and physical B is raw button 1 on this pad,
    // verified against the controller directly. The sibling-family default
    // (a:b1,b:b0) had them inverted, so raylib reported A as B. Corrected here
    // so the semantic names match the printed labels.
    "03000000c82d00001930000001000000,8BitDo 64 BT,"
    "a:b0,b:b1,x:b4,y:b3,"
    "back:b10,guide:b2,start:b11,"
    "leftstick:b13,rightstick:b14,"
    "leftshoulder:b6,rightshoulder:b7,"
    "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,"
    "lefttrigger:a4,righttrigger:a5,"
    "platform:Mac OS X,\n";
