#pragma once

// Wire serialization for the authoritative-host netcode. No sockets here — just
// turning game values into bytes and back — so it is pure, portable, and
// unit-testable without any networking (see tools/net_selftest.cpp).
//
// Encoding is explicit little-endian, field by field. All current targets
// (macOS ARM64, Windows x64, Linux x64) are little-endian IEEE-754, so this is
// also correct if a big-endian target ever appears — nothing relies on struct
// layout or native byte order.

#include "entities.hpp"   // Vector2, ShipControls

#include <cstdint>
#include <cstring>
#include <vector>

namespace net
{

// ------------------------------------------------------------- writer/reader ---

struct ByteWriter
{
    std::vector<uint8_t> buf;

    void u8(uint8_t v)  { buf.push_back(v); }
    void u16(uint16_t v){ u8(v & 0xff); u8((v >> 8) & 0xff); }
    void u32(uint32_t v){ for (int i = 0; i < 4; i++) u8((v >> (8 * i)) & 0xff); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }

    void f32(float v)
    {
        uint32_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        u32(bits);
    }

    void vec2(Vector2 v) { f32(v.x); f32(v.y); }
};

struct ByteReader
{
    const uint8_t* p;
    const uint8_t* end;
    bool           ok = true;   // false once a read runs past the buffer

    ByteReader(const uint8_t* data, size_t n) : p(data), end(data + n) {}

    uint8_t u8()
    {
        if (p >= end) { ok = false; return 0; }
        return *p++;
    }
    uint16_t u16() { uint16_t a = u8(); uint16_t b = u8(); return static_cast<uint16_t>(a | (b << 8)); }
    uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(u8()) << (8 * i); return v; }
    int32_t  i32() { return static_cast<int32_t>(u32()); }

    float f32()
    {
        uint32_t bits = u32();
        float    v;
        std::memcpy(&v, &bits, sizeof(v));
        return v;
    }

    Vector2 vec2() { float x = f32(); float y = f32(); return {x, y}; }
};

// ------------------------------------------------------------------ messages ---

// The first byte of every packet. Snapshot/Input travel on the unreliable
// channel (latest wins); the rest are reliable lifecycle messages.
enum class MsgType : uint8_t
{
    JoinRequest = 1,
    JoinAccept  = 2,   // host tells the client which player index it is
    StartGame   = 3,   // host -> client: player count + shared RNG seed
    Input       = 4,   // client -> host, every tick
    Snapshot    = 5,   // host -> client, every tick
    Disconnect  = 6,
};

// Client -> host. The tick lets the host discard stale/out-of-order inputs.
struct InputMsg
{
    uint32_t     tick = 0;
    ShipControls controls{};

    void write(ByteWriter& w) const
    {
        w.u8(static_cast<uint8_t>(MsgType::Input));
        w.u32(tick);
        w.f32(controls.turn);
        w.u8(controls.thrust ? 1 : 0);
        w.u8(controls.fire ? 1 : 0);
        w.u8(controls.hyperspace ? 1 : 0);
    }

    // Assumes the MsgType byte has already been consumed by the dispatcher.
    static InputMsg read(ByteReader& r)
    {
        InputMsg m;
        m.tick               = r.u32();
        m.controls.turn      = r.f32();
        m.controls.thrust    = r.u8() != 0;
        m.controls.fire      = r.u8() != 0;
        m.controls.hyperspace = r.u8() != 0;
        return m;
    }
};

// Host -> client, sent once when the match begins. The shared seed lets both
// sides start from the same asteroid field (the client renders snapshots, but a
// shared seed keeps any local cosmetic RNG coherent).
struct StartGameMsg
{
    uint8_t  playerCount = 2;
    uint8_t  yourPlayer  = 1;
    uint32_t seed        = 0;

    void write(ByteWriter& w) const
    {
        w.u8(static_cast<uint8_t>(MsgType::StartGame));
        w.u8(playerCount);
        w.u8(yourPlayer);
        w.u32(seed);
    }

    static StartGameMsg read(ByteReader& r)
    {
        StartGameMsg m;
        m.playerCount = r.u8();
        m.yourPlayer  = r.u8();
        m.seed        = r.u32();
        return m;
    }
};

// The Snapshot message (full entity state host -> client) is defined in Phase 1,
// once the entity set it must carry is final. The primitives above already
// cover everything it needs (vec2, floats, counts).

}  // namespace net
