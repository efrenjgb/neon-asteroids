#include "postfx.hpp"

#include <algorithm>

namespace
{
// Draw the scene at 2x and let the downsample do the antialiasing.
constexpr int kSupersample = 2;

// The resolve shader is baked into the binary rather than read from a file, so
// the executable is self-contained — no shaders/ folder to ship or keep beside
// it. Kept in sync with shaders/resolve.fs (the source-of-truth copy for
// editing).
constexpr const char* kResolveFragmentShader = R"SHADER(#version 330

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;
uniform float vignetteAmount;

void main()
{
    vec3 c = texture(texture0, fragTexCoord).rgb;

    // Subtle corner falloff. Set vignetteAmount to 0 for a completely flat image.
    vec2 uv = fragTexCoord - 0.5;
    c *= 1.0 - dot(uv, uv) * vignetteAmount;

    finalColor = vec4(c, 1.0);
}
)SHADER";
}  // namespace

bool PostFX::Load(int width, int height)
{
    width_  = width;
    height_ = height;

    scene_ = LoadRenderTexture(width_ * kSupersample, height_ * kSupersample);
    SetTextureFilter(scene_.texture, TEXTURE_FILTER_BILINEAR);

    resolveShader_ = LoadShaderFromMemory(nullptr, kResolveFragmentShader);
    locVignette_   = GetShaderLocation(resolveShader_, "vignetteAmount");

    loaded_ = IsShaderValid(resolveShader_);
    if (!loaded_)
        TraceLog(LOG_WARNING, "POSTFX: resolve shader failed to load, blitting unprocessed");

    return loaded_;
}

void PostFX::Unload()
{
    UnloadShader(resolveShader_);
    UnloadRenderTexture(scene_);
}

void PostFX::BeginScene() const
{
    BeginTextureMode(scene_);

    // Scale up so game code keeps drawing in logical coordinates and never has
    // to know the scene target is oversized.
    const Camera2D cam = {
        .offset   = {0.0f, 0.0f},
        .target   = {0.0f, 0.0f},
        .rotation = 0.0f,
        .zoom     = static_cast<float>(kSupersample),
    };
    BeginMode2D(cam);
}

void PostFX::EndScene() const
{
    EndMode2D();
    EndTextureMode();
}

void PostFX::Present(Vector2 shakeOffset) const
{
    const Rectangle source = {
        0.0f, 0.0f,
        static_cast<float>(scene_.texture.width),
        -static_cast<float>(scene_.texture.height),
    };
    // Fit the fixed-size scene into whatever the window currently is, keeping
    // aspect ratio and centring the result. Without this the scene would be
    // blitted at its native size into the corner of a larger window, which is
    // what fullscreen and any resize would otherwise produce.
    const float screenW = static_cast<float>(GetScreenWidth());
    const float screenH = static_cast<float>(GetScreenHeight());

    const float scale = std::min(screenW / static_cast<float>(width_),
                                 screenH / static_cast<float>(height_));

    const float drawW = static_cast<float>(width_)  * scale;
    const float drawH = static_cast<float>(height_) * scale;

    const Rectangle dest = {
        (screenW - drawW) * 0.5f + shakeOffset.x,
        (screenH - drawH) * 0.5f + shakeOffset.y,
        drawW,
        drawH,
    };

    if (loaded_)
    {
        SetShaderValue(resolveShader_, locVignette_, &vignette, SHADER_UNIFORM_FLOAT);
        BeginShaderMode(resolveShader_);
            DrawTexturePro(scene_.texture, source, dest, {0.0f, 0.0f}, 0.0f, WHITE);
        EndShaderMode();
    }
    else
    {
        DrawTexturePro(scene_.texture, source, dest, {0.0f, 0.0f}, 0.0f, WHITE);
    }
}
