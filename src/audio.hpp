#pragma once

#include <raylib.h>
#include <vector>

// All sound is synthesized at startup — there are no audio assets on disk.
// Everything below is built from noise, swept oscillators and envelopes, which
// suits the vector look and keeps the project a single self-contained binary.
class Audio
{
public:
    bool Load();
    void Unload();

    void PlayShoot();
    void PlayExplosion(int tier);
    void PlayShipDeath();
    void PlayWaveStart();

    // `emerging` picks the arrival variant; entry sweeps down, exit sweeps up.
    void PlayHyperspace(bool emerging);

    void PlayUfoAppear();
    void PlayUfoShoot();
    void PlayUfoWarble(bool small);   // small saucers warble higher and faster

    void PlayPowerup();       // collecting a pickup
    void PlayShieldBreak();   // shield absorbing a hit

    // Thrust is sustained, so it is driven by state rather than events. The
    // gain ramp happens per-sample inside the synthesis, so no dt is needed.
    void SetThrust(bool active);

    float masterVolume = 0.8f;

private:
    // A sound plus a ring of aliases, so repeat triggers overlap instead of
    // cutting each other off.
    struct Pool
    {
        Sound              source{};
        std::vector<Sound> voices;
        size_t             next = 0;

        void Load(Wave wave, int voiceCount);
        void Unload();
        void Play(float volume, float pitch);
    };

    Pool shoot_{};
    Pool explosionLarge_{};
    Pool explosionMedium_{};
    Pool explosionSmall_{};
    Pool shipDeath_{};
    Pool waveStart_{};
    Pool warpOut_{};
    Pool warpIn_{};
    Pool ufoWarble_{};
    Pool ufoShoot_{};
    Pool powerup_{};
    Pool shieldBreak_{};

    // Thrust is generated continuously into a stream rather than looping a
    // fixed buffer. A looped Sound has to be retriggered by polling, which
    // leaves a sub-frame gap at every loop point and clicks twice a second.
    // Streaming has no loop point at all.
    AudioStream        thrustStream_{};
    std::vector<short> thrustChunk_;

    // Ramped per-sample. Stepping the gain once per frame instead produces
    // zipper noise: 60 small amplitude discontinuities a second, which reads
    // as a grinding sound whenever the level is moving.
    float thrustGain_   = 0.0f;
    float thrustTarget_ = 0.0f;

    // Synthesis state, persisted across chunks so the signal stays continuous.
    float        thrustLp1_  = 0.0f;
    float        thrustLp2_  = 0.0f;
    float        thrustPhase_ = 0.0f;
    unsigned int thrustRng_  = 0x9E3779B9u;   // private to the synth

    void FillThrustChunk();

    bool loaded_ = false;
};
