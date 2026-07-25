#include "game.hpp"

#include <raymath.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
// --- ship feel ---
constexpr float kTurnSpeed    = 4.2f;    // rad/s
constexpr float kThrustAccel  = 430.0f;  // px/s^2

// No drag term: this is a vacuum, so velocity persists until thrust changes
// it. Slowing down means turning around and burning retrograde.
//
// The speed cap below is the one concession to playability rather than
// physics — without it an unbounded ship eventually crosses more than its own
// radius per frame and tunnels straight through asteroids without ever
// registering a collision.
constexpr float kMaxSpeed     = 520.0f;
constexpr float kShipRadius   = 11.0f;
constexpr float kSpawnInvuln  = 2.5f;

// --- weapon ---
constexpr float kBulletSpeed  = 720.0f;
constexpr float kBulletLife   = 0.85f;
constexpr float kFireCooldown = 0.16f;

// Recoil per shot. Disabled by request — firing no longer nudges the ship.
// Physically a shot should impart (bulletMass / shipMass) x bulletSpeed, but
// with no drag even a tiny per-shot kick accumulates under sustained fire, and
// the drift was more annoying than the realism was worth. Set non-zero to
// restore it; the Fire() code already scales it by the number of barrels.
constexpr float kRecoil = 0.0f;

// --- hyperspace ---
// The ship is immune for the full window but only hidden for the first slice
// of it, so it re-emerges with time left on the shield.
constexpr float kHyperspaceInvuln = 2.0f;
constexpr float kHyperspaceWarp   = 0.55f;

// --- misc ---
constexpr float kRespawnDelay  = 1.4f;
constexpr int   kStarCount     = 220;
constexpr float kStickDeadzone = 0.25f;

// Keyboard bindings per player, split into two clusters that share a laptop
// keyboard: player 1 on WASD with the left-hand modifiers, player 2 on the
// arrows with the two keys just above them. Nothing here needs a numpad, and
// nothing uses right-control, which Mac laptops do not have.
//
// The `*Alt` fields are player 1's arrow-key convenience. They are only live
// in single player, since two-player hands the arrows to player 2.
// KEY_NULL means unbound.
struct KeyBinding
{
    int left, leftAlt;
    int right, rightAlt;
    int thrust, thrustAlt;
    int fire;
    int hyperspace;
};

constexpr KeyBinding kKeys[kMaxPlayers] = {
    {KEY_A,    KEY_LEFT, KEY_D,     KEY_RIGHT, KEY_W,  KEY_UP,   KEY_SPACE,       KEY_LEFT_SHIFT},
    {KEY_LEFT, KEY_NULL, KEY_RIGHT, KEY_NULL,  KEY_UP, KEY_NULL, KEY_RIGHT_SHIFT, KEY_SLASH},
};

bool KeyHeld(int key)
{
    return (key != KEY_NULL) && IsKeyDown(key);
}

bool KeyHit(int key)
{
    return (key != KEY_NULL) && IsKeyPressed(key);
}

float ApplyDeadzone(float v)
{
    return (std::fabs(v) < kStickDeadzone) ? 0.0f : v;
}

// --- difficulty curve ---------------------------------------------------
//
// Three axes scale, deliberately at different rates. Count alone would just
// make the screen dense; speed is what actually makes waves harder, and the
// pre-split seeds put fast small rocks on the board immediately rather than
// two tiers of chewing later.

// Large asteroids: one more per wave to 11, then one per three waves to 16.
// The slower tail is what stops the screen turning to soup.
int LargeCountForWave(int wave)
{
    if (wave <= 7) return 4 + wave;                      // 5 .. 11
    return std::min(11 + (wave - 7) / 3, 16);
}

// Medium rocks seeded on top of the large ones, from wave 5. These are added,
// not substituted — swapping a large (7 fragments) for a medium (3) would make
// later waves shorter than earlier ones.
int MediumSeedsForWave(int wave)
{
    if (wave < 5) return 0;
    return std::min(1 + (wave - 5) / 2, 6);
}

// Everything moves faster as waves go on, capped so it stays readable.
float SpeedScaleForWave(int wave)
{
    return std::min(1.0f + 0.06f * static_cast<float>(wave - 1), 2.0f);
}

// --- saucers ------------------------------------------------------------

constexpr int   kUfoFirstWave    = 3;      // none before this
constexpr float kUfoBulletSpeed  = 380.0f;
constexpr float kUfoBulletLife   = 1.9f;
constexpr int   kUfoMaxAlive     = 2;

// Seconds between saucer arrivals, tightening with the wave but never so
// short that they stack up faster than they can be shot down.
float UfoIntervalForWave(int wave)
{
    return std::max(20.0f - 1.1f * static_cast<float>(wave), 7.0f);
}

// Small saucers are faster and shoot straight at you. They start appearing
// around wave 6 and gradually crowd out the large ones.
bool RollSmallUfo(int wave)
{
    const float chance = std::clamp(0.10f * static_cast<float>(wave - 5), 0.0f, 0.75f);
    return RandF(0.0f, 1.0f) < chance;
}

// Aim error in radians. Large saucers effectively spray; small ones tighten up
// as waves progress but never become perfect.
float UfoAimErrorForWave(int tier, int wave)
{
    if (tier == 2) return RandF(-0.9f, 0.9f);
    const float spread = std::max(0.34f - 0.02f * static_cast<float>(wave), 0.06f);
    return RandF(-spread, spread);
}
}  // namespace

void Game::Init()
{
    // Before ResetRun, which starts a wave and expects to be able to play.
    audio_.Load();

    stars_.clear();
    stars_.reserve(kStarCount);
    for (int i = 0; i < kStarCount; i++)
    {
        Star s;
        s.pos   = {RandF(0.0f, kScreenW), RandF(0.0f, kScreenH)};
        s.depth = RandF(0.15f, 1.0f);
        s.size  = 0.6f + s.depth * 1.4f;
        stars_.push_back(s);
    }

    EnterMenu();
}

void Game::Shutdown()
{
    audio_.Unload();
}

void Game::EnterMenu()
{
    state_ = State::Menu;
    shake_ = 0.0f;

    ships_.clear();
    bullets_.clear();
    particles_.clear();
    ufos_.clear();
    powerups_.clear();

    // A few rocks drifting behind the menu, so it is not a static screen.
    asteroids_.clear();
    for (int i = 0; i < 5; i++)
        asteroids_.push_back(MakeAsteroid({RandF(0.0f, kScreenW), RandF(0.0f, kScreenH)}, 3));
}

void Game::ResetRun(int playerCount)
{
    shake_ = 0.0f;
    state_ = State::Playing;
    wave_  = 1;

    playerCount_ = std::clamp(playerCount, 1, kMaxPlayers);

    bullets_.clear();
    particles_.clear();
    ufos_.clear();
    powerups_.clear();

    // Only counts down from kUfoFirstWave, so this is time-into-that-wave
    // rather than time-from-start. Short enough that the first saucer actually
    // arrives instead of the wave ending first.
    ufoSpawnTimer_ = 8.0f;

    ships_.clear();
    ships_.reserve(static_cast<size_t>(playerCount_));
    for (int i = 0; i < playerCount_; i++)
    {
        Ship s;
        s.player = i;
        s.lives  = 3;
        SpawnShip(s);
        ships_.push_back(s);
    }

    StartWave(wave_);
}

void Game::SpawnShip(Ship& ship)
{
    // Preserve lives and player index; reset everything transient.
    const int lives  = ship.lives;
    const int player = ship.player;

    ship = Ship{};
    ship.lives  = lives;
    ship.player = player;

    // Spread players out horizontally rather than stacking them. A lone player
    // starts dead centre.
    const float slot = (playerCount_ <= 1)
                     ? 0.5f
                     : (static_cast<float>(player) + 1.0f) / (static_cast<float>(playerCount_) + 1.0f);

    ship.pos    = {kScreenW * slot, kScreenH * 0.5f};
    ship.rot    = -PI / 2.0f;
    ship.invuln = kSpawnInvuln;
    ship.alive  = true;
}

void Game::StartWave(int wave)
{
    asteroids_.clear();

    const float speedScale = SpeedScaleForWave(wave);

    // Keep the spawn area clear so a new wave cannot materialise on top of
    // any player.
    auto safeSpawn = [this]() {
        Vector2 pos;
        bool    tooClose;
        do
        {
            pos      = {RandF(0.0f, kScreenW), RandF(0.0f, kScreenH)};
            tooClose = false;
            for (const Ship& s : ships_)
            {
                if (Vector2Length(WrappedDelta(pos, s.pos)) < 190.0f)
                {
                    tooClose = true;
                    break;
                }
            }
        } while (tooClose);
        return pos;
    };

    const int large  = LargeCountForWave(wave);
    const int medium = MediumSeedsForWave(wave);

    for (int i = 0; i < large; i++)
        asteroids_.push_back(MakeAsteroid(safeSpawn(), 3, speedScale));

    for (int i = 0; i < medium; i++)
        asteroids_.push_back(MakeAsteroid(safeSpawn(), 2, speedScale));

    audio_.PlayWaveStart();
}

void Game::UpdateMenu()
{
    int nav = 0;

    if (IsKeyPressed(KEY_UP)   || IsKeyPressed(KEY_W)) nav = -1;
    if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) nav =  1;

    bool confirm = IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE);

    if (IsGamepadAvailable(0))
    {
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_UP))   nav = -1;
        if (IsGamepadButtonPressed(0, GAMEPAD_BUTTON_LEFT_FACE_DOWN)) nav =  1;

        // Stick nav needs an edge, or holding it would race through the list.
        const float y = GetGamepadAxisMovement(0, GAMEPAD_AXIS_LEFT_Y);
        if (std::fabs(y) > 0.5f)
        {
            if (!menuAxisHeld_)
            {
                nav = (y < 0.0f) ? -1 : 1;
                menuAxisHeld_ = true;
            }
        }
        else
        {
            menuAxisHeld_ = false;
        }

        confirm = confirm
               || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)
               || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT);
    }

    if (nav != 0)
    {
        menuSelection_ = (menuSelection_ + nav + kMaxPlayers) % kMaxPlayers;
        audio_.PlayWaveStart();
    }

    if (confirm)
    {
        playerCount_ = menuSelection_ + 1;
        playerPad_[0] = -1;
        playerPad_[1] = -1;

        if (playerCount_ == 1)
        {
            // One player: bind the first controller present, else keyboard.
            for (int s = 0; s < 4; s++)
            {
                if (IsGamepadAvailable(s)) { playerPad_[0] = s; break; }
            }
            ResetRun(playerCount_);
        }
        else
        {
            // Two players: each claims their own controller.
            claimIndex_ = 0;
            state_      = State::Claiming;
        }
    }
}

void Game::UpdateClaim()
{
    // Slots already taken by earlier players cannot be claimed again.
    auto slotTaken = [this](int slot) {
        for (int p = 0; p < claimIndex_; p++)
            if (playerPad_[p] == slot) return true;
        return false;
    };

    // First fresh button press on any unclaimed pad wins this player's slot.
    int pressedSlot = -1;
    for (int slot = 0; slot < 4 && pressedSlot < 0; slot++)
    {
        if (!IsGamepadAvailable(slot) || slotTaken(slot)) continue;
        for (int b = 1; b <= 17; b++)
        {
            if (IsGamepadButtonPressed(slot, b)) { pressedSlot = slot; break; }
        }
    }

    // Keyboard fallback: a player can claim the keyboard with their own fire
    // key instead of a controller. -1 marks keyboard control.
    const bool kbClaim = KeyHit(kKeys[claimIndex_].fire);

    if (pressedSlot >= 0)
    {
        playerPad_[claimIndex_] = pressedSlot;
        audio_.PlayPowerup();
        claimIndex_++;
    }
    else if (kbClaim)
    {
        playerPad_[claimIndex_] = -1;
        audio_.PlayPowerup();
        claimIndex_++;
    }

    // Back out to the menu.
    if (IsKeyPressed(KEY_BACKSPACE)) { EnterMenu(); return; }

    if (claimIndex_ >= playerCount_) ResetRun(playerCount_);
}

ShipControls Game::ReadControls(int player) const
{
    ShipControls in;

    if (player < 0 || player >= kMaxPlayers) return in;

    const KeyBinding& k = kKeys[player];

    // Player 1's arrow-key alternates are surrendered to player 2 in a
    // two-player game, so the two never fight over the same keys.
    const bool allowAlt = (playerCount_ <= 1);

    // --- keyboard ---
    if (KeyHeld(k.left)  || (allowAlt && KeyHeld(k.leftAlt)))  in.turn -= 1.0f;
    if (KeyHeld(k.right) || (allowAlt && KeyHeld(k.rightAlt))) in.turn += 1.0f;

    in.thrust     = KeyHeld(k.thrust) || (allowAlt && KeyHeld(k.thrustAlt));
    in.fire       = KeyHeld(k.fire);
    in.hyperspace = KeyHit(k.hyperspace);

    // --- gamepad: the slot this player claimed (-1 = keyboard only) ---
    const int pad = playerPad_[player];
    if (pad >= 0 && IsGamepadAvailable(pad))
    {
        if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_LEFT))  in.turn -= 1.0f;
        if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_LEFT_FACE_RIGHT)) in.turn += 1.0f;

        const float stick = ApplyDeadzone(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_X));
        if (stick != 0.0f) in.turn = stick;

        // The 8BitDo 64 is an N64-style pad: its C-cluster is wired to the
        // right stick, so C-Up reads as the right-stick Y axis going negative.
        // The user asked for thrust there, which also frees the analog stick to
        // be pure steering. Other pads keep thrust on the left stick.
        const char* padName = GetGamepadName(pad);
        const bool  is64    = (padName != nullptr) && (std::strstr(padName, "64") != nullptr);

        if (is64)
        {
            // Right-stick Y rests at 0 and snaps to -1 for C-Up.
            if (GetGamepadAxisMovement(pad, GAMEPAD_AXIS_RIGHT_Y) < -0.5f) in.thrust = true;
        }
        else
        {
            // Thrust on the left stick: turning is the same stick's X axis, so
            // one stick drives the ship entirely. Up is negative Y.
            const float stickY = ApplyDeadzone(GetGamepadAxisMovement(pad, GAMEPAD_AXIS_LEFT_Y));
            if (stickY < 0.0f) in.thrust = true;
        }

        // Fire on the A button (bottom face), right bumper as an alternative.
        if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_FACE_DOWN)) in.fire = true;
        if (IsGamepadButtonDown(pad, GAMEPAD_BUTTON_RIGHT_TRIGGER_1)) in.fire = true;

        // Hyperspace on B for the 64 (its C-cluster occupies the X/Y slots, so
        // B is the natural second button); X on other pads. Left bumper works
        // on both as an alternative.
        const int hyperBtn = is64 ? GAMEPAD_BUTTON_RIGHT_FACE_RIGHT
                                  : GAMEPAD_BUTTON_RIGHT_FACE_LEFT;
        if (IsGamepadButtonPressed(pad, hyperBtn))                     in.hyperspace = true;
        if (IsGamepadButtonPressed(pad, GAMEPAD_BUTTON_LEFT_TRIGGER_1)) in.hyperspace = true;
    }

    in.turn = Clamp(in.turn, -1.0f, 1.0f);
    return in;
}

void Game::Update(float dt)
{
    time_ += dt;
    shake_ = std::max(0.0f, shake_ - dt * 2.6f);

    UpdateStars(dt);
    UpdateParticles(dt);

    if (state_ == State::Menu)
    {
        UpdateMenu();
        UpdateAsteroids(dt);
        audio_.SetThrust(false);
        return;
    }

    if (state_ == State::Claiming)
    {
        UpdateClaim();
        UpdateAsteroids(dt);
        audio_.SetThrust(false);
        return;
    }

    if (state_ == State::GameOver)
    {
        const bool restart = IsKeyPressed(KEY_R)
                          || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_MIDDLE_RIGHT);
        const bool toMenu  = IsKeyPressed(KEY_M)
                          || IsGamepadButtonPressed(0, GAMEPAD_BUTTON_RIGHT_FACE_RIGHT);

        if (restart)     ResetRun(playerCount_);
        else if (toMenu) EnterMenu();

        UpdateAsteroids(dt);
        audio_.SetThrust(false);
        return;
    }

    bool anyThrusting = false;

    for (Ship& ship : ships_)
    {
        if (ship.alive)
        {
            const ShipControls in = ReadControls(ship.player);
            UpdateShip(ship, in, dt);
            anyThrusting = anyThrusting || (ship.thrusting && !ship.warping);
        }
        else if (ship.lives > 0)
        {
            ship.respawnTimer -= dt;
            if (ship.respawnTimer <= 0.0f) SpawnShip(ship);
        }
    }

    audio_.SetThrust(anyThrusting);

    UpdateBullets(dt);
    UpdateUfos(dt);
    UpdatePowerups(dt);
    UpdateAsteroids(dt);
    ResolveCollisions();

    // Run ends only when every player is out of lives and off the board.
    const bool allDone = std::all_of(ships_.begin(), ships_.end(), [](const Ship& s) {
        return !s.alive && s.lives <= 0;
    });
    if (allDone) state_ = State::GameOver;

    // Clearing a wave advances it. Guarded by Playing so a wave cleared on the
    // same frame everyone dies does not also start the next one.
    if (asteroids_.empty() && state_ == State::Playing)
    {
        wave_++;

        // Co-op revive: a player who ran out of lives rejoins on the next wave,
        // since reaching here means a teammate survived to clear this one. In
        // single player this never fires — a lone player out of lives is the
        // allDone case above, so the state is already GameOver.
        for (Ship& s : ships_)
        {
            if (!s.alive && s.lives <= 0)
            {
                s.lives = 1;
                SpawnShip(s);   // preserves lives, resets everything transient
            }
        }

        StartWave(wave_);
    }
}

void Game::UpdateShip(Ship& ship, const ShipControls& in, float dt)
{
    if (ship.invuln > 0.0f)       ship.invuln -= dt;
    if (ship.fireCooldown > 0.0f) ship.fireCooldown -= dt;

    // --- hyperspace: hidden, inert, then re-emerges elsewhere ---
    if (ship.warping)
    {
        ship.warp -= dt;

        // Fly into the portal over the first stretch of the warp, then hold at
        // it, hidden, until the jump completes.
        const float q = (ship.warpDuration > 0.0f)
                      ? 1.0f - std::clamp(ship.warp / ship.warpDuration, 0.0f, 1.0f)
                      : 1.0f;
        // Step along the shortest wrapped path rather than lerping raw
        // coordinates, so a portal just over an edge is reached by crossing
        // the seam instead of flying back across the screen.
        const float   travel = SmoothStep01(q / kWarpTravelFraction);
        const Vector2 delta  = WrappedDelta(ship.warpPortal, ship.warpFrom);
        ship.pos = WrapToScreen(Vector2Add(ship.warpFrom, Vector2Scale(delta, travel)));

        if (ship.warp <= 0.0f) EmergeFromHyperspace(ship);

        // No steering, thrust or firing while out of phase.
        ship.thrusting = false;
        return;
    }

    if (in.hyperspace)
    {
        EnterHyperspace(ship);
        return;
    }

    ship.rot += in.turn * kTurnSpeed * dt;
    ship.thrusting = in.thrust;

    if (ship.thrusting)
    {
        const Vector2 dir = {std::cos(ship.rot), std::sin(ship.rot)};
        ship.vel = Vector2Add(ship.vel, Vector2Scale(dir, kThrustAccel * dt));
        SpawnThrustParticles(ship, dt);
    }

    const float speed = Vector2Length(ship.vel);
    if (speed > kMaxSpeed) ship.vel = Vector2Scale(ship.vel, kMaxSpeed / speed);

    ship.pos = Vector2Add(ship.pos, Vector2Scale(ship.vel, dt));
    WrapPosition(ship.pos, 20.0f);

    if (in.fire && ship.fireCooldown <= 0.0f) Fire(ship);
}

void Game::EnterHyperspace(Ship& ship)
{
    ship.warping      = true;
    ship.warp         = kHyperspaceWarp;
    ship.warpDuration = kHyperspaceWarp;
    ship.invuln       = kHyperspaceInvuln;   // outlasts the disappearance
    ship.thrusting    = false;
    // Velocity is deliberately untouched — momentum carries through the jump.

    // The portal opens ahead of the ship along its direction of travel, so the
    // jump looks like flying into it. With no meaningful momentum there is no
    // travel direction to use, so it falls back to where the nose points.
    const float   speed = Vector2Length(ship.vel);
    const Vector2 dir   = (speed > 20.0f)
                        ? Vector2Scale(ship.vel, 1.0f / speed)
                        : Vector2{std::cos(ship.rot), std::sin(ship.rot)};

    // Faster ships get a portal further out, so the approach takes about the
    // same time regardless of speed.
    const float dist = std::clamp(70.0f + speed * 0.18f, 70.0f, 170.0f);

    // Wrapped, not clamped: the portal can open across a screen edge and the
    // ship flies through the seam to reach it, exactly as normal movement
    // does. The approach below travels the short way around, so this never
    // becomes a dash across the whole playfield.
    const Vector2 portal = WrapToScreen(Vector2Add(ship.pos, Vector2Scale(dir, dist)));

    ship.warpFrom   = ship.pos;
    ship.warpPortal = portal;

    // Ring converges on the portal, not the ship — that is where it is going.
    SpawnRing(portal, ShipColor(ship.player), 26, 170.0f, /*inward=*/true);
    shake_ = std::min(1.0f, shake_ + 0.18f);
    audio_.PlayHyperspace(false);
    Rumble(0.25f, 0.4f, 0.12f);
}

void Game::EmergeFromHyperspace(Ship& ship)
{
    ship.warping = false;
    ship.warp    = 0.0f;

    // Anywhere on screen, with a margin so it never lands half off the edge.
    ship.pos = {RandF(60.0f, kScreenW - 60.0f), RandF(60.0f, kScreenH - 60.0f)};

    SpawnRing(ship.pos, ShipColor(ship.player), 30, 300.0f, /*inward=*/false);
    audio_.PlayHyperspace(true);
}

void Game::Fire(Ship& ship)
{
    ship.fireCooldown = kFireCooldown;

    // Fan fire lays down three shots in a spread; the plain weapon fires one.
    // The angles are symmetric about the nose so the centre shot is unchanged.
    constexpr float kFanSpread = 0.16f;   // radians between fan shots
    const float angles1[1] = {0.0f};
    const float angles3[3] = {-kFanSpread, 0.0f, kFanSpread};

    const float* angles = ship.fanFire ? angles3 : angles1;
    const int    count  = ship.fanFire ? 3 : 1;

    const Vector2 nose = {std::cos(ship.rot), std::sin(ship.rot)};

    for (int i = 0; i < count; i++)
    {
        const float   a   = ship.rot + angles[i];
        const Vector2 dir = {std::cos(a), std::sin(a)};

        Bullet b;
        b.pos   = Vector2Add(ship.pos, Vector2Scale(dir, 18.0f));
        // Inherit ship velocity so shots feel attached to the ship.
        b.vel   = Vector2Add(Vector2Scale(dir, kBulletSpeed), ship.vel);
        b.life  = kBulletLife;
        b.owner = ship.player;
        bullets_.push_back(b);

        SpawnBurst(b.pos, BulletColor(ship.player), 4, 90.0f);
    }

    // Recoil is along the nose regardless of spread, and a fan kicks harder.
    ship.vel = Vector2Subtract(ship.vel, Vector2Scale(nose, kRecoil * static_cast<float>(count)));
    shake_ = std::min(1.0f, shake_ + 0.06f);
    Rumble(0.0f, 0.18f, 0.05f);
    audio_.PlayShoot();
}

const Ship* Game::NearestShip(Vector2 from) const
{
    const Ship* best     = nullptr;
    float       bestDist = 0.0f;

    for (const Ship& s : ships_)
    {
        if (!ShipVisible(s)) continue;   // warping ships cannot be targeted

        const float d = Vector2Length(WrappedDelta(s.pos, from));
        if (best == nullptr || d < bestDist)
        {
            best     = &s;
            bestDist = d;
        }
    }

    return best;
}

void Game::SpawnUfo()
{
    Ufo u;
    u.tier   = RollSmallUfo(wave_) ? 1 : 2;
    u.radius = (u.tier == 1) ? 14.0f : 22.0f;

    const float speed = ((u.tier == 1) ? 150.0f : 95.0f) * SpeedScaleForWave(wave_);

    // Enters from one side and crosses; it does not wrap horizontally, so a
    // saucer you fail to kill eventually leaves rather than hounding you.
    const bool fromLeft = (RandI(0, 1) == 0);
    u.pos = {fromLeft ? -u.radius * 2.0f : kScreenW + u.radius * 2.0f,
             RandF(80.0f, kScreenH - 80.0f)};
    u.vel = {fromLeft ? speed : -speed, 0.0f};

    u.fireTimer = RandF(0.6f, 1.4f);
    u.jinkTimer = RandF(0.8f, 1.6f);

    ufos_.push_back(u);
    audio_.PlayUfoAppear();
}

void Game::UpdateUfos(float dt)
{
    // Arrival pacing. Saucers stay out of the opening waves so the basics can
    // be learned first.
    if (wave_ >= kUfoFirstWave && state_ == State::Playing)
    {
        ufoSpawnTimer_ -= dt;
        if (ufoSpawnTimer_ <= 0.0f)
        {
            if (static_cast<int>(ufos_.size()) < kUfoMaxAlive) SpawnUfo();
            ufoSpawnTimer_ = UfoIntervalForWave(wave_) * RandF(0.75f, 1.25f);
        }
    }

    for (Ufo& u : ufos_)
    {
        u.travelled += std::fabs(u.vel.x) * dt;

        // Periodic vertical course changes, so it is not a trivial lead shot.
        u.jinkTimer -= dt;
        if (u.jinkTimer <= 0.0f)
        {
            const float lateral = std::fabs(u.vel.x) * 0.55f;
            u.vel.y     = static_cast<float>(RandI(-1, 1)) * lateral;
            u.jinkTimer = RandF(0.7f, 1.7f);
        }

        u.pos = Vector2Add(u.pos, Vector2Scale(u.vel, dt));

        // Wraps vertically but not horizontally — crossing the screen is what
        // ends its visit.
        if (u.pos.y < -u.radius)            u.pos.y = kScreenH + u.radius;
        if (u.pos.y > kScreenH + u.radius)  u.pos.y = -u.radius;

        u.fireTimer -= dt;
        if (u.fireTimer <= 0.0f)
        {
            UfoFire(u);
            u.fireTimer = ((u.tier == 1) ? RandF(0.9f, 1.5f) : RandF(1.4f, 2.2f));
        }

        // Warble on a timer while it is on screen, so its presence is audible
        // even when it is off at the edge of attention.
        u.warbleTimer -= dt;
        if (u.warbleTimer <= 0.0f)
        {
            audio_.PlayUfoWarble(u.tier == 1);
            u.warbleTimer = 0.55f;
        }
    }

    // Gone once it has crossed the full width plus its entry margin.
    std::erase_if(ufos_, [](const Ufo& u) {
        return u.travelled > kScreenW + 120.0f;
    });
}

void Game::UfoFire(Ufo& u)
{
    const Ship* target = NearestShip(u.pos);

    float angle;
    if (u.tier == 1 && target != nullptr)
    {
        // Small saucers aim at the nearest ship, across the wrap seam if that
        // is genuinely the shorter line.
        const Vector2 to = WrappedDelta(target->pos, u.pos);
        angle = std::atan2(to.y, to.x);
    }
    else
    {
        angle = RandF(0.0f, 2.0f * PI);
    }

    angle += UfoAimErrorForWave(u.tier, wave_);

    Bullet b;
    b.pos   = u.pos;
    b.vel   = {std::cos(angle) * kUfoBulletSpeed, std::sin(angle) * kUfoBulletSpeed};
    b.life  = kUfoBulletLife;
    b.owner = kUfoBulletOwner;
    bullets_.push_back(b);

    SpawnBurst(u.pos, kUfoGreen, 3, 70.0f);
    audio_.PlayUfoShoot();
}

void Game::DestroyUfo(size_t index)
{
    const Ufo u = ufos_[index];

    SpawnBurst(u.pos, kUfoGreen, 30, 240.0f);
    SpawnBurst(u.pos, WHITE, 12, 140.0f);
    shake_ = std::min(1.0f, shake_ + 0.35f);
    Rumble(0.4f, 0.5f, 0.15f);
    audio_.PlayExplosion(2);

    // Every downed saucer leaves a pickup.
    SpawnPowerup(u.pos);

    ufos_.erase(ufos_.begin() + static_cast<long>(index));
}

void Game::SpawnPowerup(Vector2 at)
{
    Powerup p;
    p.pos    = at;
    // Drifts gently, so it neither sits perfectly still nor flees the player.
    p.vel    = {RandF(-40.0f, 40.0f), RandF(-40.0f, 40.0f)};
    p.type   = static_cast<PowerupType>(RandI(0, static_cast<int>(PowerupType::Count) - 1));
    p.life   = 20.0f;
    p.spin   = 0.0f;
    powerups_.push_back(p);
}

void Game::ApplyPowerup(Ship& ship, PowerupType type)
{
    switch (type)
    {
        case PowerupType::ExtraLife:
            ship.lives++;
            break;
        case PowerupType::FanFire:
            ship.fanFire = true;
            break;
        case PowerupType::Shield:
            ship.shield = true;
            break;
        default:
            break;
    }

    SpawnBurst(ship.pos, PowerupColor(type), 18, 160.0f);
    audio_.PlayPowerup();
}

void Game::UpdatePowerups(float dt)
{
    for (Powerup& p : powerups_)
    {
        p.pos = Vector2Add(p.pos, Vector2Scale(p.vel, dt));
        p.spin += dt * 1.4f;
        p.life -= dt;
        WrapPosition(p.pos, p.radius);
    }

    // Collect on ship contact.
    for (size_t pi = 0; pi < powerups_.size();)
    {
        bool taken = false;

        for (Ship& ship : ships_)
        {
            if (!ShipVisible(ship)) continue;
            if (Vector2Length(WrappedDelta(ship.pos, powerups_[pi].pos)) > powerups_[pi].radius + kShipRadius)
                continue;

            ApplyPowerup(ship, powerups_[pi].type);
            taken = true;
            break;
        }

        if (taken) powerups_.erase(powerups_.begin() + static_cast<long>(pi));
        else       pi++;
    }

    std::erase_if(powerups_, [](const Powerup& p) { return p.life <= 0.0f; });
}

void Game::UpdateBullets(float dt)
{
    for (Bullet& b : bullets_)
    {
        b.pos = Vector2Add(b.pos, Vector2Scale(b.vel, dt));
        b.life -= dt;
        WrapPosition(b.pos, 4.0f);
    }

    std::erase_if(bullets_, [](const Bullet& b) { return b.life <= 0.0f; });
}

void Game::UpdateAsteroids(float dt)
{
    for (Asteroid& a : asteroids_)
    {
        a.pos = Vector2Add(a.pos, Vector2Scale(a.vel, dt));
        a.rot += a.rotSpeed * dt;
        WrapPosition(a.pos, a.radius + 8.0f);
    }
}

void Game::UpdateParticles(float dt)
{
    for (Particle& p : particles_)
    {
        p.pos = Vector2Add(p.pos, Vector2Scale(p.vel, dt));
        p.vel = Vector2Scale(p.vel, std::exp(-1.8f * dt));
        p.life -= dt;
    }

    std::erase_if(particles_, [](const Particle& p) { return p.life <= 0.0f; });
}

void Game::UpdateStars(float dt)
{
    // Parallax follows the first live ship; with no ship it just drifts.
    Vector2 ref{0.0f, 0.0f};
    for (const Ship& s : ships_)
    {
        if (ShipVisible(s)) { ref = s.vel; break; }
    }

    for (Star& s : stars_)
    {
        const Vector2 drift = {
            -ref.x * s.depth * 0.08f - 5.0f * s.depth,
            -ref.y * s.depth * 0.08f,
        };
        s.pos = Vector2Add(s.pos, Vector2Scale(drift, dt));
        WrapPosition(s.pos, 2.0f);
    }
}

void Game::ResolveCollisions()
{
    // --- bullets ---
    for (size_t bi = 0; bi < bullets_.size();)
    {
        const Bullet& b        = bullets_[bi];
        bool          consumed = false;

        // Anything can break a rock, including saucer fire — a stray shot
        // clearing an asteroid for you is part of the chaos.
        for (size_t ai = 0; ai < asteroids_.size(); ai++)
        {
            if (Vector2Length(WrappedDelta(b.pos, asteroids_[ai].pos)) > asteroids_[ai].radius)
                continue;

            SplitAsteroid(ai);
            consumed = true;
            break;
        }

        // Player fire hits saucers. Saucers cannot shoot each other.
        if (!consumed && !IsEnemyBullet(b))
        {
            for (size_t ui = 0; ui < ufos_.size(); ui++)
            {
                if (Vector2Length(WrappedDelta(b.pos, ufos_[ui].pos)) > ufos_[ui].radius)
                    continue;

                DestroyUfo(ui);
                consumed = true;
                break;
            }
        }

        // Saucer fire hits ships.
        if (!consumed && IsEnemyBullet(b))
        {
            for (Ship& ship : ships_)
            {
                if (!ShipVisible(ship) || ship.invuln > 0.0f) continue;
                if (Vector2Length(WrappedDelta(b.pos, ship.pos)) > kShipRadius) continue;

                KillShip(ship);
                consumed = true;
                break;
            }
        }

        if (consumed) bullets_.erase(bullets_.begin() + static_cast<long>(bi));
        else          bi++;
    }

    // --- ships vs asteroids and saucers ---
    for (Ship& ship : ships_)
    {
        // Warping ships are out of phase; invulnerable ones shrug it off.
        if (!ShipVisible(ship) || ship.invuln > 0.0f) continue;

        bool killed = false;

        for (const Asteroid& a : asteroids_)
        {
            if (Vector2Length(WrappedDelta(ship.pos, a.pos)) < a.radius + kShipRadius)
            {
                KillShip(ship);
                killed = true;
                break;
            }
        }

        if (killed) continue;

        // Ramming a saucer destroys both.
        for (size_t ui = 0; ui < ufos_.size(); ui++)
        {
            if (Vector2Length(WrappedDelta(ship.pos, ufos_[ui].pos)) < ufos_[ui].radius + kShipRadius)
            {
                DestroyUfo(ui);
                KillShip(ship);
                break;
            }
        }
    }

    // Saucers deliberately ignore asteroids. Letting rocks kill them would
    // mean most saucers die in dense waves without the player involved.
}

void Game::SplitAsteroid(size_t index)
{
    const Asteroid hit = asteroids_[index];

    SpawnBurst(hit.pos, AsteroidColor(hit.tier), 14 + hit.tier * 6, 60.0f + hit.tier * 40.0f);
    shake_ = std::min(1.0f, shake_ + 0.10f * static_cast<float>(hit.tier));
    Rumble(0.12f * static_cast<float>(hit.tier), 0.25f, 0.09f);
    audio_.PlayExplosion(hit.tier);

    asteroids_.erase(asteroids_.begin() + static_cast<long>(index));

    if (hit.tier <= 1) return;

    for (int i = 0; i < 2; i++)
    {
        // Same wave scaling as the spawn, or fragments would be slower than
        // the rock they came from in later waves.
        Asteroid child = MakeAsteroid(hit.pos, hit.tier - 1, SpeedScaleForWave(wave_));
        // Push the halves apart so they visibly separate.
        child.vel = Vector2Add(child.vel, Vector2Scale(hit.vel, 0.45f));
        asteroids_.push_back(std::move(child));
    }
}

void Game::KillShip(Ship& ship)
{
    // Shield eats the hit instead of a life, then a short grace period so the
    // same asteroid that broke it does not immediately kill on the next frame.
    if (ship.shield)
    {
        ship.shield = false;
        ship.invuln = 1.0f;

        SpawnBurst(ship.pos, PowerupColor(PowerupType::Shield), 26, 220.0f);
        shake_ = std::min(1.0f, shake_ + 0.25f);
        Rumble(0.3f, 0.4f, 0.12f);
        audio_.PlayShieldBreak();
        return;
    }

    ship.alive     = false;
    ship.thrusting = false;
    ship.lives--;
    ship.respawnTimer = kRespawnDelay;

    SpawnBurst(ship.pos, ShipColor(ship.player), 40, 260.0f);
    SpawnBurst(ship.pos, WHITE, 18, 150.0f);
    shake_ = 1.0f;
    Rumble(1.0f, 1.0f, 0.45f);
    audio_.PlayShipDeath();
}

void Game::SpawnThrustParticles(const Ship& ship, float dt)
{
    // Rate-independent emission so the plume looks the same at any framerate.
    static float accumulator = 0.0f;
    accumulator += dt * 90.0f;

    const int count = static_cast<int>(accumulator);
    accumulator -= static_cast<float>(count);

    const Vector2 back   = {-std::cos(ship.rot), -std::sin(ship.rot)};
    const Vector2 origin = Vector2Add(ship.pos, Vector2Scale(back, 11.0f));

    for (int i = 0; i < count; i++)
    {
        Particle p;
        const float   spread = RandF(-0.45f, 0.45f);
        const Vector2 dir    = Vector2Rotate(back, spread);

        p.pos     = origin;
        p.vel     = Vector2Add(Vector2Scale(dir, RandF(90.0f, 210.0f)), ship.vel);
        p.maxLife = RandF(0.18f, 0.40f);
        p.life    = p.maxLife;
        p.size    = RandF(1.6f, 3.4f);
        p.color   = (RandF(0.0f, 1.0f) < 0.5f) ? kThrustHot : kThrustCool;

        particles_.push_back(p);
    }
}

void Game::SpawnBurst(Vector2 at, Color color, int count, float speed)
{
    for (int i = 0; i < count; i++)
    {
        Particle p;
        const float ang = RandF(0.0f, 2.0f * PI);
        const float mag = RandF(speed * 0.25f, speed);

        p.pos     = at;
        p.vel     = {std::cos(ang) * mag, std::sin(ang) * mag};
        p.maxLife = RandF(0.35f, 0.95f);
        p.life    = p.maxLife;
        p.size    = RandF(1.8f, 4.0f);
        p.color   = color;

        particles_.push_back(p);
    }
}

// Even ring rather than a random spray. Inward reads as collapsing into the
// jump, outward as arriving — which is what sells the teleport.
void Game::SpawnRing(Vector2 at, Color color, int count, float speed, bool inward)
{
    constexpr float kRadius = 46.0f;

    for (int i = 0; i < count; i++)
    {
        const float ang = (2.0f * PI * static_cast<float>(i)) / static_cast<float>(count);
        const Vector2 outward = {std::cos(ang), std::sin(ang)};

        Particle p;
        p.pos     = inward ? Vector2Add(at, Vector2Scale(outward, kRadius)) : at;
        p.vel     = Vector2Scale(outward, inward ? -speed : speed);
        p.maxLife = inward ? 0.36f : 0.45f;
        p.life    = p.maxLife;
        p.size    = 2.6f;
        p.color   = color;

        particles_.push_back(p);
    }
}

void Game::Rumble(float leftMotor, float rightMotor, float duration) const
{
#if defined(__APPLE__)
    // raylib's desktop GLFW backend has no vibration implementation — the call
    // is a stub that logs a warning every time. Firing a few shots a second
    // turns that into thousands of lines per session, so it is compiled out
    // here rather than called and ignored.
    (void)leftMotor;
    (void)rightMotor;
    (void)duration;
#else
    if (!IsGamepadAvailable(0)) return;
    SetGamepadVibration(0, leftMotor, rightMotor, duration);
#endif
}

Vector2 Game::ShakeOffset() const
{
    if (shake_ <= 0.0f) return {0.0f, 0.0f};

    // Squared falloff — big hits punch, small ones barely register.
    const float mag = shake_ * shake_ * 14.0f;
    return {RandF(-mag, mag), RandF(-mag, mag)};
}

void Game::Draw() const
{
    ClearBackground(kSpaceBg);

    // Starfield first — dim enough to stay clearly behind the action.
    for (const Star& s : stars_)
    {
        const float a = 0.25f + s.depth * 0.55f;
        DrawCircleV(s.pos, s.size, Fade(Color{170, 200, 255, 255}, a));
    }

    BeginBlendMode(BLEND_ADDITIVE);

    for (const Particle& p : particles_) DrawParticle(p);
    for (const Bullet& b : bullets_)     DrawBullet(b);
    for (const Asteroid& a : asteroids_) DrawAsteroid(a);
    for (const Ufo& u : ufos_)           DrawUfo(u, time_);
    for (const Powerup& p : powerups_)   DrawPowerup(p, time_);

    if (state_ == State::Playing)
        for (const Ship& s : ships_) DrawShip(s, time_);

    EndBlendMode();

    if (state_ == State::Menu)          DrawMenu();
    else if (state_ == State::Claiming) DrawClaim();
    else                                DrawHud();
}

void Game::DrawClaim() const
{
    const char* title = "ASSIGN CONTROLLERS";
    const int   tw    = MeasureText(title, 48);
    DrawText(title, (kScreenW - tw) / 2, 120, 48, ShipColor(0));

    // Keyboard fallback label per player, matching their fire key.
    const char* kbName[kMaxPlayers] = {"SPACE", "R-SHIFT"};

    char buf[128];
    for (int p = 0; p < playerCount_; p++)
    {
        const Color tint = ShipColor(p);
        const int   y    = 260 + p * 90;

        if (p < claimIndex_)
        {
            // Already claimed — show what it landed on.
            if (playerPad_[p] < 0)
                std::snprintf(buf, sizeof(buf), "PLAYER %d   >  KEYBOARD", p + 1);
            else
                std::snprintf(buf, sizeof(buf), "PLAYER %d   >  CONTROLLER %d", p + 1, playerPad_[p] + 1);

            const int w = MeasureText(buf, 30);
            DrawText(buf, (kScreenW - w) / 2, y, 30, tint);
        }
        else if (p == claimIndex_)
        {
            // Active prompt, pulsing to draw the eye.
            const float pulse = 0.55f + 0.45f * std::sin(time_ * 5.0f);
            std::snprintf(buf, sizeof(buf), "PLAYER %d  -  PRESS A BUTTON", p + 1);
            const int w = MeasureText(buf, 34);
            DrawText(buf, (kScreenW - w) / 2, y, 34, Fade(tint, pulse));

            std::snprintf(buf, sizeof(buf), "on the controller you want, or %s for keyboard", kbName[p]);
            const int w2 = MeasureText(buf, 18);
            DrawText(buf, (kScreenW - w2) / 2, y + 40, 18, Fade(tint, 0.5f));
        }
        else
        {
            // Waiting its turn.
            std::snprintf(buf, sizeof(buf), "PLAYER %d   -  waiting", p + 1);
            const int w = MeasureText(buf, 30);
            DrawText(buf, (kScreenW - w) / 2, y, 30, Fade(tint, 0.3f));
        }
    }

    const char* hint = "BACKSPACE  TO  GO  BACK";
    const int   hw   = MeasureText(hint, 18);
    DrawText(hint, (kScreenW - hw) / 2, kScreenH - 70, 18, Fade(ShipColor(0), 0.45f));
}

void Game::DrawMenu() const
{
    const char* title = "NEON ASTEROIDS";
    const int   tw    = MeasureText(title, 72);
    DrawText(title, (kScreenW - tw) / 2, 130, 72, ShipColor(0));

    const char* options[kMaxPlayers] = {"1  PLAYER", "2  PLAYERS"};

    for (int i = 0; i < kMaxPlayers; i++)
    {
        const bool  selected = (i == menuSelection_);
        const int   size     = selected ? 36 : 30;
        const int   w        = MeasureText(options[i], size);
        const int   y        = 300 + i * 60;

        // Colour each option as the player it represents, so two-player mode
        // previews the second ship's amber.
        const Color tint = (i == 1) ? ShipColor(1) : ShipColor(0);
        DrawText(options[i], (kScreenW - w) / 2, y, size,
                 selected ? tint : Fade(tint, 0.35f));

        if (selected)
        {
            // Chevron that breathes, so the selection is unmistakable.
            const float pulse = 6.0f * std::sin(time_ * 5.0f);
            DrawText(">", (kScreenW - w) / 2 - 40 + static_cast<int>(pulse), y, size, tint);
        }
    }

    const char* hint = "UP / DOWN  TO  CHOOSE      ENTER  OR  A  TO  START";
    const int   hw   = MeasureText(hint, 18);
    DrawText(hint, (kScreenW - hw) / 2, 470, 18, Fade(ShipColor(0), 0.55f));

    // Player 1 only keeps the arrows when playing alone, so say so accurately.
    const char* p1 = (menuSelection_ == 0)
        ? "P1   WASD / ARROWS      SPACE FIRE      L-SHIFT HYPERSPACE      or  PAD 1"
        : "P1   W A D      SPACE FIRE      L-SHIFT HYPERSPACE      or  PAD 1";
    const int p1w = MeasureText(p1, 15);
    DrawText(p1, (kScreenW - p1w) / 2, 540, 15, Fade(ShipColor(0), 0.40f));

    if (menuSelection_ == 1)
    {
        const char* p2 = "P2   ARROW KEYS      R-SHIFT FIRE      /  HYPERSPACE      or  PAD 2";
        const int   p2w = MeasureText(p2, 15);
        DrawText(p2, (kScreenW - p2w) / 2, 564, 15, Fade(ShipColor(1), 0.40f));
    }
}

void Game::DrawHud() const
{
    char buf[64];

    std::snprintf(buf, sizeof(buf), "WAVE %d", wave_);
    DrawText(buf, 24, 20, 34, Fade(ShipColor(0), 0.9f));

    // Progress through the current wave.
    const int remaining = static_cast<int>(asteroids_.size());
    std::snprintf(buf, sizeof(buf), "%d ROCK%s LEFT", remaining, (remaining == 1) ? "" : "S");
    DrawText(buf, 24, 62, 16, Fade(ShipColor(0), 0.45f));

    // Lives as ship glyphs. Player 0 hugs the right edge; each further player
    // stacks below, in its own colour.
    for (const Ship& ship : ships_)
    {
        const Color tint = ShipColor(ship.player);
        const float row  = 36.0f + static_cast<float>(ship.player) * 34.0f;

        for (int i = 0; i < ship.lives; i++)
        {
            const Vector2 at = {kScreenW - 40.0f - static_cast<float>(i) * 30.0f, row};
            const std::vector<Vector2> glyph = {
                {at.x + 0.0f, at.y - 11.0f},
                {at.x - 8.0f, at.y +  9.0f},
                {at.x + 0.0f, at.y +  4.0f},
                {at.x + 8.0f, at.y +  9.0f},
            };
            DrawNeonPolyline(glyph, 1.6f, Fade(tint, 0.85f), true);
        }

        // Active-powerup pips beneath the lives, so held effects are visible.
        float px = kScreenW - 44.0f;
        if (ship.fanFire)
        {
            DrawText("FAN", static_cast<int>(px) - 24, static_cast<int>(row) + 16, 14,
                     PowerupColor(PowerupType::FanFire));
            px -= 52.0f;
        }
        if (ship.shield)
        {
            DrawText("SHLD", static_cast<int>(px) - 30, static_cast<int>(row) + 16, 14,
                     PowerupColor(PowerupType::Shield));
        }
    }

    if (IsGamepadAvailable(0))
    {
        const char* name = GetGamepadName(0);
        std::snprintf(buf, sizeof(buf), "PAD  %s", (name != nullptr) ? name : "CONNECTED");
        DrawText(buf, 24, kScreenH - 34, 16, Fade(ShipColor(0), 0.45f));
    }

    if (state_ == State::GameOver)
    {
        const char* title = "GAME OVER";
        const int   tw    = MeasureText(title, 64);
        DrawText(title, (kScreenW - tw) / 2, kScreenH / 2 - 80, 64, ShipColor(0));

        // You die *on* a wave, so the number you cleared is one less.
        const int cleared = wave_ - 1;
        std::snprintf(buf, sizeof(buf), "%d WAVE%s CLEARED", cleared, (cleared == 1) ? "" : "S");
        const int cw = MeasureText(buf, 28);
        DrawText(buf, (kScreenW - cw) / 2, kScreenH / 2 - 4, 28, Fade(ShipColor(0), 0.85f));

        const char* hint = "R  OR  START  TO  PLAY  AGAIN      M  OR  B  FOR  MENU";
        const int   hw   = MeasureText(hint, 20);
        DrawText(hint, (kScreenW - hw) / 2, kScreenH / 2 + 46, 20, Fade(ShipColor(0), 0.6f));
    }
}
