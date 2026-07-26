#pragma once

#include <raylib.h>
#include <random>
#include <vector>

inline constexpr int kScreenW = 1280;
inline constexpr int kScreenH = 720;

// Everything player-indexed is sized by this. Raising it and pushing another
// Ship into Game::ships_ is what enables a second player.
inline constexpr int kMaxPlayers = 2;

// Fraction of the hyperspace warp spent flying into the portal. The remainder
// is the ship being gone before it re-emerges. Shared because the simulation
// moves the ship along this curve and the renderer sizes it to match.
inline constexpr float kWarpTravelFraction = 0.70f;

inline float SmoothStep01(float t)
{
    t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;
    return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------- random ---

inline std::mt19937& Rng()
{
    static std::mt19937 gen(std::random_device{}());
    return gen;
}

inline float RandF(float lo, float hi)
{
    return std::uniform_real_distribution<float>{lo, hi}(Rng());
}

inline int RandI(int lo, int hi)
{
    return std::uniform_int_distribution<int>{lo, hi}(Rng());
}

// --------------------------------------------------------------- palette ---

inline constexpr Color kSpaceBg    = {  6,   8,  20, 255};
inline constexpr Color kThrustHot  = {255, 220, 130, 255};
inline constexpr Color kThrustCool = {255,  80,  40, 255};

// Per-player so two ships stay distinguishable on screen.
Color ShipColor(int player);
Color BulletColor(int player);
Color AsteroidColor(int tier);

// Hostile green, deliberately outside the cyan/amber player range and the
// magenta/violet asteroid range, so a threat never reads as scenery.
inline constexpr Color kUfoGreen = {120, 255, 130, 255};

// -------------------------------------------------------------- entities ---

struct Ship
{
    Vector2 pos{};
    Vector2 vel{};
    float   rot       = -PI / 2.0f;   // radians, 0 = facing +X
    bool    thrusting = false;
    bool    alive     = true;

    // Per-ship rather than per-game, so a second player keeps its own.
    float invuln       = 0.0f;   // seconds of damage immunity remaining
    float fireCooldown = 0.0f;
    float respawnTimer = 0.0f;
    int   lives        = 3;

    // Powerup effects. Both reset on death, since SpawnShip rebuilds the ship
    // from a default — losing them is the cost of dying.
    bool fanFire = false;   // spread shot
    bool shield  = false;   // one free hit, drawn as a ring like spawn immunity

    // Hyperspace. The ship is hidden and non-collidable while `warping`;
    // `warp` counts down to the moment it re-emerges somewhere else. Immunity
    // runs off `invuln` and outlasts the disappearance.
    bool    warping      = false;
    float   warp         = 0.0f;   // seconds left before re-emerging
    float   warpDuration = 0.0f;   // what `warp` started at, for the animation
    Vector2 warpFrom{};            // where the jump was triggered
    Vector2 warpPortal{};          // the circle, opened ahead along travel

    int player = 0;   // selects input bindings and colour
};

// Out of phase during the warp. Gates collision and anything that should treat
// the ship as absent — drawing handles the warp itself, since the collapse is
// the animation the player sees.
inline bool ShipVisible(const Ship& s) { return s.alive && !s.warping; }

// Per-frame intent for one ship, decoupled from where it came from: local
// input, a networked peer, or an AI all just produce one of these. Lives here
// rather than in game.hpp so the network serializer can use it without pulling
// in the whole Game.
struct ShipControls
{
    float turn       = 0.0f;   // -1 left .. +1 right
    bool  thrust     = false;
    bool  fire       = false;
    bool  hyperspace = false;  // edge-triggered by the caller
};

struct Asteroid
{
    Vector2 pos{};
    Vector2 vel{};
    float   rot      = 0.0f;
    float   rotSpeed = 0.0f;
    float   radius   = 0.0f;
    int     tier     = 3;              // 3 large, 2 medium, 1 small
    std::vector<float> shape;          // per-vertex radius multipliers
};

// A saucer that crosses the screen and shoots back. Unlike asteroids it is an
// active threat, which is the point: later waves get a new verb rather than
// just more rocks.
struct Ufo
{
    Vector2 pos{};
    Vector2 vel{};
    float   radius    = 22.0f;
    int     tier      = 2;     // 2 = large and inaccurate, 1 = small and aimed
    float   fireTimer = 0.0f;
    float   jinkTimer = 0.0f;  // until the next vertical course change
    float   warbleTimer = 0.0f;
    float   travelled = 0.0f;  // horizontal distance, for despawn
};

// Bullets are shared between players and saucers. Negative owner means the
// shot came from a UFO, which is what decides who it can hurt.
inline constexpr int kUfoBulletOwner = -1;

struct Bullet
{
    Vector2 pos{};
    Vector2 vel{};
    float   life  = 0.0f;
    int     owner = 0;                 // player index, or kUfoBulletOwner
};

inline bool IsEnemyBullet(const Bullet& b) { return b.owner < 0; }

struct Particle
{
    Vector2 pos{};
    Vector2 vel{};
    float   life    = 0.0f;
    float   maxLife = 0.0f;
    float   size    = 0.0f;
    Color   color{};
};

struct Star
{
    Vector2 pos{};
    float   depth = 0.0f;              // 0 far .. 1 near
    float   size  = 0.0f;
};

// Dropped by a destroyed saucer. Collected by ship contact; effect is applied
// per-ship so it works unchanged in two-player.
enum class PowerupType
{
    ExtraLife,   // +1 life immediately
    FanFire,     // spread shot, until the ship dies
    Shield,      // absorbs one hit without costing a life
    Count,
};

struct Powerup
{
    Vector2     pos{};
    Vector2     vel{};
    PowerupType type   = PowerupType::ExtraLife;
    float       radius = 16.0f;
    float       life   = 0.0f;   // seconds until it despawns
    float       spin   = 0.0f;
};

Color PowerupColor(PowerupType type);

// ---------------------------------------------------------------- helpers ---

// Toroidal playfield: anything leaving one edge re-enters on the opposite one.
void    WrapPosition(Vector2& p, float margin);

// True modulo wrap into screen bounds. WrapPosition only handles points that
// have just stepped past an edge; this copes with arbitrary distances, which
// the hyperspace portal needs since it opens some way ahead of the ship.
Vector2 WrapToScreen(Vector2 p);

// Shortest delta between two points across the wrap seam.
Vector2 WrappedDelta(Vector2 a, Vector2 b);

float   AsteroidRadiusForTier(int tier);

// `speedScale` lets later waves move faster without changing the tier ranges.
Asteroid MakeAsteroid(Vector2 pos, int tier, float speedScale = 1.0f);

// ---------------------------------------------------------------- drawing ---

// The neon look: a wide dim halo, a solid mid stroke, and a near-white core.
void DrawNeonLine(Vector2 a, Vector2 b, float thickness, Color c);
void DrawNeonPolyline(const std::vector<Vector2>& pts, float thickness, Color c, bool closed);
void DrawNeonDot(Vector2 p, float radius, Color c);

void DrawShip(const Ship& ship, float time);
void DrawUfo(const Ufo& u, float time);
void DrawPowerup(const Powerup& p, float time);
void DrawAsteroid(const Asteroid& a);
void DrawBullet(const Bullet& b);
void DrawParticle(const Particle& p);
