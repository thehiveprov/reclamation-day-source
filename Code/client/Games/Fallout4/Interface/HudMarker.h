#pragma once

struct NiPoint3;
struct IMenu;
namespace GFx
{
struct Value;
struct Movie;
}

// A marker in the game's own HUD (HUDMenu's Flash movie, no overlay): a small line icon in the HUD's colour with the
// HUD's glow, put on a point in the world, larger the nearer it is, pulsing softly. Main thread only.
struct HudMarker
{
    // Other drawings of ours in the HUD movie (the actor bars), made of shader targets of their own (MakeShaderTarget):
    // changed only from the HUD's frame (Advance), their targets' rectangles added to the HUD's drawing along with the
    // marker's (AppendShaderFXInfos, AppendShaderTarget; only the targets of apHud's movie), and taken down when the HUD
    // goes, while its movie is still there (TearDown). A layer lives as long as the game.
    struct Layer
    {
        virtual ~Layer() = default;
        virtual void Advance(IMenu* apHud) noexcept = 0;
        virtual void AppendShaderFXInfos(IMenu* apHud, void* apBackground, void* apForeground) noexcept = 0;
        virtual void TearDown(IMenu* apHud) noexcept = 0;
    };
    static void AddLayer(Layer* apLayer) noexcept;

    // A display object as one of the HUD's shader targets, as the marker is: its drawing multiplied by the colour of
    // aColourType (the game's HUDColorTypes: 1 white, 2 the gameplay HUD colour, 5 the warning colour; 0 is BLACK, so
    // anything drawn turns black), so the game's own way is to draw it in white. With aBacking, the HUD's shaded backing
    // (a gradient box) fills the object's whole bounds. Freed with FreeShaderTarget while its movie is still there.
    static uint8_t* MakeShaderTarget(const GFx::Value& acObject, int32_t aColourType = 2, bool aBacking = true) noexcept;
    // A target's colour fixed to aColour (0xRRGGBB, the game's colour type 7), which the player's HUD colour changes
    // leave alone; its drawing is multiplied by it, so draw in white
    static void SetShaderTargetColour(uint8_t* apTarget, uint32_t aColour) noexcept;
    static void FreeShaderTarget(uint8_t* apTarget) noexcept;
    // A target's rectangles for the HUD's drawing (from a layer's AppendShaderFXInfos)
    static void AppendShaderTarget(uint8_t* apTarget, void* apBackground, void* apForeground) noexcept;
    // The HUD colour the player picked (Fallout4Prefs.ini)
    static uint32_t GetHudColour() noexcept;

    enum class Icon : uint8_t
    {
        kNone,
        kClimb,
        kVault,
        kStep,
        // a climb without the AP for it
        kNoAp,
        // where the grappling gun's spike will land
        kTarget,
    };

    // Shows aIcon on a point in the world (hidden while the point is behind the view); kNone hides it
    static void Show(Icon aIcon, const NiPoint3& acWorldPoint, float aDelta) noexcept;
    static void Hide() noexcept;

    // Where a point in the world is on the screen (0..1 left to right, top to bottom), from the world camera; false
    // behind the view
    static bool WorldToScreen(const NiPoint3& acWorldPoint, float& aX, float& aY) noexcept;
    // Where a point on the screen (0..1) is in the space of acObject, a display object in the HUD movie (from the HUD's
    // frame)
    static bool ScreenToLocal(GFx::Movie* apMovie, const GFx::Value& acObject, float aX, float aY, double& aOutX, double& aOutY) noexcept;
    // Where a point in the world is from the world camera: how far right of and above the middle of the view it is and
    // how far ahead (world units); false when the camera isn't known
    static bool WorldToView(const NiPoint3& acWorldPoint, float& aRight, float& aUp, float& aAhead) noexcept;
    // The world camera's world to clip matrix (by rows: a world point's clip x, y, z and w); false when there's no camera
    static bool GetWorldToClip(float (&aOut)[4][4]) noexcept;
    // Where the world camera is
    static bool GetCameraPosition(NiPoint3& aOut) noexcept;
};
