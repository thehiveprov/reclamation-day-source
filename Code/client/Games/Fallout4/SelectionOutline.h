#pragma once

// A coloured outline around one reference's 3D, drawn into the frame just before it's shown (over the game's HUD, under
// our overlay): its meshes are drawn into a mask from the game's own vertex buffers (skinned ones with their bones),
// the distance to the mask is spread out from it in two passes, and the band within the width is blended in. Main
// thread only.
struct SelectionOutline
{
    struct Style
    {
        // 0..1, not premultiplied
        float Red = 1.f;
        float Green = 1.f;
        float Blue = 1.f;
        float Opacity = 0.85f;
        // in pixels at 1080 lines, scaled up with the resolution
        int Width = 2;
    };

    // Outlines the reference with aHandle from the next frame on, for as long as it keeps being set (a frame that wasn't
    // preceded by a Set lately draws nothing: the pick stopped running, e.g. under a menu)
    static void Set(uint32_t aHandle, const Style& acStyle) noexcept;
    static void Clear() noexcept;
    // The reference that's outlined (0 for none)
    [[nodiscard]] static uint32_t Current() noexcept;

    // What the last drawn frame had: meshes drawn (static and skinned) and left out (no buffer yet, a format or skin we
    // don't draw, effects)
    struct Stats
    {
        uint32_t Static = 0;
        uint32_t Skinned = 0;
        uint32_t Skipped = 0;
        uint32_t Effects = 0;
        bool Drawn = false;
        // why the frame drew nothing ("" when it drew, "no target" when nothing was set)
        const char* Stopped = "no target";
    };
    [[nodiscard]] static Stats LastStats() noexcept;

    // From BSGraphics' Present (Hook_StopTimer), before the overlay
    static void Draw() noexcept;
};
