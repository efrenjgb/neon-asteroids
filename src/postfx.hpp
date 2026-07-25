#pragma once

#include <raylib.h>

// Scene resolve: the world is drawn into a 2x oversized render target and
// filtered back down to the window on present. Render textures ignore the MSAA
// window hint, so without this the thin vector lines would alias hard.
//
// The blit uses a negative source height because raylib stores render-texture
// contents vertically flipped.
class PostFX
{
public:
    bool Load(int width, int height);
    void Unload();

    // Draw the world inside this pair.
    void BeginScene() const;
    void EndScene() const;

    // Downsamples to the backbuffer. shakeOffset nudges the blit for screen shake.
    void Present(Vector2 shakeOffset) const;

    float vignette = 0.35f;   // 0 disables the corner falloff

private:
    int width_  = 0;          // logical (window) size
    int height_ = 0;

    RenderTexture2D scene_{};
    Shader          resolveShader_{};
    int             locVignette_ = -1;
    bool            loaded_      = false;
};
