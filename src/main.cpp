#include "entities.hpp"
#include "game.hpp"
#include "gamepad_mappings.hpp"
#include "net.hpp"
#include "net_serialize.hpp"
#include "postfx.hpp"

#include <raylib.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

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

// Owns the network link and drives the authoritative-host lifecycle. Reachable
// for now from CLI flags (--host / --join <ip>); the menu will call the same
// StartHost/StartClient path in a later step.
struct NetSession
{
    enum class Mode { Local, Host, Join };

    net::NetLink link;
    Mode         mode = Mode::Local;
    std::string  ip;
    uint16_t     port = 45123;
    uint32_t     seed = 1u;

    bool          started       = false;   // BeginOnline has run
    bool          lostPeer      = false;
    bool          connectFailed = false;
    ShipControls  remoteInput{};       // host: the client's latest input
    net::Snapshot latestSnap;          // client: newest snapshot to apply
    bool          haveSnap   = false;
    uint32_t      clientTick = 0;

    // Diagnostics.
    int snapsOut = 0;   // host: snapshots sent
    int snapsIn  = 0;   // client: snapshots applied

    bool IsOnline() const { return mode != Mode::Local; }

    void ParseArgs(int argc, char** argv)
    {
        for (int i = 1; i < argc; i++)
        {
            if (std::strcmp(argv[i], "--host") == 0)
            {
                mode = Mode::Host;
                if (i + 1 < argc && argv[i + 1][0] != '-') port = static_cast<uint16_t>(std::atoi(argv[++i]));
            }
            else if (std::strcmp(argv[i], "--join") == 0)
            {
                mode = Mode::Join;
                if (i + 1 < argc) ip = argv[++i];
                if (i + 1 < argc && argv[i + 1][0] != '-') port = static_cast<uint16_t>(std::atoi(argv[++i]));
            }
        }
    }

    bool Start()
    {
        if (mode == Mode::Local) return true;
        if (!net::GlobalInit()) return false;
        if (mode == Mode::Host)
        {
            TraceLog(LOG_INFO, "NET: hosting on port %u, waiting for a player", port);
            return link.StartHost(port);
        }
        TraceLog(LOG_INFO, "NET: joining %s:%u", ip.c_str(), port);
        return link.StartClient(ip.c_str(), port);
    }

    // Runtime entry points driven by the in-game menu (as opposed to CLI flags).
    void StartHostRuntime()
    {
        if (mode != Mode::Local) return;
        if (!net::GlobalInit()) return;
        mode = Mode::Host;
        if (!link.StartHost(port)) { mode = Mode::Local; net::GlobalShutdown(); return; }
        TraceLog(LOG_INFO, "NET: hosting on port %u", port);
    }

    void StartJoinRuntime(const char* ipStr)
    {
        if (mode != Mode::Local) return;
        if (!net::GlobalInit()) return;
        ip   = ipStr;
        mode = Mode::Join;
        if (!link.StartClient(ip.c_str(), port)) { mode = Mode::Local; net::GlobalShutdown(); return; }
        TraceLog(LOG_INFO, "NET: joining %s:%u", ip.c_str(), port);
    }

    void CancelRuntime()
    {
        if (mode == Mode::Local) return;
        link.Shutdown();
        net::GlobalShutdown();
        mode          = Mode::Local;
        started       = false;
        haveSnap      = false;
        lostPeer      = false;
        connectFailed = false;
        snapsOut      = 0;
        snapsIn       = 0;
        clientTick    = 0;
    }

    // Poll the socket, handle connect/disconnect and inbound messages, and start
    // the match when the peers are ready.
    void Service(Game& game)
    {
        if (mode == Mode::Local) return;
        link.Poll();

        // Host: a connected peer means both players are present — start.
        if (mode == Mode::Host && !started && link.State() == net::LinkState::Connected)
        {
            net::ByteWriter w;
            net::StartGameMsg m;
            m.playerCount = 2;
            m.yourPlayer  = 1;
            m.seed        = seed;
            m.write(w);
            link.SendReliable(w.buf.data(), w.buf.size());

            game.BeginOnline(NetRole::Host, 0, seed);
            started = true;
            TraceLog(LOG_INFO, "NET: peer connected, host started match");
        }

        net::Packet pkt;
        while (link.Receive(pkt))
        {
            net::ByteReader r(pkt.data.data(), pkt.data.size());
            const auto type = static_cast<net::MsgType>(r.u8());

            if (type == net::MsgType::Input && mode == Mode::Host)
            {
                remoteInput = net::InputMsg::read(r).controls;
            }
            else if (type == net::MsgType::StartGame && mode == Mode::Join)
            {
                const auto m = net::StartGameMsg::read(r);
                seed = m.seed;
                game.BeginOnline(NetRole::Client, m.yourPlayer, seed);
                started = true;
                TraceLog(LOG_INFO, "NET: got StartGame, joined as player %u", m.yourPlayer);
            }
            else if (type == net::MsgType::Snapshot && mode == Mode::Join)
            {
                latestSnap = net::Snapshot::read(r);
                haveSnap   = true;
            }
        }

        if (started && link.State() == net::LinkState::Disconnected) lostPeer = true;

        // A client whose connect attempt timed out (ENet gives up after a few
        // seconds) lands here: online, never started, socket now closed.
        if (mode != Mode::Local && !started && link.State() == net::LinkState::Disconnected)
            connectFailed = true;
    }
};
}  // namespace

int main(int argc, char** argv)
{
    NetSession session;
    session.ParseArgs(argc, argv);
    // Resizable is safe because Present letterboxes the fixed-size scene into
    // whatever the window becomes.
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(kScreenW, kScreenH, "Neon Asteroids");

    // Must follow InitWindow: this forwards to GLFW, which needs to be up.
    SetGamepadMappings(kExtraGamepadMappings);

    InitAudioDevice();
    SetExitKey(KEY_NULL);   // Esc is handled below, not a hard quit
    // No SetTargetFPS: vsync (FLAG_VSYNC_HINT) paces the frames on its own.
    // Adding a software 60 fps cap on top fights vsync — on a 120 Hz display the
    // two beat against each other and frames land on the wrong refresh boundary,
    // which reads as stutter even though nothing is dropping frames. The fixed
    // 60 Hz simulation step (kFixedDt) is independent of the render rate.

    PostFX fx;
    fx.Load(kScreenW, kScreenH);

    Game game;
    game.Init();

    if (!session.Start())
        TraceLog(LOG_ERROR, "NET: failed to start session; continuing offline");

    // The simulation runs on a fixed 60 Hz step, decoupled from render rate.
    // This gives the sim a stable cadence — the foundation the networked host
    // will tick against — and makes physics independent of framerate. Rendering
    // still happens once per real frame. At the usual vsync-locked 60 fps this
    // is exactly one tick per frame; the accumulator only matters when a frame
    // runs long or short.
    constexpr float kFixedDt   = 1.0f / 60.0f;
    constexpr float kMaxCatchUp = 0.25f;   // cap to avoid a death spiral after a hitch
    float accumulator = 0.0f;

    // Hyperspace is edge-triggered (fires on the one frame it is pressed), but
    // the render rate (vsync, e.g. 120 Hz) is higher than the 60 Hz sim, so some
    // frames run no tick at all. A press sampled on such a frame would be lost.
    // These latches hold each player's press until a tick actually consumes it.
    bool hyperLatch[kMaxPlayers] = {};

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

        // Leave an online match (during play or at game-over) back to the menu.
        // Tearing down the socket notifies the peer, which then returns to its
        // own menu. M on the keyboard, B on the pad.
        if (session.IsOnline()
            && (IsKeyPressed(KEY_M)
                || (IsGamepadAvailable(0) && IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT))))
        {
            session.CancelRuntime();
            game.NetReturnToMenu(nullptr);
        }

        // The menu asks main to open or tear down the socket.
        switch (game.ConsumeNetRequest())
        {
            case NetRequest::Host:
                session.StartHostRuntime();
                game.SetHostInfo(net::LocalIP().c_str(), session.port);
                break;
            case NetRequest::Join:   session.StartJoinRuntime(game.JoinIp()); break;
            case NetRequest::Cancel: session.CancelRuntime();                 break;
            case NetRequest::None:                                            break;
        }

        // Network lifecycle: connect, start the match, receive input/snapshots.
        session.Service(game);
        if (session.lostPeer || session.connectFailed)
        {
            const char* msg = session.lostPeer ? "PLAYER LEFT" : "COULD NOT CONNECT";
            TraceLog(LOG_INFO, "NET: %s, returning to menu", msg);
            session.CancelRuntime();
            game.NetReturnToMenu(msg);
        }

        const NetRole role = game.GetRole();

        if (role == NetRole::Client)
        {
            // A client never simulates: send this frame's input, then render the
            // newest authoritative snapshot the host has sent.
            net::ByteWriter w;
            net::InputMsg m;
            m.tick     = session.clientTick++;
            m.controls = game.SampleControls(0);   // local human uses P1 bindings
            m.write(w);
            session.link.SendUnreliable(w.buf.data(), w.buf.size());

            if (session.haveSnap)
            {
                game.ApplySnapshot(session.latestSnap);
                session.haveSnap = false;
                session.snapsIn++;
            }

            // Animate the local cosmetic layers every frame (independent of
            // snapshot arrival), so particles age and the starfield drifts.
            game.UpdateClientView(frameDt);
            accumulator = 0.0f;
        }
        else if (game.IsPlaying())
        {
            accumulator = std::min(accumulator + frameDt, kMaxCatchUp);

            // Sample this frame's intent once, then feed it to every tick.
            ShipControls controls[kMaxPlayers];
            if (role == NetRole::Host)
            {
                controls[0] = game.SampleControls(0);   // local host player
                controls[1] = session.remoteInput;      // client, over the wire
                session.remoteInput.hyperspace = false;  // captured into the latch below
            }
            else
            {
                for (int p = 0; p < kMaxPlayers; p++) controls[p] = game.SampleControls(p);
            }

            // Latch the hyperspace edge so a zero-tick frame doesn't drop it, and
            // feed the latched value to the ticks.
            for (int p = 0; p < kMaxPlayers; p++)
            {
                if (controls[p].hyperspace) hyperLatch[p] = true;
                controls[p].hyperspace = hyperLatch[p];
            }

            while (accumulator >= kFixedDt)
            {
                game.Update(kFixedDt, controls);
                // A press fires once: clear both the per-tick flag and the latch.
                for (int p = 0; p < kMaxPlayers; p++)
                {
                    controls[p].hyperspace = false;
                    hyperLatch[p]          = false;
                }
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

        // Host publishes the authoritative state every frame, including during
        // game-over, so the client keeps following (and sees the game-over
        // screen and any restart) rather than freezing on the last play frame.
        if (role == NetRole::Host && session.started)
        {
            net::ByteWriter w;
            game.SerializeSnapshot(w);
            session.link.SendUnreliable(w.buf.data(), w.buf.size());
            session.snapsOut++;
        }

        fx.BeginScene();
            game.Draw();
        fx.EndScene();

        BeginDrawing();
            ClearBackground(BLACK);
            fx.Present(game.ShakeOffset());
        EndDrawing();
    }

    if (session.IsOnline())
    {
        session.link.Shutdown();
        net::GlobalShutdown();
    }

    game.Shutdown();
    fx.Unload();
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
