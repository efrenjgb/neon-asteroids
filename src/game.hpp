#pragma once

#include "audio.hpp"
#include "entities.hpp"
#include "net_serialize.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Local play, authoritative host, or client. In Local and Host the game runs
// its own simulation; a Client never simulates and instead renders snapshots.
enum class NetRole { Local, Host, Client };

// The menu records a pending network intent; main consumes it to drive the
// socket (the menu UI lives in Game, the socket lives in main).
enum class NetRequest { None, Host, Join, Cancel };

class Game
{
public:
    void Init();
    void Shutdown();

    // The simulation advances in fixed steps while playing; menus and the
    // game-over screen run once per frame. main.cpp drives that split, which is
    // also the seam the authoritative-host netcode plugs into: the host ticks
    // the sim, the client applies snapshots instead.
    //   controls: one entry per player for a Playing tick; nullptr otherwise.
    void Update(float dt, const ShipControls* controls);

    bool         IsPlaying() const { return state_ == State::Playing; }
    ShipControls SampleControls(int player) const { return ReadControls(player); }

    // --- networking ---
    void    SetRole(NetRole r) { role_ = r; }
    NetRole GetRole() const    { return role_; }
    int     LocalPlayer() const { return localPlayer_; }

    // Enter a 2-player online match. Host builds the authoritative world; a
    // client starts empty and is populated by the first snapshot. localPlayer
    // is which ship the human here drives (host 0, client 1).
    void BeginOnline(NetRole role, int localPlayer, uint32_t seed);

    // main polls this each frame to act on the menu's HOST/JOIN/cancel choice.
    NetRequest  ConsumeNetRequest() { NetRequest r = netRequest_; netRequest_ = NetRequest::None; return r; }
    const char* JoinIp() const      { return joinIp_.c_str(); }

    // main tells the game the address to show on the hosting screen.
    void SetHostInfo(const char* ip, uint16_t port) { hostIp_ = ip; hostPort_ = port; }

    // Return to the menu from an online session, optionally with a status line
    // (e.g. "COULD NOT CONNECT"). Resets role to Local.
    void NetReturnToMenu(const char* message);
    void NetConnectFailed() { NetReturnToMenu("COULD NOT CONNECT"); }

    // Host: pack the current world (entities, wave, this tick's cosmetic events)
    // into a snapshot. Client: overwrite the world from a received snapshot and
    // replay its events into local audio and particles.
    void SerializeSnapshot(net::ByteWriter& w);
    void ApplySnapshot(const net::Snapshot& snap);

    // Client only: advance the local cosmetic layers each render frame (the
    // client never simulates). Starfield drift, particle aging, thrust plumes
    // and the engine/UFO audio the host produces continuously.
    void UpdateClientView(float dt);

    // Draws the world and HUD. Call inside PostFX::BeginScene/EndScene.
    void Draw() const;

    Vector2 ShakeOffset() const;

private:
    enum class State
    {
        Menu,
        Claiming,   // players press a button to bind their controller
        HostWait,   // hosting online, waiting for a peer to join
        JoinEntry,  // typing the host IP / connecting as a client
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

    // Menu: 0=1P, 1=2P local, 2=host online, 3=join online.
    int  menuSelection_ = 0;
    bool menuAxisHeld_  = false;   // edge-detects stick nav so it steps once

    // Online connection UI state.
    NetRequest  netRequest_ = NetRequest::None;
    std::string joinIp_;
    bool        connecting_ = false;   // JoinEntry: waiting for StartGame
    std::string hostIp_;               // shown on the hosting screen
    uint16_t    hostPort_   = 45123;
    std::string menuMessage_;          // transient status line on the menu
    float       menuMessageTimer_ = 0.0f;

    // --- networking ---
    NetRole  role_        = NetRole::Local;
    int      localPlayer_ = 0;                // which ship the local human drives
    uint32_t netTick_     = 0;                // host: increments per simulated tick
    float    clientWarbleTimer_ = 0.0f;       // client: local UFO warble cadence
    std::vector<net::NetEvent> events_;       // host: cosmetic events this tick

    // Host: append a cosmetic/audio event for the client (no-op unless hosting).
    // Client: turn a received event back into local audio + particles.
    void RecordEvent(net::EventType type, Vector2 pos, uint8_t param);
    void ReplayEvent(const net::NetEvent& e);

    void StartWave(int wave);
    void ResetRun(int playerCount);
    void SpawnShip(Ship& ship);
    void EnterMenu();
    void UpdateMenu();
    void UpdateClaim();
    void UpdateHostWait();
    void UpdateJoinEntry();

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
    void DrawHostWait() const;
    void DrawJoinEntry() const;
};
