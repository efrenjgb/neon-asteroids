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
