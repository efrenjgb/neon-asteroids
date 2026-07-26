// Round-trips the network serialization primitives and messages through a byte
// buffer and checks they come back bit-identical. Pure logic, no window and no
// sockets, so it runs headless in CI or locally: exit code 0 = all passed.

#include "../src/net_serialize.hpp"

#include <cstdio>
#include <cstring>

namespace
{
int g_failures = 0;

void check(bool cond, const char* what)
{
    if (!cond)
    {
        std::printf("FAIL: %s\n", what);
        g_failures++;
    }
}

// Bit-exact float compare — serialization must not perturb the value at all.
bool bitEqual(float a, float b)
{
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}
}  // namespace

int main()
{
    using namespace net;

    // --- primitives round-trip ---
    {
        ByteWriter w;
        w.u8(0xAB);
        w.u16(0x1234);
        w.u32(0xDEADBEEF);
        w.i32(-42);
        w.f32(3.14159265f);
        w.f32(-0.0f);
        w.f32(1e-7f);
        w.vec2({123.5f, -678.25f});

        ByteReader r(w.buf.data(), w.buf.size());
        check(r.u8() == 0xAB, "u8");
        check(r.u16() == 0x1234, "u16");
        check(r.u32() == 0xDEADBEEF, "u32");
        check(r.i32() == -42, "i32 negative");
        check(bitEqual(r.f32(), 3.14159265f), "f32 pi");
        check(bitEqual(r.f32(), -0.0f), "f32 negative zero");
        check(bitEqual(r.f32(), 1e-7f), "f32 tiny");
        const Vector2 v = r.vec2();
        check(bitEqual(v.x, 123.5f) && bitEqual(v.y, -678.25f), "vec2");
        check(r.ok, "reader stayed in bounds");
    }

    // --- InputMsg round-trip ---
    {
        InputMsg in;
        in.tick = 987654;
        in.controls.turn = -0.73f;
        in.controls.thrust = true;
        in.controls.fire = false;
        in.controls.hyperspace = true;

        ByteWriter w;
        in.write(w);

        ByteReader r(w.buf.data(), w.buf.size());
        check(static_cast<MsgType>(r.u8()) == MsgType::Input, "InputMsg type tag");
        const InputMsg out = InputMsg::read(r);
        check(out.tick == in.tick, "InputMsg tick");
        check(bitEqual(out.controls.turn, in.controls.turn), "InputMsg turn");
        check(out.controls.thrust == in.controls.thrust, "InputMsg thrust");
        check(out.controls.fire == in.controls.fire, "InputMsg fire");
        check(out.controls.hyperspace == in.controls.hyperspace, "InputMsg hyperspace");
        check(r.ok, "InputMsg consumed cleanly");
    }

    // --- StartGameMsg round-trip ---
    {
        StartGameMsg in;
        in.playerCount = 2;
        in.yourPlayer = 1;
        in.seed = 0xC0FFEE42;

        ByteWriter w;
        in.write(w);

        ByteReader r(w.buf.data(), w.buf.size());
        check(static_cast<MsgType>(r.u8()) == MsgType::StartGame, "StartGameMsg type tag");
        const StartGameMsg out = StartGameMsg::read(r);
        check(out.playerCount == in.playerCount, "StartGameMsg playerCount");
        check(out.yourPlayer == in.yourPlayer, "StartGameMsg yourPlayer");
        check(out.seed == in.seed, "StartGameMsg seed");
    }

    // --- Snapshot round-trip: a populated world must survive intact ---
    {
        Snapshot snap;
        snap.tick      = 4242;
        snap.gameState = 1;
        snap.wave      = 9;
        snap.shake     = 0.42f;

        Ship s0;
        s0.player = 0; s0.lives = 3;
        s0.alive = true; s0.thrusting = true; s0.shield = true; s0.fanFire = true;
        s0.pos = {640.0f, 360.0f}; s0.vel = {-12.5f, 33.0f};
        s0.rot = 1.2345f; s0.invuln = 0.75f;
        snap.ships.push_back(s0);

        Ship s1;
        s1.player = 1; s1.lives = 1;
        s1.alive = true; s1.warping = true;
        s1.pos = {100.0f, 200.0f};
        s1.warp = 0.3f; s1.warpDuration = 0.55f; s1.warpPortal = {800.0f, 150.0f};
        snap.ships.push_back(s1);

        Asteroid a;
        a.pos = {12.0f, 34.0f}; a.rot = 0.5f; a.radius = 58.0f; a.tier = 3;
        a.shape = {0.9f, 1.1f, 0.8f, 1.25f, 1.0f, 0.72f, 1.18f, 0.95f, 1.05f};
        snap.asteroids.push_back(a);

        Bullet b0; b0.pos = {1.0f, 2.0f}; b0.vel = {700.0f, 0.0f}; b0.owner = 1;
        Bullet b1; b1.pos = {3.0f, 4.0f}; b1.vel = {0.0f, -380.0f}; b1.owner = kUfoBulletOwner;
        snap.bullets.push_back(b0);
        snap.bullets.push_back(b1);

        Ufo u; u.pos = {500.0f, 90.0f}; u.radius = 14.0f; u.tier = 1;
        snap.ufos.push_back(u);

        Powerup p; p.pos = {250.0f, 250.0f}; p.type = PowerupType::Shield;
        p.radius = 16.0f; p.spin = 2.1f; p.life = 7.5f;
        snap.powerups.push_back(p);

        snap.events.push_back({EventType::Explosion, {12.0f, 34.0f}, 3});
        snap.events.push_back({EventType::Powerup,  {250.0f, 250.0f}, static_cast<uint8_t>(PowerupType::Shield)});

        ByteWriter w;
        snap.write(w);

        ByteReader r(w.buf.data(), w.buf.size());
        check(static_cast<MsgType>(r.u8()) == MsgType::Snapshot, "Snapshot type tag");
        const Snapshot out = Snapshot::read(r);

        check(r.ok, "Snapshot consumed cleanly");
        check(out.tick == snap.tick && out.gameState == snap.gameState
              && out.wave == snap.wave && bitEqual(out.shake, snap.shake), "Snapshot header");

        check(out.ships.size() == 2, "Snapshot ship count");
        check(out.ships[0].player == 0 && out.ships[0].lives == 3
              && out.ships[0].alive && out.ships[0].thrusting
              && out.ships[0].shield && out.ships[0].fanFire
              && bitEqual(out.ships[0].pos.x, 640.0f) && bitEqual(out.ships[0].vel.y, 33.0f)
              && bitEqual(out.ships[0].rot, 1.2345f) && bitEqual(out.ships[0].invuln, 0.75f),
              "Snapshot ship 0 fields+flags");
        check(out.ships[1].warping && bitEqual(out.ships[1].warp, 0.3f)
              && bitEqual(out.ships[1].warpDuration, 0.55f)
              && bitEqual(out.ships[1].warpPortal.x, 800.0f), "Snapshot ship 1 warp state");

        check(out.asteroids.size() == 1 && out.asteroids[0].tier == 3
              && out.asteroids[0].shape.size() == 9
              && bitEqual(out.asteroids[0].shape[3], 1.25f)
              && bitEqual(out.asteroids[0].radius, 58.0f), "Snapshot asteroid + shape");

        check(out.bullets.size() == 2 && out.bullets[0].owner == 1
              && out.bullets[1].owner == kUfoBulletOwner, "Snapshot bullet owners (incl -1)");

        check(out.ufos.size() == 1 && out.ufos[0].tier == 1
              && bitEqual(out.ufos[0].radius, 14.0f), "Snapshot ufo");

        check(out.powerups.size() == 1 && out.powerups[0].type == PowerupType::Shield
              && bitEqual(out.powerups[0].spin, 2.1f) && bitEqual(out.powerups[0].life, 7.5f),
              "Snapshot powerup");

        check(out.events.size() == 2 && out.events[0].type == EventType::Explosion
              && out.events[0].param == 3
              && out.events[1].type == EventType::Powerup, "Snapshot events");
    }

    // --- empty snapshot (start of a wave transition, nothing on screen) ---
    {
        Snapshot snap;
        snap.tick = 1;
        ByteWriter w;
        snap.write(w);
        ByteReader r(w.buf.data(), w.buf.size());
        r.u8();
        const Snapshot out = Snapshot::read(r);
        check(r.ok && out.ships.empty() && out.asteroids.empty()
              && out.events.empty(), "empty Snapshot round-trips");
    }

    // --- truncated buffer is detected, not read past ---
    {
        ByteReader r(nullptr, 0);
        r.u32();
        check(!r.ok, "reader flags underrun");
    }

    if (g_failures == 0)
    {
        std::printf("net_selftest: all checks passed\n");
        return 0;
    }
    std::printf("net_selftest: %d failure(s)\n", g_failures);
    return 1;
}
