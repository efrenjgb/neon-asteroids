#pragma once

#include "audio.hpp"
#include "entities.hpp"

#include <vector>

// Per-frame intent for one ship, decoupled from where it came from. Reading a
// second player, or an AI, means producing one of these — nothing downstream
// needs to know the difference.
struct ShipControls
{
    float turn       = 0.0f;   // -1 left .. +1 right
    bool  thrust     = false;
    bool  fire       = false;
    bool  hyperspace = false;  // edge-triggered by the caller
};

class Game
{
public:
    void Init();
    void Shutdown();
    void Update(float dt);

    // Draws the world and HUD. Call inside PostFX::BeginScene/EndScene.
    void Draw() const;

    Vector2 ShakeOffset() const;

private:
    enum class State
    {
        Menu,
        Claiming,   // players press a button to bind their controller
        Playing,
        GameOver,
    };

    // One entry per active player. Adding a second player is pushing another
    // Ship here — every system below iterates rather than assuming one.
    std::vector<Ship>     ships_;
    std::vector<Asteroid> asteroids_;
    std::vector<Bullet>   bullets_;
    std::vector<Ufo>      ufos_;
    std::vector<Powerup>  powerups_;
    std::vector<Particle> particles_;
    std::vector<Star>     stars_;
    Audio                 audio_{};

    State state_       = State::Menu;
    int   wave_        = 1;
    float time_        = 0.0f;
    float shake_       = 0.0f;
    int   playerCount_ = 1;
    float ufoSpawnTimer_ = 0.0f;

    // Which gamepad slot each player drives, or -1 for keyboard. Set by the
    // claim step, so two identical controllers get assigned deliberately rather
    // than by Bluetooth connection order.
    int playerPad_[kMaxPlayers] = {-1, -1};
    int claimIndex_ = 0;   // which player is currently claiming

    // 0 = one player, 1 = two players.
    int  menuSelection_ = 0;
    bool menuAxisHeld_  = false;   // edge-detects stick nav so it steps once

    void StartWave(int wave);
    void ResetRun(int playerCount);
    void SpawnShip(Ship& ship);
    void EnterMenu();
    void UpdateMenu();
    void UpdateClaim();

    ShipControls ReadControls(int player) const;

    void UpdateShip(Ship& ship, const ShipControls& in, float dt);
    void UpdateBullets(float dt);
    void UpdateUfos(float dt);
    void UpdateAsteroids(float dt);

    void SpawnUfo();
    void UfoFire(Ufo& u);
    void DestroyUfo(size_t index);

    void UpdatePowerups(float dt);
    void SpawnPowerup(Vector2 at);
    void ApplyPowerup(Ship& ship, PowerupType type);

    // Nearest live ship to a point, or nullptr if none are on the board.
    const Ship* NearestShip(Vector2 from) const;
    void UpdateParticles(float dt);
    void UpdateStars(float dt);
    void ResolveCollisions();

    void Fire(Ship& ship);
    void EnterHyperspace(Ship& ship);
    void EmergeFromHyperspace(Ship& ship);
    void SplitAsteroid(size_t index);
    void KillShip(Ship& ship);

    void SpawnThrustParticles(const Ship& ship, float dt);
    void SpawnBurst(Vector2 at, Color color, int count, float speed);
    void SpawnRing(Vector2 at, Color color, int count, float speed, bool inward);

    void Rumble(float leftMotor, float rightMotor, float duration) const;

    void DrawHud() const;
    void DrawMenu() const;
    void DrawClaim() const;
};
