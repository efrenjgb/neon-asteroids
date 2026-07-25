#include "entities.hpp"

#include <raymath.h>
#include <cmath>

namespace
{
// Local-space ship hull, nose pointing +X.
const std::vector<Vector2> kHull = {
    { 18.0f,   0.0f},
    {-12.0f, -11.0f},
    { -6.0f,   0.0f},
    {-12.0f,  11.0f},
};

// Draws the portal at every wrapped duplicate, so one opening across a screen
// edge appears on both sides instead of being sliced in half.
void DrawPortalRings(Vector2 centre, float r, Color tint)
{
    for (int dy = -1; dy <= 1; dy++)
    {
        for (int dx = -1; dx <= 1; dx++)
        {
            const Vector2 p = {
                centre.x + static_cast<float>(dx) * static_cast<float>(kScreenW),
                centre.y + static_cast<float>(dy) * static_cast<float>(kScreenH),
            };

            if (p.x < -r || p.x > kScreenW + r) continue;
            if (p.y < -r || p.y > kScreenH + r) continue;

            DrawCircleLinesV(p, r,         Fade(tint, 0.60f));
            DrawCircleLinesV(p, r * 0.62f, Fade(tint, 0.38f));
            DrawCircleLinesV(p, r * 0.28f, Fade(tint, 0.22f));
        }
    }
}

Color Brighten(Color c, float amount)
{
    auto up = [&](unsigned char v) {
        return static_cast<unsigned char>(Clamp(v + (255 - v) * amount, 0.0f, 255.0f));
    };
    return {up(c.r), up(c.g), up(c.b), c.a};
}
}  // namespace

Color ShipColor(int player)
{
    // Cyan and amber: far enough apart in hue to read instantly, and neither
    // collides with the magenta/violet asteroid range.
    return (player == 0) ? Color{ 60, 255, 230, 255}
                         : Color{255, 185,  60, 255};
}

Color BulletColor(int player)
{
    return (player == 0) ? Color{190, 255, 255, 255}
                         : Color{255, 225, 170, 255};
}

Color AsteroidColor(int tier)
{
    switch (tier)
    {
        case 3:  return {255,  60, 180, 255};   // magenta
        case 2:  return {185,  85, 255, 255};   // violet
        default: return {255, 125, 225, 255};   // pink
    }
}

Color PowerupColor(PowerupType type)
{
    // Each distinct from the players, saucers and asteroids so a pickup never
    // reads as a threat or as scenery.
    switch (type)
    {
        case PowerupType::ExtraLife: return {120, 255, 180, 255};   // mint
        case PowerupType::FanFire:   return {255, 210,  70, 255};   // gold
        case PowerupType::Shield:    return {120, 200, 255, 255};   // ice blue
        default:                     return WHITE;
    }
}

void WrapPosition(Vector2& p, float margin)
{
    if (p.x < -margin)            p.x = kScreenW + margin;
    if (p.x > kScreenW + margin)  p.x = -margin;
    if (p.y < -margin)            p.y = kScreenH + margin;
    if (p.y > kScreenH + margin)  p.y = -margin;
}

Vector2 WrapToScreen(Vector2 p)
{
    p.x = std::fmod(p.x, static_cast<float>(kScreenW));
    if (p.x < 0.0f) p.x += static_cast<float>(kScreenW);

    p.y = std::fmod(p.y, static_cast<float>(kScreenH));
    if (p.y < 0.0f) p.y += static_cast<float>(kScreenH);

    return p;
}

Vector2 WrappedDelta(Vector2 a, Vector2 b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;

    if (dx >  kScreenW * 0.5f) dx -= kScreenW;
    if (dx < -kScreenW * 0.5f) dx += kScreenW;
    if (dy >  kScreenH * 0.5f) dy -= kScreenH;
    if (dy < -kScreenH * 0.5f) dy += kScreenH;

    return {dx, dy};
}

float AsteroidRadiusForTier(int tier)
{
    switch (tier)
    {
        case 3:  return 58.0f;
        case 2:  return 34.0f;
        default: return 18.0f;
    }
}

Asteroid MakeAsteroid(Vector2 pos, int tier, float speedScale)
{
    Asteroid a;
    a.pos    = pos;
    a.tier   = tier;
    a.radius = AsteroidRadiusForTier(tier);

    // Smaller fragments travel faster — keeps late-wave pressure up.
    const float speed = ((tier == 3) ? RandF( 40.0f,  90.0f)
                       : (tier == 2) ? RandF( 70.0f, 145.0f)
                                     : RandF(110.0f, 200.0f)) * speedScale;
    const float dir = RandF(0.0f, 2.0f * PI);
    a.vel = {std::cos(dir) * speed, std::sin(dir) * speed};

    a.rot      = RandF(0.0f, 2.0f * PI);
    a.rotSpeed = RandF(-1.6f, 1.6f);

    const int verts = RandI(9, 13);
    a.shape.reserve(verts);
    for (int i = 0; i < verts; i++)
        a.shape.push_back(RandF(0.68f, 1.28f));

    return a;
}

void DrawNeonLine(Vector2 a, Vector2 b, float thickness, Color c)
{
    DrawLineEx(a, b, thickness * 2.6f, Fade(c, 0.18f));
    DrawLineEx(a, b, thickness,        c);
    DrawLineEx(a, b, thickness * 0.4f, Brighten(c, 0.75f));
}

void DrawNeonPolyline(const std::vector<Vector2>& pts, float thickness, Color c, bool closed)
{
    if (pts.size() < 2) return;

    for (size_t i = 0; i + 1 < pts.size(); i++)
        DrawNeonLine(pts[i], pts[i + 1], thickness, c);

    if (closed)
        DrawNeonLine(pts.back(), pts.front(), thickness, c);
}

void DrawNeonDot(Vector2 p, float radius, Color c)
{
    DrawCircleV(p, radius * 2.4f, Fade(c, 0.16f));
    DrawCircleV(p, radius,        c);
    DrawCircleV(p, radius * 0.45f, Brighten(c, 0.8f));
}

void DrawShip(const Ship& ship, float time)
{
    if (!ship.alive) return;

    const Color tint = ShipColor(ship.player);

    // Hyperspace entry: a ring closes in while the ship spins and shrinks into
    // it, so the jump reads as the ship being drawn into the circle rather than
    // simply blinking out. Past the collapse it is genuinely gone.
    float scale = 1.0f;
    float spin  = 0.0f;

    if (ship.warping)
    {
        const float q = (ship.warpDuration > 0.0f)
                      ? 1.0f - Clamp(ship.warp / ship.warpDuration, 0.0f, 1.0f)
                      : 1.0f;

        // The portal opens quickly, holds while the ship flies in, then shuts
        // behind it.
        const float open  = Clamp(q / 0.20f, 0.0f, 1.0f);
        const float close = 1.0f - Clamp((q - 0.78f) / 0.22f, 0.0f, 1.0f);
        const float r     = 44.0f * open * close;

        if (r > 0.5f) DrawPortalRings(ship.warpPortal, r, tint);

        // Ship travel is handled by the simulation; this only sizes it. Once
        // it reaches the portal it has gone through, so stop drawing it.
        const float t = Clamp(q / kWarpTravelFraction, 0.0f, 1.0f);
        if (t >= 1.0f) return;

        // Holds full size for most of the approach, then shrinks sharply as it
        // crosses the threshold — it reads as passing through rather than
        // fading out. Heading is left alone: the ship flies in straight.
        scale = 1.0f - SmoothStep01((t - 0.55f) / 0.45f);
    }
    else if (ship.invuln > 0.0f && std::fmod(time, 0.16f) < 0.08f)
    {
        // Blink while the shield is up so the player reads it as temporary.
        return;
    }

    std::vector<Vector2> hull;
    hull.reserve(kHull.size());
    for (const Vector2& v : kHull)
        hull.push_back(Vector2Add(ship.pos, Vector2Rotate(Vector2Scale(v, scale), ship.rot + spin)));

    DrawNeonPolyline(hull, 2.2f, tint, true);

    if (ship.thrusting && !ship.warping)
    {
        // Flicker the flame length per frame — a static cone looks dead.
        const float len = RandF(12.0f, 24.0f);
        const std::vector<Vector2> flame = {
            Vector2Add(ship.pos, Vector2Rotate({ -9.0f, -6.0f}, ship.rot)),
            Vector2Add(ship.pos, Vector2Rotate({-9.0f - len, 0.0f}, ship.rot)),
            Vector2Add(ship.pos, Vector2Rotate({ -9.0f,  6.0f}, ship.rot)),
        };
        DrawNeonPolyline(flame, 2.0f, kThrustHot, false);
    }

    // Spawn/hyperspace immunity ring, in the ship's own colour.
    if (ship.invuln > 0.0f && !ship.warping)
    {
        const float pulse = 0.35f + 0.15f * std::sin(time * 9.0f);
        DrawCircleLinesV(ship.pos, 22.0f, Fade(tint, pulse));
    }

    // Shield ring, in the shield's ice-blue and a touch larger, so it reads as
    // a distinct state from spawn immunity. A double ring makes it unmistakable.
    if (ship.shield && !ship.warping)
    {
        const Color shieldCol = PowerupColor(PowerupType::Shield);
        const float pulse     = 0.55f + 0.25f * std::sin(time * 6.0f);
        DrawCircleLinesV(ship.pos, 25.0f, Fade(shieldCol, pulse));
        DrawCircleLinesV(ship.pos, 27.0f, Fade(shieldCol, pulse * 0.5f));
    }
}

void DrawUfo(const Ufo& u, float time)
{
    const float r = u.radius;

    // Flattened hull plus a dome — the classic saucer silhouette, which reads
    // instantly as "not a rock" even at a glance.
    const std::vector<Vector2> hull = {
        {u.pos.x - r,         u.pos.y},
        {u.pos.x - r * 0.45f, u.pos.y - r * 0.32f},
        {u.pos.x + r * 0.45f, u.pos.y - r * 0.32f},
        {u.pos.x + r,         u.pos.y},
        {u.pos.x + r * 0.45f, u.pos.y + r * 0.34f},
        {u.pos.x - r * 0.45f, u.pos.y + r * 0.34f},
    };
    DrawNeonPolyline(hull, 2.2f, kUfoGreen, true);

    const std::vector<Vector2> dome = {
        {u.pos.x - r * 0.45f, u.pos.y - r * 0.32f},
        {u.pos.x - r * 0.24f, u.pos.y - r * 0.64f},
        {u.pos.x + r * 0.24f, u.pos.y - r * 0.64f},
        {u.pos.x + r * 0.45f, u.pos.y - r * 0.32f},
    };
    DrawNeonPolyline(dome, 2.0f, kUfoGreen, false);

    // Waist band, pulsing — gives it a sense of being powered and alive.
    const float pulse = 0.45f + 0.35f * std::sin(time * 7.0f);
    DrawNeonLine({u.pos.x - r * 0.9f, u.pos.y}, {u.pos.x + r * 0.9f, u.pos.y},
                 1.6f, Fade(kUfoGreen, pulse));
}

void DrawAsteroid(const Asteroid& a)
{
    const int n = static_cast<int>(a.shape.size());
    if (n < 3) return;

    std::vector<Vector2> pts;
    pts.reserve(n);
    for (int i = 0; i < n; i++)
    {
        const float ang = a.rot + (2.0f * PI * static_cast<float>(i)) / static_cast<float>(n);
        const float r   = a.radius * a.shape[i];
        pts.push_back({a.pos.x + std::cos(ang) * r, a.pos.y + std::sin(ang) * r});
    }

    DrawNeonPolyline(pts, (a.tier == 3) ? 2.4f : 2.0f, AsteroidColor(a.tier), true);
}

void DrawPowerup(const Powerup& p, float time)
{
    const Color tint = PowerupColor(p.type);
    const float r    = p.radius;

    // Blink out over the final second so an expiring pickup is not grabbed by
    // surprise.
    if (p.life < 1.0f && std::fmod(time, 0.18f) < 0.09f) return;

    // A slowly spinning hexagon shell marks it as a pickup regardless of icon.
    std::vector<Vector2> shell;
    shell.reserve(6);
    for (int i = 0; i < 6; i++)
    {
        const float a = p.spin + (2.0f * PI * static_cast<float>(i)) / 6.0f;
        shell.push_back({p.pos.x + std::cos(a) * r, p.pos.y + std::sin(a) * r});
    }
    DrawNeonPolyline(shell, 2.0f, tint, true);

    // Icon inside, one per type.
    switch (p.type)
    {
        case PowerupType::ExtraLife:
        {
            // A little ship glyph.
            const std::vector<Vector2> glyph = {
                {p.pos.x,        p.pos.y - r * 0.5f},
                {p.pos.x - r * 0.42f, p.pos.y + r * 0.45f},
                {p.pos.x,        p.pos.y + r * 0.15f},
                {p.pos.x + r * 0.42f, p.pos.y + r * 0.45f},
            };
            DrawNeonPolyline(glyph, 1.8f, tint, true);
            break;
        }
        case PowerupType::FanFire:
        {
            // Three diverging strokes — the spread pattern it grants.
            for (int i = -1; i <= 1; i++)
            {
                const float a  = -PI / 2.0f + static_cast<float>(i) * 0.42f;
                const Vector2 tip = {p.pos.x + std::cos(a) * r * 0.55f,
                                     p.pos.y + std::sin(a) * r * 0.55f};
                DrawNeonLine({p.pos.x, p.pos.y + r * 0.35f}, tip, 1.8f, tint);
            }
            break;
        }
        case PowerupType::Shield:
        {
            DrawCircleLinesV(p.pos, r * 0.5f, tint);
            DrawCircleLinesV(p.pos, r * 0.32f, Fade(tint, 0.6f));
            break;
        }
        default:
            break;
    }
}

void DrawBullet(const Bullet& b)
{
    const Vector2 tail = Vector2Subtract(b.pos, Vector2Scale(b.vel, 0.012f));
    const Color   tint = IsEnemyBullet(b) ? kUfoGreen : BulletColor(b.owner);
    DrawNeonLine(tail, b.pos, 2.4f, tint);
}

void DrawParticle(const Particle& p)
{
    const float t = (p.maxLife > 0.0f) ? (p.life / p.maxLife) : 0.0f;
    DrawCircleV(p.pos, p.size * t, Fade(p.color, t));
}
