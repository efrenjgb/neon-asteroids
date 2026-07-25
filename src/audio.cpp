#include "audio.hpp"

#include "entities.hpp"   // RandF

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace
{
constexpr int kSampleRate = 44100;

// ~46 ms per chunk at 44.1 kHz: long enough that refills are cheap, short
// enough that the gain ramp still sounds immediate.
constexpr int kThrustChunkFrames = 2048;

// One-pole smoothing coefficients: 1 - exp(-1 / (tau * sampleRate)).
// tau = 35 ms rising, 70 ms falling, so releasing thrust spins down rather
// than cutting. Roughly 3 tau to settle.
constexpr float kThrustAttackCoeff  = 0.000648f;
constexpr float kThrustReleaseCoeff = 0.000324f;

// ------------------------------------------------------------ synthesis ---

using Samples = std::vector<float>;

int SampleCount(float seconds)
{
    return static_cast<int>(seconds * static_cast<float>(kSampleRate));
}

float NoiseSample()
{
    return RandF(-1.0f, 1.0f);
}

// One-pole lowpass. `a` near 1 passes everything, near 0 is heavily muffled.
struct Lowpass
{
    float y = 0.0f;
    float Process(float x, float a)
    {
        y += a * (x - y);
        return y;
    }
};

// Scale so the loudest sample lands exactly at `target`. Noise-based sounds
// peak differently every run, so a hand-tuned gain constant either clips on an
// unlucky seed or leaves headroom on a lucky one. This is deterministic.
void NormalizePeak(Samples& s, float target)
{
    float peak = 0.0f;
    for (float v : s) peak = std::max(peak, std::fabs(v));
    if (peak <= 1e-6f) return;

    const float gain = target / peak;
    for (float& v : s) v *= gain;
}

// Convert to signed 16-bit and hand raylib a malloc'd buffer it can own.
Wave ToWave(const Samples& s)
{
    Wave w{};
    w.frameCount = static_cast<unsigned int>(s.size());
    w.sampleRate = kSampleRate;
    w.sampleSize = 16;
    w.channels   = 1;

    auto* data = static_cast<short*>(std::malloc(s.size() * sizeof(short)));
    for (size_t i = 0; i < s.size(); i++)
    {
        const float clamped = (s[i] < -1.0f) ? -1.0f : (s[i] > 1.0f) ? 1.0f : s[i];
        data[i] = static_cast<short>(clamped * 32000.0f);
    }
    w.data = data;

    return w;
}

// Laser zap. A square wave up in the 900 Hz range reads as a platformer jump,
// so this uses a sawtooth roughly an octave lower, stacked with a sub-octave
// for weight and opened with a noise transient for sizzle.
Samples MakeShoot()
{
    const float dur = 0.20f;
    const int   n   = SampleCount(dur);

    Samples out(static_cast<size_t>(n));
    float phase = 0.0f;
    float subPhase = 0.0f;

    for (int i = 0; i < n; i++)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n);

        // Steep downward sweep is what makes it read as a discharge.
        const float freq = 420.0f * std::pow(70.0f / 420.0f, t);

        phase += freq / static_cast<float>(kSampleRate);
        phase -= std::floor(phase);
        const float saw = phase * 2.0f - 1.0f;

        subPhase += (freq * 0.5f) / static_cast<float>(kSampleRate);
        subPhase -= std::floor(subPhase);
        const float sub = subPhase * 2.0f - 1.0f;

        // Brief noise crack on the attack only.
        const float transient = NoiseSample() * std::exp(-t * 55.0f) * 0.35f;

        const float env = std::exp(-t * 4.5f);

        out[static_cast<size_t>(i)] = (saw * 0.62f + sub * 0.38f + transient) * env * 0.55f;
    }

    return out;
}

// Filtered noise burst with a falling cutoff, plus a low sine thump for the
// bigger rocks. Larger tiers are longer, darker and boomier.
Samples MakeExplosion(int tier)
{
    const float dur = (tier == 3) ? 0.80f : (tier == 2) ? 0.58f : 0.40f;
    const float decay = (tier == 3) ? 4.2f : (tier == 2) ? 6.0f : 8.5f;
    const float thumpFreq = (tier == 3) ? 58.0f : (tier == 2) ? 82.0f : 0.0f;

    const int n = SampleCount(dur);
    Samples out(static_cast<size_t>(n));

    Lowpass lp;
    float   thumpPhase = 0.0f;

    for (int i = 0; i < n; i++)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n);

        // Cutoff closes over time so the tail darkens as it fades.
        const float cutoff = 0.42f * std::exp(-t * 3.0f) + 0.015f;
        const float noise  = lp.Process(NoiseSample(), cutoff);
        const float env    = std::exp(-t * decay);

        float v = noise * env;

        if (thumpFreq > 0.0f)
        {
            thumpPhase += 2.0f * PI * thumpFreq / static_cast<float>(kSampleRate);
            v += std::sin(thumpPhase) * std::exp(-t * 7.0f) * 0.55f;
        }

        out[static_cast<size_t>(i)] = v * 0.85f;
    }

    return out;
}

// Ship death, built as an actual explosion rather than a pitched sweep.
//
// The previous version leaned on a descending sawtooth, and a sustained tonal
// element is precisely what stops something reading as an explosion — you hear
// a pitch, not a detonation. This is broadband instead, in the three parts a
// real blast has:
//
//   crack  — a few milliseconds of bright noise: the initial rupture
//   body   — broadband noise whose filter closes as it decays, becoming rumble
//   boom   — a low sine dropping fast in pitch, felt as impact rather than tone
//
// The body uses a two-stage decay (fast blast + slow tail) because a single
// exponential decays too evenly and sounds synthetic.
Samples MakeShipDeath()
{
    const float dur = 1.60f;
    const int   n   = SampleCount(dur);

    Samples out(static_cast<size_t>(n));
    Lowpass lpBody;
    Lowpass lpBody2;
    float   boomPhase = 0.0f;

    for (int i = 0; i < n; i++)
    {
        // Time constants are in real seconds so the shape does not shift if
        // the duration is retuned.
        const float sec = static_cast<float>(i) / static_cast<float>(kSampleRate);

        // --- crack: gone in ~50 ms, gives the onset its edge ---
        const float crack = NoiseSample() * std::exp(-sec * 55.0f) * 0.8f;

        // --- body: filter closes over time, so it darkens as it decays ---
        const float cutoff = 0.30f * std::exp(-sec * 3.5f) + 0.004f;
        float body = lpBody.Process(NoiseSample(), cutoff);
        body = lpBody2.Process(body, cutoff);

        const float bodyEnv = 0.75f * std::exp(-sec * 6.0f)    // initial blast
                            + 0.35f * std::exp(-sec * 1.4f);   // rumbling tail
        body *= bodyEnv * 7.0f;   // heavy filtering costs amplitude

        // --- boom: 95 Hz falling to 32 Hz within a fraction of a second ---
        const float boomFreq = 95.0f * std::exp(-sec * 9.0f) + 32.0f;
        boomPhase += 2.0f * PI * boomFreq / static_cast<float>(kSampleRate);
        const float boom = std::sin(boomPhase) * std::exp(-sec * 3.2f) * 0.9f;

        out[static_cast<size_t>(i)] = crack + body + boom;
    }

    // Layers align on the attack, so the raw peak overshoots badly.
    NormalizePeak(out, 0.95f);
    return out;
}

// Hyperspace whoosh: a resonant-feeling noise sweep plus a tone gliding the
// same direction. Descending reads as collapsing away, ascending as arriving.
Samples MakeWarp(bool ascending)
{
    const float dur = 0.55f;
    const int   n   = SampleCount(dur);

    Samples out(static_cast<size_t>(n));
    Lowpass lp;
    float   phase = 0.0f;

    const float fromHz = ascending ? 110.0f : 780.0f;
    const float toHz   = ascending ? 780.0f : 110.0f;

    for (int i = 0; i < n; i++)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n);

        // Sweeping the filter cutoff with the tone is what makes it read as
        // motion rather than just a pitch change.
        const float sweep  = std::pow(toHz / fromHz, t);
        const float cutoff = std::clamp(0.06f * sweep, 0.004f, 0.85f);
        const float noise  = lp.Process(NoiseSample(), cutoff);

        const float freq = fromHz * sweep;
        phase += 2.0f * PI * freq / static_cast<float>(kSampleRate);
        const float tone = std::sin(phase);

        // Arrival lands hard; departure fades out.
        const float env = ascending
            ? std::min(t * 3.0f, 1.0f) * std::exp(-t * 2.0f)
            : std::exp(-t * 2.4f);

        out[static_cast<size_t>(i)] = (noise * 1.6f + tone * 0.45f) * env;
    }

    NormalizePeak(out, 0.9f);
    return out;
}

// The saucer warble: a tone whose pitch is modulated by a slow oscillator.
// That vibrato is the whole character — a steady tone reads as a machine, a
// wobbling one reads as something alive and hostile.
Samples MakeUfoWarble()
{
    const float dur = 0.42f;
    const int   n   = SampleCount(dur);

    Samples out(static_cast<size_t>(n));
    float   phase = 0.0f;

    for (int i = 0; i < n; i++)
    {
        const float sec = static_cast<float>(i) / static_cast<float>(kSampleRate);
        const float t   = static_cast<float>(i) / static_cast<float>(n);

        // 11 Hz vibrato swinging the carrier a fifth either way.
        const float wobble = std::sin(2.0f * PI * 11.0f * sec);
        const float freq   = 168.0f * std::pow(1.5f, wobble);

        phase += 2.0f * PI * freq / static_cast<float>(kSampleRate);

        // Soft-clipped, for a reedy tone with some bite rather than a pure sine.
        const float tone = std::tanh(std::sin(phase) * 2.4f);

        // Gentle in and out so repeated plays do not click.
        const float env = std::min(t * 12.0f, 1.0f) * std::min((1.0f - t) * 12.0f, 1.0f);

        out[static_cast<size_t>(i)] = tone * env * 0.5f;
    }

    NormalizePeak(out, 0.85f);
    return out;
}

// Saucer weapon: lower and dirtier than the player's, so incoming fire is
// distinguishable from your own by ear alone.
Samples MakeUfoShoot()
{
    const float dur = 0.22f;
    const int   n   = SampleCount(dur);

    Samples out(static_cast<size_t>(n));
    float   phase = 0.0f;

    for (int i = 0; i < n; i++)
    {
        const float t    = static_cast<float>(i) / static_cast<float>(n);
        const float freq = 260.0f * std::pow(48.0f / 260.0f, t);

        phase += freq / static_cast<float>(kSampleRate);
        phase -= std::floor(phase);

        const float saw   = phase * 2.0f - 1.0f;
        const float noise = NoiseSample() * 0.25f;
        const float env   = std::exp(-t * 5.0f);

        out[static_cast<size_t>(i)] = (saw * 0.7f + noise) * env;
    }

    NormalizePeak(out, 0.85f);
    return out;
}

// Pickup: a bright three-note arpeggio, unmistakably positive against the
// harsher combat sounds.
Samples MakePowerup()
{
    const float dur = 0.34f;
    const int   n   = SampleCount(dur);

    Samples out(static_cast<size_t>(n));

    // A major triad climbing, one note per third of the sound.
    const float notes[3] = {523.0f, 659.0f, 784.0f};   // C5 E5 G5
    float phase = 0.0f;

    for (int i = 0; i < n; i++)
    {
        const float t    = static_cast<float>(i) / static_cast<float>(n);
        const int   step = std::min(static_cast<int>(t * 3.0f), 2);
        const float freq = notes[step];

        phase += 2.0f * PI * freq / static_cast<float>(kSampleRate);

        // A little square adds sparkle over the sine.
        const float tone = std::sin(phase) * 0.7f + ((std::sin(phase) >= 0.0f) ? 0.15f : -0.15f);

        // Re-attack each note so the triad is articulated, not a glide.
        const float local = t * 3.0f - static_cast<float>(step);
        const float env   = std::min(local * 8.0f, 1.0f) * (1.0f - local * 0.3f);

        out[static_cast<size_t>(i)] = tone * env * 0.5f;
    }

    NormalizePeak(out, 0.85f);
    return out;
}

// Shield break: a short bright burst with a metallic ring, so absorbing a hit
// reads as a save rather than damage.
Samples MakeShieldBreak()
{
    const float dur = 0.35f;
    const int   n   = SampleCount(dur);

    Samples out(static_cast<size_t>(n));
    float   p1 = 0.0f, p2 = 0.0f;

    for (int i = 0; i < n; i++)
    {
        const float t = static_cast<float>(i) / static_cast<float>(n);

        // Two detuned high partials beating against each other give the shimmer.
        p1 += 2.0f * PI * 880.0f / static_cast<float>(kSampleRate);
        p2 += 2.0f * PI * 932.0f / static_cast<float>(kSampleRate);
        const float ring = (std::sin(p1) + std::sin(p2)) * 0.5f;

        const float noise = NoiseSample() * std::exp(-t * 22.0f) * 0.5f;   // impact
        const float env   = std::exp(-t * 5.0f);

        out[static_cast<size_t>(i)] = (ring * 0.6f + noise) * env;
    }

    NormalizePeak(out, 0.85f);
    return out;
}

// A quick rising blip to mark a new wave.
Samples MakeWaveStart()
{
    const float dur = 0.30f;
    const int   n   = SampleCount(dur);

    Samples out(static_cast<size_t>(n));
    float   phase = 0.0f;

    for (int i = 0; i < n; i++)
    {
        const float t    = static_cast<float>(i) / static_cast<float>(n);
        const float freq = 220.0f * std::pow(660.0f / 220.0f, t);

        phase += 2.0f * PI * freq / static_cast<float>(kSampleRate);

        const float sine = std::sin(phase);
        const float env  = std::min(t * 18.0f, 1.0f) * std::exp(-t * 3.4f);

        out[static_cast<size_t>(i)] = sine * env * 0.5f;
    }

    return out;
}
}  // namespace

// ----------------------------------------------------------------- Pool ---

void Audio::Pool::Load(Wave wave, int voiceCount)
{
    source = LoadSoundFromWave(wave);
    UnloadWave(wave);   // LoadSoundFromWave copies the samples out

    voices.reserve(static_cast<size_t>(voiceCount));
    for (int i = 0; i < voiceCount; i++)
        voices.push_back(LoadSoundAlias(source));
}

void Audio::Pool::Unload()
{
    // Aliases share the source's sample data, so they must go first.
    for (Sound& s : voices) UnloadSoundAlias(s);
    voices.clear();
    UnloadSound(source);
}

void Audio::Pool::Play(float volume, float pitch)
{
    if (voices.empty()) return;

    Sound& s = voices[next];
    next = (next + 1) % voices.size();

    SetSoundVolume(s, volume);
    SetSoundPitch(s, pitch);
    PlaySound(s);
}

// ---------------------------------------------------------------- Audio ---

bool Audio::Load()
{
    if (!IsAudioDeviceReady()) return false;

    shoot_.Load(ToWave(MakeShoot()), 8);
    explosionLarge_.Load(ToWave(MakeExplosion(3)), 4);
    explosionMedium_.Load(ToWave(MakeExplosion(2)), 5);
    explosionSmall_.Load(ToWave(MakeExplosion(1)), 6);
    shipDeath_.Load(ToWave(MakeShipDeath()), 2);
    waveStart_.Load(ToWave(MakeWaveStart()), 2);
    warpOut_.Load(ToWave(MakeWarp(/*ascending=*/false)), 3);
    warpIn_.Load(ToWave(MakeWarp(/*ascending=*/true)), 3);
    ufoWarble_.Load(ToWave(MakeUfoWarble()), 3);
    ufoShoot_.Load(ToWave(MakeUfoShoot()), 5);
    powerup_.Load(ToWave(MakePowerup()), 3);
    shieldBreak_.Load(ToWave(MakeShieldBreak()), 3);

    SetAudioStreamBufferSizeDefault(kThrustChunkFrames);
    thrustStream_ = LoadAudioStream(kSampleRate, 16, 1);
    thrustChunk_.resize(kThrustChunkFrames);
    SetAudioStreamVolume(thrustStream_, 0.0f);

    // The stream runs for the whole session and only its volume changes. Never
    // starting or stopping it removes that transient as a source of clicks.
    PlayAudioStream(thrustStream_);

    loaded_ = true;
    return true;
}

void Audio::Unload()
{
    if (!loaded_) return;

    shoot_.Unload();
    explosionLarge_.Unload();
    explosionMedium_.Unload();
    explosionSmall_.Unload();
    shipDeath_.Unload();
    waveStart_.Unload();
    warpOut_.Unload();
    warpIn_.Unload();
    ufoWarble_.Unload();
    ufoShoot_.Unload();
    powerup_.Unload();
    shieldBreak_.Unload();

    StopAudioStream(thrustStream_);
    UnloadAudioStream(thrustStream_);
    loaded_ = false;
}

void Audio::PlayShoot()
{
    if (!loaded_) return;
    // Slight pitch jitter stops rapid fire from sounding like a machine.
    shoot_.Play(0.30f * masterVolume, RandF(0.94f, 1.08f));
}

void Audio::PlayExplosion(int tier)
{
    if (!loaded_) return;

    Pool& pool = (tier == 3) ? explosionLarge_
               : (tier == 2) ? explosionMedium_
                             : explosionSmall_;

    pool.Play(0.45f * masterVolume, RandF(0.90f, 1.12f));
}

void Audio::PlayShipDeath()
{
    if (!loaded_) return;
    shipDeath_.Play(0.75f * masterVolume, 1.0f);
}

void Audio::PlayWaveStart()
{
    if (!loaded_) return;
    waveStart_.Play(0.35f * masterVolume, 1.0f);
}

void Audio::PlayHyperspace(bool emerging)
{
    if (!loaded_) return;
    Pool& pool = emerging ? warpIn_ : warpOut_;
    pool.Play(0.40f * masterVolume, RandF(0.96f, 1.05f));
}

void Audio::PlayUfoAppear()
{
    if (!loaded_) return;
    // The warble pitched down and played loud: an arrival, not an ambience.
    ufoWarble_.Play(0.55f * masterVolume, 0.72f);
}

void Audio::PlayUfoShoot()
{
    if (!loaded_) return;
    ufoShoot_.Play(0.34f * masterVolume, RandF(0.94f, 1.06f));
}

void Audio::PlayUfoWarble(bool small)
{
    if (!loaded_) return;
    // Small saucers sit higher, which reads as more urgent — and they are.
    ufoWarble_.Play(0.22f * masterVolume, small ? 1.35f : 1.0f);
}

void Audio::PlayPowerup()
{
    if (!loaded_) return;
    powerup_.Play(0.45f * masterVolume, 1.0f);
}

void Audio::PlayShieldBreak()
{
    if (!loaded_) return;
    shieldBreak_.Play(0.5f * masterVolume, RandF(0.97f, 1.03f));
}

// Generates one chunk of engine rumble. Filter and oscillator state live on
// the object, so consecutive chunks join with no discontinuity — there is no
// loop point and therefore nothing to click.
void Audio::FillThrustChunk()
{
    for (size_t i = 0; i < thrustChunk_.size(); i++)
    {
        // xorshift32, kept local to the synth. The shared game RNG is not safe
        // to reach into from here and would also perturb gameplay randomness.
        thrustRng_ ^= thrustRng_ << 13;
        thrustRng_ ^= thrustRng_ >> 17;
        thrustRng_ ^= thrustRng_ << 5;
        const float noise =
            (static_cast<float>(thrustRng_) / 2147483648.0f) - 1.0f;

        // Two cascaded poles: steep rolloff, so very little hiss survives.
        thrustLp1_ += 0.020f * (noise - thrustLp1_);
        thrustLp2_ += 0.020f * (thrustLp1_ - thrustLp2_);

        // Sub-bass sine gives it body rather than just filtered hiss.
        thrustPhase_ += 2.0f * PI * 44.0f / static_cast<float>(kSampleRate);
        if (thrustPhase_ > 2.0f * PI) thrustPhase_ -= 2.0f * PI;
        const float sub = std::sin(thrustPhase_) * 0.30f;

        // One-pole gain smoothing, per sample. Exponential rather than linear,
        // so the envelope has no corner at either end of the ramp — a linear
        // ramp's abrupt change in slope is itself faintly audible.
        const float coeff = (thrustTarget_ > thrustGain_) ? kThrustAttackCoeff
                                                          : kThrustReleaseCoeff;
        thrustGain_ += coeff * (thrustTarget_ - thrustGain_);

        float v = (thrustLp2_ * 11.0f + sub) * thrustGain_;
        v = (v < -1.0f) ? -1.0f : (v > 1.0f) ? 1.0f : v;

        thrustChunk_[i] = static_cast<short>(v * 32000.0f);
    }
}

void Audio::SetThrust(bool active)
{
    if (!loaded_) return;

    // Only the target moves here. FillThrustChunk walks the actual gain toward
    // it one sample at a time, so the stream volume itself stays constant and
    // never steps.
    thrustTarget_ = active ? 1.0f : 0.0f;

    SetAudioStreamVolume(thrustStream_, 0.22f * masterVolume);

    // Refill every buffer the mixer has drained. Looping rather than filling
    // one keeps the stream fed if a frame runs long — an underrun would be
    // heard as exactly the kind of tick this is meant to eliminate.
    while (IsAudioStreamProcessed(thrustStream_))
    {
        FillThrustChunk();
        UpdateAudioStream(thrustStream_, thrustChunk_.data(),
                          static_cast<int>(thrustChunk_.size()));
    }
}
