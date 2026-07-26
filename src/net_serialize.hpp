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

// ------------------------------------------------------------------- events ---

// Cosmetic/audio moments the host emits each tick. The client has no simulation
// to produce these itself, so it replays them into local particles, sound and
// screen shake. `param` carries a small integer where a type needs one (an
// explosion's tier, a powerup's PowerupType).
enum class EventType : uint8_t
{
    Shoot = 0,
    Explosion,     // param = asteroid tier
    ShipDeath,
    UfoShoot,
    UfoDeath,
    HyperEnter,
    HyperExit,
    Powerup,       // param = PowerupType
    ShieldBreak,
    WaveStart,
};

struct NetEvent
{
    EventType type  = EventType::Shoot;
    Vector2   pos{};
    uint8_t   param = 0;
};

// ----------------------------------------------------------------- snapshot ---
//
// Full host -> client world state, sent every tick. Deliberately stateless: the
// client replaces its whole entity set from each snapshot, so there are no
// entity IDs to track. Only render-relevant fields travel; simulation-only
// state (cooldowns, spawn timers, asteroid velocity/rotSpeed) is omitted, since
// the client never steps the sim.

namespace detail
{

// Ship flags packed into one byte.
enum ShipFlag : uint8_t
{
    ShipAlive     = 1 << 0,
    ShipThrusting = 1 << 1,
    ShipWarping   = 1 << 2,
    ShipShield    = 1 << 3,
    ShipFanFire   = 1 << 4,
};

inline void writeShip(ByteWriter& w, const Ship& s)
{
    uint8_t flags = 0;
    if (s.alive)     flags |= ShipAlive;
    if (s.thrusting) flags |= ShipThrusting;
    if (s.warping)   flags |= ShipWarping;
    if (s.shield)    flags |= ShipShield;
    if (s.fanFire)   flags |= ShipFanFire;

    w.u8(static_cast<uint8_t>(s.player));
    w.u8(static_cast<uint8_t>(s.lives < 0 ? 0 : s.lives));
    w.u8(flags);
    w.vec2(s.pos);
    w.vec2(s.vel);
    w.f32(s.rot);
    w.f32(s.invuln);
    w.f32(s.warp);
    w.f32(s.warpDuration);
    w.vec2(s.warpPortal);
}

inline Ship readShip(ByteReader& r)
{
    Ship s;
    s.player = r.u8();
    s.lives  = r.u8();
    const uint8_t flags = r.u8();
    s.alive     = (flags & ShipAlive) != 0;
    s.thrusting = (flags & ShipThrusting) != 0;
    s.warping   = (flags & ShipWarping) != 0;
    s.shield    = (flags & ShipShield) != 0;
    s.fanFire   = (flags & ShipFanFire) != 0;
    s.pos          = r.vec2();
    s.vel          = r.vec2();
    s.rot          = r.f32();
    s.invuln       = r.f32();
    s.warp         = r.f32();
    s.warpDuration = r.f32();
    s.warpPortal   = r.vec2();
    return s;
}

inline void writeAsteroid(ByteWriter& w, const Asteroid& a)
{
    w.vec2(a.pos);
    w.f32(a.rot);
    w.f32(a.radius);
    w.u8(static_cast<uint8_t>(a.tier));
    w.u8(static_cast<uint8_t>(a.shape.size()));
    for (float m : a.shape) w.f32(m);
}

inline Asteroid readAsteroid(ByteReader& r)
{
    Asteroid a;
    a.pos    = r.vec2();
    a.rot    = r.f32();
    a.radius = r.f32();
    a.tier   = r.u8();
    const uint8_t n = r.u8();
    a.shape.reserve(n);
    for (uint8_t i = 0; i < n; i++) a.shape.push_back(r.f32());
    return a;
}

inline void writeBullet(ByteWriter& w, const Bullet& b)
{
    w.vec2(b.pos);
    w.vec2(b.vel);
    w.u8(static_cast<uint8_t>(static_cast<int8_t>(b.owner)));   // -1 (UFO) round-trips
}

inline Bullet readBullet(ByteReader& r)
{
    Bullet b;
    b.pos   = r.vec2();
    b.vel   = r.vec2();
    b.owner = static_cast<int8_t>(r.u8());
    return b;
}

inline void writeUfo(ByteWriter& w, const Ufo& u)
{
    w.vec2(u.pos);
    w.f32(u.radius);
    w.u8(static_cast<uint8_t>(u.tier));
}

inline Ufo readUfo(ByteReader& r)
{
    Ufo u;
    u.pos    = r.vec2();
    u.radius = r.f32();
    u.tier   = r.u8();
    return u;
}

inline void writePowerup(ByteWriter& w, const Powerup& p)
{
    w.vec2(p.pos);
    w.u8(static_cast<uint8_t>(p.type));
    w.f32(p.radius);
    w.f32(p.spin);
    w.f32(p.life);
}

inline Powerup readPowerup(ByteReader& r)
{
    Powerup p;
    p.pos    = r.vec2();
    p.type   = static_cast<PowerupType>(r.u8());
    p.radius = r.f32();
    p.spin   = r.f32();
    p.life   = r.f32();
    return p;
}

}  // namespace detail

struct Snapshot
{
    uint32_t tick      = 0;
    uint8_t  gameState = 0;   // 0 = Playing, 1 = GameOver (client mirrors)
    uint16_t wave      = 1;
    float    shake     = 0.0f;

    std::vector<Ship>     ships;
    std::vector<Asteroid> asteroids;
    std::vector<Bullet>   bullets;
    std::vector<Ufo>      ufos;
    std::vector<Powerup>  powerups;
    std::vector<NetEvent> events;

    void write(ByteWriter& w) const
    {
        w.u8(static_cast<uint8_t>(MsgType::Snapshot));
        w.u32(tick);
        w.u8(gameState);
        w.u16(wave);
        w.f32(shake);

        w.u8(static_cast<uint8_t>(ships.size()));
        for (const Ship& s : ships) detail::writeShip(w, s);

        w.u16(static_cast<uint16_t>(asteroids.size()));
        for (const Asteroid& a : asteroids) detail::writeAsteroid(w, a);

        w.u16(static_cast<uint16_t>(bullets.size()));
        for (const Bullet& b : bullets) detail::writeBullet(w, b);

        w.u8(static_cast<uint8_t>(ufos.size()));
        for (const Ufo& u : ufos) detail::writeUfo(w, u);

        w.u8(static_cast<uint8_t>(powerups.size()));
        for (const Powerup& p : powerups) detail::writePowerup(w, p);

        w.u8(static_cast<uint8_t>(events.size()));
        for (const NetEvent& e : events)
        {
            w.u8(static_cast<uint8_t>(e.type));
            w.vec2(e.pos);
            w.u8(e.param);
        }
    }

    // Assumes the MsgType byte has already been consumed by the dispatcher.
    static Snapshot read(ByteReader& r)
    {
        Snapshot s;
        s.tick      = r.u32();
        s.gameState = r.u8();
        s.wave      = r.u16();
        s.shake     = r.f32();

        const uint8_t shipCount = r.u8();
        for (uint8_t i = 0; i < shipCount && r.ok; i++) s.ships.push_back(detail::readShip(r));

        const uint16_t asteroidCount = r.u16();
        for (uint16_t i = 0; i < asteroidCount && r.ok; i++) s.asteroids.push_back(detail::readAsteroid(r));

        const uint16_t bulletCount = r.u16();
        for (uint16_t i = 0; i < bulletCount && r.ok; i++) s.bullets.push_back(detail::readBullet(r));

        const uint8_t ufoCount = r.u8();
        for (uint8_t i = 0; i < ufoCount && r.ok; i++) s.ufos.push_back(detail::readUfo(r));

        const uint8_t powerupCount = r.u8();
        for (uint8_t i = 0; i < powerupCount && r.ok; i++) s.powerups.push_back(detail::readPowerup(r));

        const uint8_t eventCount = r.u8();
        for (uint8_t i = 0; i < eventCount && r.ok; i++)
        {
            NetEvent e;
            e.type  = static_cast<EventType>(r.u8());
            e.pos   = r.vec2();
            e.param = r.u8();
            s.events.push_back(e);
        }

        return s;
    }
};

}  // namespace net
