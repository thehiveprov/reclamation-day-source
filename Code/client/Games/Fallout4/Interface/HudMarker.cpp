#include <HivemindPCH.h>

#include <Interface/HudMarker.h>

#include <Interface/IMenu.h>
#include <Interface/Scaleform.h>
#include <SaveFolder.h>
#include <Games/Primitives.h>
#include <PerfProbe.h>

#include <array>
#include <atomic>
#include <intrin.h>
#include <mutex>

namespace
{
// PlayerCamera (CommonLibF4 id 1171980): TESCamera::cameraRoot at 0x20, the world camera (a NiCamera) among its
// children; a NiNode's children (NiTObjectArray) at 0x120: the array at 0x128, its capacity at 0x130. NiCamera (read
// in game on 1.10.163): world to clip at 0x120, a row major 4x4 - rows: right / tan(half the width's angle), up /
// tan(half the height's angle), depth, and distance ahead
constexpr size_t kCameraRoot = 0x20;
constexpr size_t kNodeChildren = 0x128;
constexpr size_t kNodeChildCapacity = 0x130;
constexpr size_t kWorldToClip = 0x120;

// Icons are drawn on a 32 unit grid around their centre, this big on the HUD's 1280 x 720 stage at kNearDistance
constexpr float kIconSize = 30.f;
constexpr float kNearDistance = 150.f;
constexpr float kMinScale = 0.55f;
constexpr float kMaxScale = 1.25f;
// a soft pulse, the original's blink: this many times a second, between these
constexpr float kPulseRate = 1.6f;
constexpr float kPulseLow = 0.6f;
constexpr float kNoApPulseRate = 3.5f;
constexpr float kFadeTime = 0.12f;
// how close to the screen's edges (shares of it) the marker of a point off the screen keeps
constexpr float kEdgeX = 0.04f;
constexpr float kEdgeY = 0.07f;
// the no-AP climb's strength (its icon is the climb's)
constexpr float kNoApAlpha = 0.45f;

// What the marker should look like, from the game's update (Show, Hide); the HUD's own advance puts it on the HUD
struct Wanted
{
    HudMarker::Icon icon{HudMarker::Icon::kNone};
    // on the screen, 0..1 left to right and top to bottom
    float x{0.f};
    float y{0.f};
    float scale{1.f};
    float alpha{0.f};
};
std::mutex s_wantedLock;
Wanted s_wanted;

// The fade and the pulse (the game's update only)
float s_shown = 0.f;
float s_pulse = 0.f;

// The marker in the HUD movie: only touched in the HUD's advance, which is where the game captures the movie for
// drawing
struct Sprite
{
    void* pMovie{nullptr};
    // the crosshair's group, which the sprite is put in
    GFx::Value root;
    GFx::Value sprite;
    HudMarker::Icon drawn{HudMarker::Icon::kNone};
    // the sprite as one of the HUD's shader targets (a BSGFxShaderFXTarget): the game draws its HUD off the screen and
    // puts on the screen only the rectangles of its shader targets, so a sprite that isn't one is never seen
    uint8_t* pTarget{nullptr};
};
Sprite s_sprite;

// Our other drawings in the HUD movie (HudMarker::Layer), added once at the start
std::array<HudMarker::Layer*, 4> s_layers{};
std::atomic<size_t> s_layerCount{0};

template <class TVisit> void ForEachLayer(const TVisit& acVisit) noexcept
{
    const size_t count = s_layerCount.load();
    for (size_t i = 0; i < count; ++i)
        acVisit(*s_layers[i]);
}

// Makes the sprite a shader target, as the compass's drawn part is made (HUDCompass's constructor, CommonLibF4 id
// 29609): BSGFxShaderFXTarget(const Value&) (id 599569), the foreground effect 2 (id 876001), the background 1 (id
// 278402) and flag 4 (id 1235532). Its rectangle is added to the HUD's by the hook on AppendShaderFXInfos. It's made a
// sink of the game's HUD colour events, so it goes with the HUD (TearDown, from the HUD's destructor).
// What those do (read from the exe, 2026-11-06):
// - 876001 stores the colour type at 0xA8 and takes its colour from 165455 (a jump table by type: 0 black (0, 0, 0),
//   1 white, 2 the gameplay HUD colour, 3 the player's set colour, 4 power armor, 5 warning, 6 alt warning, 7 custom),
//   then 1256920 sets the display object's colour transform to multiply by that colour times the brightness (alpha
//   kept, nothing added). So type 0 turns anything drawn black; the game draws in white and lets type 2 tint it.
// - 278402 (EnableShadedBackground) sets the backing's colour by its type (0xAC; 85881) and its box to the target's
//   whole rectangle (insets (1 - scale) / 2), and flag 4 (1235532) makes that box a vertical gradient.
// - The target's rectangle (1352934) is its display object's whole bounds in the movie's root, strokes included
//   (1533653, "root"), put through the movie's visible frame and viewport. A target is drawn only while it and every
//   parent are visible with some alpha (1567406).
uint8_t* MakeTarget(const GFx::Value& acSprite, int32_t aColourType = 2, bool aBacking = true) noexcept
{
    using TConstructor = void*(void*, const GFx::Value&);
    using TSetEffect = void(void*, int32_t, float);
    using TSetFlag = void(void*);
    POINTER_FALLOUT4(TConstructor, s_constructor, 599570);
    POINTER_FALLOUT4(TSetEffect, s_setForeground, 876002);
    POINTER_FALLOUT4(TSetEffect, s_setBackground, 278403);
    POINTER_FALLOUT4(TSetFlag, s_setFlag4, 1235533);
    // the target is 0xB0 bytes (its last member, the background type, at 0xAC)
    constexpr size_t kTargetSize = 0x100;
    auto* pTarget = static_cast<uint8_t*>(_aligned_malloc(kTargetSize, 16));
    if (!pTarget)
        return nullptr;
    std::memset(pTarget, 0, kTargetSize);
    s_constructor.Get()(pTarget, acSprite);
    s_setForeground.Get()(pTarget, aColourType, 1.f);
    if (aBacking)
    {
        s_setBackground.Get()(pTarget, 1, 1.f);
        s_setFlag4.Get()(pTarget);
    }
    return pTarget;
}

// BSGFxShaderFXTarget's scalar deleting destructor (its vtable's slot 0), told not to free: the memory is ours. It takes
// the target off the HUD colour events
void FreeTarget(uint8_t* apTarget) noexcept
{
    if (!apTarget)
        return;
    using TDestructor = void*(void*, uint32_t);
    (*reinterpret_cast<TDestructor* const* const*>(apTarget))[0](apTarget, 0);
    _aligned_free(apTarget);
}

// The marker out of a HUD that's going away, while its movie is still there. The target's destructor takes it off the
// HUD colour events: left on them, the next HUD's opening sent it one that read the old movie, freed by then (F4SE's
// onApplyColorChange through the movie's object interface, 2026-10-15/16, four times, each on the first load after
// the marker had been shown)
void TearDown() noexcept
{
    if (s_sprite.pTarget)
    {
        FreeTarget(s_sprite.pTarget);
        s_sprite.pTarget = nullptr;
    }
    s_sprite.sprite = GFx::Value();
    s_sprite.root = GFx::Value();
    s_sprite.pMovie = nullptr;
    s_sprite.drawn = HudMarker::Icon::kNone;
}

// The HUD colour of Fallout4Prefs.ini, the game's default when it isn't set
uint32_t HudColour() noexcept
{
    static uint32_t s_colour = []
    {
        const std::wstring ini = (SaveFolder::GetGameUserFolder() / "Fallout4Prefs.ini").wstring();
        const auto get = [&](const wchar_t* acpKey, int aDefault) { return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Interface", acpKey, aDefault, ini.c_str())), 0, 255); };
        return static_cast<uint32_t>(get(L"iHUDColorR", 18) << 16 | get(L"iHUDColorG", 255) << 8 | get(L"iHUDColorB", 21));
    }();
    return s_colour;
}

// What the marker is drawn in: white, which its shader target (colour type 2) multiplies to the HUD colour the player
// has now, as the game's own HUD is drawn. Drawn in the HUD colour, it came out as that colour squared: about right for
// the default green, darker for amber, white or any colour mixed from part-lit channels
constexpr uint32_t kDrawnColour = 0xFFFFFF;

// Drawing on a sprite's graphics, in the grid's units
struct Pen
{
    GFx::Value graphics;
    float unit;

    void Move(float aX, float aY) noexcept
    {
        const GFx::Value args[] = {GFx::Value(static_cast<double>(aX * unit)), GFx::Value(static_cast<double>(aY * unit))};
        graphics.Invoke("moveTo", nullptr, args, 2);
    }
    void Line(float aX, float aY) noexcept
    {
        const GFx::Value args[] = {GFx::Value(static_cast<double>(aX * unit)), GFx::Value(static_cast<double>(aY * unit))};
        graphics.Invoke("lineTo", nullptr, args, 2);
    }
};

// An icon drawn in scan lines, the HUD's look: each line a filled quad on the grid (y down), its top edge from
// topLeft to topRight, its bottom edge from bottomLeft to bottomRight
struct ScanLine
{
    float top;
    float bottom;
    float topLeft;
    float topRight;
    float bottomLeft;
    float bottomRight;
};

// The climb: Tools/HudMarkerIcons/climb.png (pasted from the tool's output)
// From climb.png by Tools/HudMarkerIcons/make_icon.ps1 -Lines 16 -Fill 0.7 -MinSpan 0.8
constexpr ScanLine kClimbLines[] = {
    {-14.14f, -12.9f, -0.67f, 0.57f, -0.67f, 0.57f},
    {-12.43f, -11.1f, -1.33f, 1.33f, -2.19f, 2.29f},
    {-10.62f, -9.29f, -3.05f, 2.95f, -3.05f, 2.95f},
    {-8.9f, -7.57f, -3.81f, -0.57f, -4.86f, -1.33f},
    {-8.9f, -7.57f, 0.48f, 3.71f, 1.33f, 4.76f},
    {-7.1f, -5.76f, -5.24f, -1.71f, -6.1f, -2.57f},
    {-7.1f, -5.76f, 1.71f, 5.14f, 2.57f, 6.0f},
    {-5.38f, -4.05f, -6.38f, -2.95f, -7.24f, -4.0f},
    {-5.38f, -4.05f, 2.95f, 6.38f, 3.81f, 7.24f},
    {-3.57f, -2.24f, -7.52f, -4.38f, -8.57f, -5.05f},
    {-3.57f, -2.24f, 4.38f, 7.52f, 5.05f, 8.48f},
    {-1.86f, -0.52f, -9.24f, -5.9f, -9.24f, -5.9f},
    {-1.86f, -0.52f, 5.9f, 9.24f, 5.9f, 9.24f},
    {-0.05f, 1.29f, -10.19f, -6.76f, -10.95f, -7.52f},
    {-0.05f, 1.29f, -1.14f, 1.05f, -2.0f, 2.0f},
    {-0.05f, 1.29f, 6.76f, 10.1f, 7.52f, 10.95f},
    {1.76f, 3.1f, -11.33f, -7.9f, -12.1f, -8.86f},
    {1.76f, 3.1f, -2.38f, 2.38f, -3.14f, 3.14f},
    {1.76f, 3.1f, 7.9f, 11.33f, 8.95f, 12.19f},
    {3.48f, 4.81f, -12.38f, -9.24f, -13.14f, -10.0f},
    {3.48f, 4.81f, -3.52f, 3.52f, -4.38f, 4.38f},
    {3.48f, 4.81f, 9.24f, 12.48f, 10.0f, 13.33f},
    {5.29f, 6.62f, -13.52f, -10.0f, -11.71f, -8.1f},
    {5.29f, 6.62f, -4.76f, 4.86f, -5.81f, 5.81f},
    {5.29f, 6.62f, 10.0f, 13.62f, 8.19f, 11.62f},
    {7.0f, 8.33f, -12.1f, -8.86f, -12.1f, -8.86f},
    {7.0f, 8.33f, -6.67f, -0.48f, -6.67f, -0.48f},
    {7.0f, 8.33f, 0.48f, 6.67f, 0.48f, 6.67f},
    {7.0f, 8.33f, 8.86f, 12.1f, 8.86f, 12.1f},
    {8.81f, 10.14f, -13.24f, -10.0f, -13.24f, -10.0f},
    {8.81f, 10.14f, -7.81f, -1.81f, -7.81f, -1.81f},
    {8.81f, 10.14f, 1.71f, 7.81f, 1.71f, 7.81f},
    {8.81f, 10.14f, 9.9f, 13.24f, 9.9f, 13.24f},
    {10.52f, 11.86f, -13.9f, -10.86f, -14.67f, -11.33f},
    {10.52f, 11.86f, -5.43f, -2.86f, -6.19f, -3.71f},
    {10.52f, 11.86f, -1.43f, 1.33f, -2.19f, 2.19f},
    {10.52f, 11.86f, 2.76f, 5.43f, 3.71f, 6.29f},
    {10.52f, 11.86f, 10.86f, 13.9f, 11.24f, 14.67f},
    {12.33f, 13.67f, -14.95f, -8.0f, -15.9f, -8.76f},
    {12.33f, 13.67f, -6.57f, -4.1f, -7.71f, -4.86f},
    {12.33f, 13.67f, -1.71f, 1.62f, -0.57f, 0.48f},
    {12.33f, 13.67f, 4.0f, 6.67f, 4.76f, 7.62f},
    {12.33f, 13.67f, 8.0f, 14.95f, 8.67f, 15.9f},
};

// The step up: Tools/HudMarkerIcons/step.png (pasted from the tool's output)
// From step.png by Tools/HudMarkerIcons/make_icon.ps1 -Lines 9 -Fill 0.7 -MinSpan 0.8
constexpr ScanLine kStepLines[] = {
    {-5.62f, -4.67f, -13.06f, -10.98f, -14.27f, -10.81f},
    {-5.62f, -4.67f, 10.98f, 13.06f, 10.9f, 14.36f},
    {-4.41f, -3.46f, -15.14f, -12.19f, -15.14f, -12.19f},
    {-4.41f, -3.46f, -11.76f, -10.81f, -11.76f, -10.81f},
    {-4.41f, -3.46f, 10.9f, 11.85f, 10.9f, 11.85f},
    {-4.41f, -3.46f, 12.19f, 15.22f, 12.19f, 15.22f},
    {-3.2f, -2.16f, -15.57f, -13.49f, -15.91f, -13.92f},
    {-3.2f, -2.16f, 13.49f, 15.57f, 13.92f, 16.0f},
    {-1.9f, -0.95f, -15.91f, -14.27f, -16.0f, -14.27f},
    {-1.9f, -0.95f, 14.36f, 16.0f, 14.36f, 16.0f},
    {-0.69f, 0.26f, -16.0f, -14.27f, -15.91f, -13.92f},
    {-0.69f, 0.26f, -12.54f, 12.63f, -11.68f, 11.76f},
    {-0.69f, 0.26f, 14.36f, 16.0f, 13.92f, 15.91f},
    {0.61f, 1.56f, -15.57f, -13.15f, -15.57f, -13.15f},
    {0.61f, 1.56f, -10.81f, -9.77f, -10.81f, -9.77f},
    {0.61f, 1.56f, 9.6f, 10.9f, 9.6f, 10.9f},
    {0.61f, 1.56f, 13.23f, 15.57f, 13.23f, 15.57f},
    {1.82f, 2.77f, -14.36f, -11.68f, -14.36f, -11.68f},
    {1.82f, 2.77f, -10.21f, -8.74f, -10.21f, -8.74f},
    {1.82f, 2.77f, 8.74f, 10.29f, 8.74f, 10.29f},
    {1.82f, 2.77f, 11.76f, 14.44f, 11.76f, 14.44f},
    {3.11f, 4.06f, -13.06f, -8.74f, -10.38f, -8.74f},
    {3.11f, 4.06f, 8.74f, 13.06f, 8.74f, 10.46f},
    {4.32f, 5.28f, -10.38f, -8.74f, -10.03f, -8.74f},
    {4.32f, 5.28f, 8.74f, 10.46f, 8.74f, 10.03f},
};

// The vault: Tools/HudMarkerIcons/vault.png (pasted from the tool's output)
// From vault.png by Tools/HudMarkerIcons/make_icon.ps1 -Lines 16 -Fill 0.7 -MinSpan 1.3
constexpr ScanLine kVaultLines[] = {
    {-14.14f, -12.9f, -15.9f, -8.67f, -15.9f, -8.67f},
    {-14.14f, -12.9f, -7.62f, -4.86f, -7.62f, -4.86f},
    {-14.14f, -12.9f, 4.76f, 7.52f, 4.76f, 7.52f},
    {-14.14f, -12.9f, 8.67f, 15.81f, 8.67f, 15.81f},
    {-12.43f, -11.1f, -14.95f, -8.0f, -14.29f, -11.14f},
    {-12.43f, -11.1f, -6.57f, -4.1f, -5.81f, -3.24f},
    {-12.43f, -11.1f, -1.71f, 1.62f, -1.9f, 1.81f},
    {-12.43f, -11.1f, 4.0f, 6.67f, 3.24f, 5.9f},
    {-12.43f, -11.1f, 8.0f, 14.95f, 11.24f, 14.29f},
    {-10.62f, -9.29f, -13.52f, -10.38f, -13.52f, -10.38f},
    {-10.62f, -9.29f, -5.05f, -2.29f, -5.05f, -2.29f},
    {-10.62f, -9.29f, -0.86f, 0.86f, -0.86f, 0.86f},
    {-10.62f, -9.29f, 2.19f, 5.05f, 2.19f, 5.05f},
    {-10.62f, -9.29f, 10.29f, 13.62f, 10.29f, 13.62f},
    {-8.9f, -7.57f, -12.86f, -9.62f, -12.1f, -8.86f},
    {-8.9f, -7.57f, -7.43f, -1.33f, -6.57f, -0.48f},
    {-8.9f, -7.57f, 1.33f, 7.43f, 0.38f, 6.57f},
    {-8.9f, -7.57f, 9.62f, 12.86f, 8.76f, 12.0f},
    {-7.1f, -5.76f, -11.62f, -8.48f, -14.0f, -7.62f},
    {-7.1f, -5.76f, -6.1f, 6.1f, -5.14f, 5.14f},
    {-7.1f, -5.76f, 8.48f, 11.62f, 7.71f, 14.0f},
    {-5.38f, -4.05f, -13.52f, -10.0f, -12.76f, -9.52f},
    {-5.38f, -4.05f, -4.76f, 4.86f, -4.0f, 4.0f},
    {-5.38f, -4.05f, 10.0f, 13.62f, 9.62f, 12.95f},
    {-3.57f, -2.24f, -12.38f, -9.24f, -11.71f, -8.29f},
    {-3.57f, -2.24f, -3.52f, 3.52f, -2.76f, 2.76f},
    {-3.57f, -2.24f, 9.24f, 12.48f, 8.29f, 11.71f},
    {-1.86f, -0.52f, -11.33f, -7.9f, -10.57f, -7.14f},
    {-1.86f, -0.52f, -2.38f, 2.38f, -1.62f, 1.52f},
    {-1.86f, -0.52f, 7.9f, 11.33f, 7.14f, 10.48f},
    {-0.05f, 1.29f, -9.71f, -6.29f, -9.71f, -6.29f},
    {-0.05f, 1.29f, 6.29f, 9.62f, 6.29f, 9.62f},
    {1.76f, 3.1f, -8.86f, -5.43f, -7.9f, -4.67f},
    {1.76f, 3.1f, 5.43f, 8.86f, 4.67f, 7.9f},
    {3.48f, 4.81f, -7.52f, -4.38f, -6.76f, -3.33f},
    {3.48f, 4.81f, 4.38f, 7.52f, 3.24f, 6.76f},
    {5.29f, 6.62f, -6.38f, -2.95f, -5.52f, -2.1f},
    {5.29f, 6.62f, 2.95f, 6.38f, 2.0f, 5.43f},
    {7.0f, 8.33f, -5.24f, -1.71f, -4.19f, -0.95f},
    {7.0f, 8.33f, 1.71f, 5.14f, 0.95f, 4.1f},
    {8.81f, 10.14f, -3.43f, 0.0f, -3.43f, 0.0f},
    {8.81f, 10.14f, 0.19f, 3.33f, 0.19f, 3.33f},
    {10.52f, 11.86f, -2.57f, 2.57f, -1.71f, 1.71f},
    {12.33f, 13.67f, -1.05f, 0.86f, -1.05f, 0.86f},
};

template <size_t N> void DrawScanLines(Pen& pen, const ScanLine (&acLines)[N], float aAlpha) noexcept
{
    // no outline, only the fill
    pen.graphics.Invoke("lineStyle");
    const GFx::Value fillArgs[] = {GFx::Value(static_cast<double>(kDrawnColour)), GFx::Value(static_cast<double>(aAlpha))};
    pen.graphics.Invoke("beginFill", nullptr, fillArgs, 2);
    for (const ScanLine& line : acLines)
    {
        pen.Move(line.topLeft, line.top);
        pen.Line(line.topRight, line.top);
        pen.Line(line.bottomRight, line.bottom);
        pen.Line(line.bottomLeft, line.bottom);
        pen.Line(line.topLeft, line.top);
    }
    pen.graphics.Invoke("endFill");
}

// The grapple's landing point: a ring cut into scan lines, a dot in its middle (the move icons' look)
void DrawTarget(Pen& pen) noexcept
{
    constexpr float kOuter = 11.f;
    constexpr float kInner = 7.f;
    constexpr float kPitch = 2.4f;
    constexpr float kFill = 0.7f;
    constexpr float kDot = 1.7f;
    const auto half = [](float aRadius, float aY) { return aY * aY < aRadius * aRadius ? std::sqrt(aRadius * aRadius - aY * aY) : 0.f; };
    const auto quad = [&pen](float aTop, float aBottom, float aTopLeft, float aTopRight, float aBottomLeft, float aBottomRight)
    {
        pen.Move(aTopLeft, aTop);
        pen.Line(aTopRight, aTop);
        pen.Line(aBottomRight, aBottom);
        pen.Line(aBottomLeft, aBottom);
        pen.Line(aTopLeft, aTop);
    };

    pen.graphics.Invoke("lineStyle");
    const GFx::Value fillArgs[] = {GFx::Value(static_cast<double>(kDrawnColour)), GFx::Value(1.)};
    pen.graphics.Invoke("beginFill", nullptr, fillArgs, 2);
    for (float y = -kOuter + kPitch * 0.5f; y < kOuter; y += kPitch)
    {
        const float top = y - kPitch * kFill * 0.5f;
        const float bottom = y + kPitch * kFill * 0.5f;
        const float outerTop = half(kOuter, top);
        const float outerBottom = half(kOuter, bottom);
        const float innerTop = half(kInner, top);
        const float innerBottom = half(kInner, bottom);
        if (outerTop <= 0.f && outerBottom <= 0.f)
            continue;
        if (innerTop <= 0.f && innerBottom <= 0.f)
        {
            quad(top, bottom, -outerTop, outerTop, -outerBottom, outerBottom);
            continue;
        }
        quad(top, bottom, -outerTop, -innerTop, -outerBottom, -innerBottom);
        quad(top, bottom, innerTop, outerTop, innerBottom, outerBottom);
    }
    quad(-kDot, kDot, -kDot, kDot, -kDot, kDot);
    pen.graphics.Invoke("endFill");
}

// The icons: what the move does, in scan lines drawn in white and tinted to the HUD's colour by the marker's shader target
// (the game's HUD effect draws its dark edge around them); a climb without the AP for it is the climb's, fainter
void Draw(GFx::Value& aSprite, HudMarker::Icon aIcon) noexcept
{
    Pen pen{};
    if (!aSprite.GetMember("graphics", &pen.graphics) || !pen.graphics.IsObject())
        return;
    pen.graphics.Invoke("clear");
    pen.unit = kIconSize / 32.f;

    switch (aIcon)
    {
    case HudMarker::Icon::kClimb: DrawScanLines(pen, kClimbLines, 1.f); break;
    case HudMarker::Icon::kNoAp: DrawScanLines(pen, kClimbLines, kNoApAlpha); break;
    case HudMarker::Icon::kVault: DrawScanLines(pen, kVaultLines, 1.f); break;
    case HudMarker::Icon::kStep: DrawScanLines(pen, kStepLines, 1.f); break;
    case HudMarker::Icon::kTarget: DrawTarget(pen); break;
    case HudMarker::Icon::kNone: break;
    }
}

// Where a point on the screen (0..1) is in a display object's space: on the part of the stage that's on the screen (the
// stage is 1280 x 720; the movie shows more of it on a screen of another shape), then from the stage into the object
bool ScreenToObject(GFx::Movie* apMovie, GFx::Value aObject, float aX, float aY, double& aOutX, double& aOutY) noexcept
{
    float visible[4]{0.f, 0.f, 1280.f, 720.f};
    if (!apMovie->GetVisibleFrameRect(visible))
    {
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            spdlog::warn("Fluid Movement: the HUD's visible area isn't known, the marker takes the stage as the screen");
        }
    }
    aOutX = visible[0] + aX * (visible[2] - visible[0]);
    aOutY = visible[1] + aY * (visible[3] - visible[1]);

    const GFx::Value pointArgs[] = {GFx::Value(aOutX), GFx::Value(aOutY)};
    GFx::Value point;
    apMovie->CreateObject(&point, "flash.geom.Point", pointArgs, 2);
    GFx::Value local;
    if (!point.IsObject() || !aObject.Invoke("globalToLocal", &local, &point, 1) || !local.IsObject())
        return false;
    GFx::Value x;
    GFx::Value y;
    if (!local.GetMember("x", &x) || !local.GetMember("y", &y))
        return false;
    aOutX = x.GetNumber();
    aOutY = y.GetNumber();
    return true;
}

// From the HUD's advance: the marker as it's wanted, on the HUD
void Apply(IMenu* apHud) noexcept
{
    auto* pMovie = static_cast<GFx::Movie*>(apHud->pMovie);
    if (!pMovie)
        return;

    Wanted wanted;
    {
        std::scoped_lock _(s_wantedLock);
        wanted = s_wanted;
    }

    // made once per HUD movie (a new game or a load can make a new one)
    if (pMovie != s_sprite.pMovie)
    {
        // an old movie's target, were it still here, is never handed to this one
        s_sprite.pTarget = nullptr;
        if (wanted.icon == HudMarker::Icon::kNone)
            return;
        // the old movie's values belong to a movie that's gone
        s_sprite.sprite.Forget();
        s_sprite.root.Forget();
        s_sprite.pMovie = pMovie;
        s_sprite.drawn = HudMarker::Icon::kNone;
        if (!pMovie->GetVariable(&s_sprite.root, "root.CenterGroup_mc") || !s_sprite.root.IsObject())
            return;
        pMovie->CreateObject(&s_sprite.sprite, "flash.display.Sprite");
        if (!s_sprite.sprite.IsObject())
            return;
        s_sprite.sprite.SetMember("mouseEnabled", GFx::Value(false));
        s_sprite.sprite.SetMember("visible", GFx::Value(false));
        s_sprite.root.Invoke("addChild", nullptr, &s_sprite.sprite, 1);
        s_sprite.pTarget = MakeTarget(s_sprite.sprite);
        spdlog::info("Fluid Movement: HUD marker made, colour {:06X}", HudColour());
    }
    if (!s_sprite.sprite.IsObject())
        return;

    if (wanted.icon == HudMarker::Icon::kNone)
    {
        s_sprite.sprite.SetMember("visible", GFx::Value(false));
        return;
    }
    if (wanted.icon != s_sprite.drawn)
    {
        Draw(s_sprite.sprite, wanted.icon);
        s_sprite.drawn = wanted.icon;
    }
    double x = 0.;
    double y = 0.;
    if (!ScreenToObject(pMovie, s_sprite.root, wanted.x, wanted.y, x, y))
    {
        s_sprite.sprite.SetMember("visible", GFx::Value(false));
        return;
    }
    s_sprite.sprite.SetMember("x", GFx::Value(x));
    s_sprite.sprite.SetMember("y", GFx::Value(y));
    s_sprite.sprite.SetMember("scaleX", GFx::Value(static_cast<double>(wanted.scale)));
    s_sprite.sprite.SetMember("scaleY", GFx::Value(static_cast<double>(wanted.scale)));
    s_sprite.sprite.SetMember("alpha", GFx::Value(static_cast<double>(wanted.alpha)));
    s_sprite.sprite.SetMember("visible", GFx::Value(true));
}

// HUDMenu::AdvanceMovie (CommonLibF4 id 494688; IMenu's slot 4): the HUD's frame, captured for drawing at its end
TP_THIS_FUNCTION(THudAdvanceMovie, void, IMenu, float aDelta, uint64_t aTime);
THudAdvanceMovie* RealHudAdvanceMovie = nullptr;
IMenu* s_pHudMenu = nullptr;

void TP_MAKE_THISCALL(HookHudAdvanceMovie, IMenu, float aDelta, uint64_t aTime)
{
    static PerfProbe::HookStat s_perf("HudAdvanceMovie");
    PerfProbe::Hook _perf(s_perf);
    if (apThis)
    {
        s_pHudMenu = apThis;
        Apply(apThis);
        ForEachLayer([apThis](HudMarker::Layer& aLayer) { aLayer.Advance(apThis); });
    }
    PerfProbe::GameCall _game;
    TiltedPhoques::ThisCall(RealHudAdvanceMovie, apThis, aDelta, aTime);
}

// A shader target's slot 1: adds the rectangles it's drawn in (none while hidden)
void AppendTarget(uint8_t* apTarget, void* apBackground, void* apForeground) noexcept
{
    using TTargetAppend = void(void*, void*, void*);
    auto** pVTable = *reinterpret_cast<TTargetAppend***>(apTarget);
    pVTable[1](apTarget, apBackground, apForeground);
}

// GameMenuBase::AppendShaderFXInfos (CommonLibF4 id 583584): each of a menu's shader targets adds the rectangles it's
// drawn in (its slot 1); the HUD's also gets the marker's and our layers'
TP_THIS_FUNCTION(TAppendShaderFXInfos, void, IMenu, void* apBackground, void* apForeground);
TAppendShaderFXInfos* RealAppendShaderFXInfos = nullptr;

void TP_MAKE_THISCALL(HookAppendShaderFXInfos, IMenu, void* apBackground, void* apForeground)
{
    static PerfProbe::HookStat s_perf("HudShaderFX");
    PerfProbe::Hook _perf(s_perf);
    {
        PerfProbe::GameCall _game;
        TiltedPhoques::ThisCall(RealAppendShaderFXInfos, apThis, apBackground, apForeground);
    }
    if (apThis && apThis == s_pHudMenu)
    {
        if (s_sprite.pTarget && apThis->pMovie == s_sprite.pMovie)
            AppendTarget(s_sprite.pTarget, apBackground, apForeground);
        ForEachLayer([=](HudMarker::Layer& aLayer) { aLayer.AppendShaderFXInfos(apThis, apBackground, apForeground); });
    }
}

// HUDMenu's destructor (its vtable's slot 0, id 1126720; scalar deleting: this, flags): the marker goes with the HUD,
// before the HUD lets go of its movie
TP_THIS_FUNCTION(THudDestructor, void*, IMenu, uint32_t aFlags);
THudDestructor* RealHudDestructor = nullptr;

void* TP_MAKE_THISCALL(HookHudDestructor, IMenu, uint32_t aFlags)
{
    if (apThis)
    {
        if (s_sprite.pMovie && apThis->pMovie == s_sprite.pMovie)
            TearDown();
        ForEachLayer([apThis](HudMarker::Layer& aLayer) { aLayer.TearDown(apThis); });
        if (apThis == s_pHudMenu)
            s_pHudMenu = nullptr;
    }
    return TiltedPhoques::ThisCall(RealHudDestructor, apThis, aFlags);
}

// A shader target's rectangle on the screen (CommonLibF4 id 1352934): 0..1 across it (left, right, top, bottom). Two
// callers: the quads that put each target's part of the off screen HUD on the screen (id 1539756), right as they are,
// and the quads of the targets' shaded backings (id 1550461, its call returning at +0xFB). The game draws the backings
// as if the screen were a 16:9 area in its middle, while the movies themselves span all of a wider screen: there they
// land toward the middle, the further from it the further off (on a 3440 x 1440 screen, the marker's backing 490 of the
// stage's 1280 in, 100 pixels right of the marker). The backings' rectangles are put back under their elements; on a
// 16:9 screen or a narrower one they're left as the game has them.
using TShaderRect = float*(void*, float*);
TShaderRect* RealShaderRect = nullptr;

float* HookShaderRect(void* apTarget, float* apOut)
{
    float* pRect = RealShaderRect(apTarget, apOut);
    POINTER_FALLOUT4(uint8_t, s_appendBacking, 1550462);
    if (_ReturnAddress() != s_appendBacking.Get() + 0xFB)
        return pRect;
    // the target's movie, as the game finds it (its display object's value at 0x08: the object interface, whose movie
    // is at 0x08), and the movie's viewport (Movie's slot 0x0D; GFx's Viewport: the buffer's width and height first, 0x34
    // bytes in all)
    auto* pInterface = apTarget ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(apTarget) + 0x08) : nullptr;
    void* pMovie = pInterface ? *reinterpret_cast<void**>(pInterface + 0x08) : nullptr;
    if (!pRect || !pMovie)
        return pRect;
    alignas(16) int32_t viewport[16]{};
    using TGetViewport = void(const void*, int32_t*);
    (*reinterpret_cast<TGetViewport* const* const*>(pMovie))[0x0D](pMovie, viewport);
    const float width = static_cast<float>(viewport[0]);
    const float height = static_cast<float>(viewport[1]);
    const float wide = height * 16.f / 9.f;
    if (height <= 0.f || width <= wide + 1.f)
        return pRect;
    const float offset = (width - wide) * 0.5f;
    pRect[0] = (pRect[0] * width - offset) / wide;
    pRect[1] = (pRect[1] * width - offset) / wide;
    return pRect;
}

TiltedPhoques::Initializer s_hudMarkerHooks(
    []()
    {
        POINTER_FALLOUT4(THudAdvanceMovie, s_hudAdvanceMovie, 494689);
        RealHudAdvanceMovie = s_hudAdvanceMovie.Get();
        TP_HOOK(&RealHudAdvanceMovie, HookHudAdvanceMovie);

        POINTER_FALLOUT4(TAppendShaderFXInfos, s_appendShaderFXInfos, 583585);
        RealAppendShaderFXInfos = s_appendShaderFXInfos.Get();
        TP_HOOK(&RealAppendShaderFXInfos, HookAppendShaderFXInfos);

        POINTER_FALLOUT4(THudDestructor, s_hudDestructor, 1126721);
        RealHudDestructor = s_hudDestructor.Get();
        TP_HOOK(&RealHudDestructor, HookHudDestructor);

        POINTER_FALLOUT4(TShaderRect, s_shaderRect, 1352935);
        RealShaderRect = s_shaderRect.Get();
        TP_HOOK(&RealShaderRect, HookShaderRect);
    });
} // namespace

namespace
{
uint8_t* CameraRoot() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerCamera, 1171981);
    uint8_t* pCamera = *s_playerCamera.Get();
    return pCamera ? *reinterpret_cast<uint8_t**>(pCamera + kCameraRoot) : nullptr;
}

// The world camera's world to clip matrix, null when there's no camera
const float (*WorldToClip() noexcept)[4]
{
    uint8_t* pRoot = CameraRoot();
    if (!pRoot)
        return nullptr;

    uint8_t** pChildren = *reinterpret_cast<uint8_t***>(pRoot + kNodeChildren);
    const uint16_t capacity = *reinterpret_cast<uint16_t*>(pRoot + kNodeChildCapacity);
    for (uint16_t i = 0; pChildren && i < capacity; ++i)
    {
        if (pChildren[i] && Cast<NiCamera>(reinterpret_cast<NiAVObject*>(pChildren[i])))
            return reinterpret_cast<const float(*)[4]>(pChildren[i] + kWorldToClip);
    }
    return nullptr;
}

float MatrixRow(const float (*apMatrix)[4], int aRow, const NiPoint3& acPoint) noexcept
{
    return apMatrix[aRow][0] * acPoint.x + apMatrix[aRow][1] * acPoint.y + apMatrix[aRow][2] * acPoint.z + apMatrix[aRow][3];
}
} // namespace

bool HudMarker::WorldToScreen(const NiPoint3& acWorldPoint, float& aX, float& aY) noexcept
{
    const auto* m = WorldToClip();
    if (!m)
        return false;

    const float ahead = MatrixRow(m, 3, acWorldPoint);
    if (ahead < 1.f)
        return false;
    aX = (MatrixRow(m, 0, acWorldPoint) / ahead + 1.f) * 0.5f;
    aY = (1.f - MatrixRow(m, 1, acWorldPoint) / ahead) * 0.5f;
    return true;
}

bool HudMarker::ScreenToLocal(GFx::Movie* apMovie, const GFx::Value& acObject, float aX, float aY, double& aOutX, double& aOutY) noexcept
{
    return apMovie && acObject.IsObject() && ScreenToObject(apMovie, acObject, aX, aY, aOutX, aOutY);
}

bool HudMarker::WorldToView(const NiPoint3& acWorldPoint, float& aRight, float& aUp, float& aAhead) noexcept
{
    const auto* m = WorldToClip();
    if (!m)
        return false;

    // each row's direction is one of the camera's axes, scaled (right and up by the view's width and height)
    const auto length = [m](int aRow) { return std::max(std::sqrt(m[aRow][0] * m[aRow][0] + m[aRow][1] * m[aRow][1] + m[aRow][2] * m[aRow][2]), 1e-6f); };
    aRight = MatrixRow(m, 0, acWorldPoint) / length(0);
    aUp = MatrixRow(m, 1, acWorldPoint) / length(1);
    aAhead = MatrixRow(m, 3, acWorldPoint) / length(3);
    return true;
}

bool HudMarker::GetWorldToClip(float (&aOut)[4][4]) noexcept
{
    const auto* m = WorldToClip();
    if (!m)
        return false;
    std::memcpy(aOut, m, sizeof(aOut));
    return true;
}

bool HudMarker::GetCameraPosition(NiPoint3& aOut) noexcept
{
    const uint8_t* pRoot = CameraRoot();
    if (!pRoot)
        return false;
    const float* pEye = reinterpret_cast<const float*>(pRoot + 0xA0);
    aOut = NiPoint3(glm::vec3(pEye[0], pEye[1], pEye[2]));
    return true;
}

void HudMarker::AddLayer(Layer* apLayer) noexcept
{
    const size_t count = s_layerCount.load();
    if (!apLayer || count >= s_layers.size())
        return;
    s_layers[count] = apLayer;
    s_layerCount.store(count + 1);
}

uint8_t* HudMarker::MakeShaderTarget(const GFx::Value& acObject, int32_t aColourType, bool aBacking) noexcept
{
    return MakeTarget(acObject, aColourType, aBacking);
}

void HudMarker::SetShaderTargetColour(uint8_t* apTarget, uint32_t aColour) noexcept
{
    // BSGFxShaderFXTarget's CreateAndSetFiltersToColor(r, g, b, brightness) (CommonLibF4 id 783104): colour type 7, its
    // colour from the bytes (/ 255). The target's HUD colour events leave type 7 alone (its ProcessEvent, 848563)
    using TSetColour = void(void*, uint8_t, uint8_t, uint8_t, float);
    POINTER_FALLOUT4(TSetColour, s_setColour, 783105);
    if (apTarget)
        s_setColour.Get()(apTarget, static_cast<uint8_t>(aColour >> 16), static_cast<uint8_t>(aColour >> 8), static_cast<uint8_t>(aColour), 1.f);
}

void HudMarker::FreeShaderTarget(uint8_t* apTarget) noexcept
{
    FreeTarget(apTarget);
}

void HudMarker::AppendShaderTarget(uint8_t* apTarget, void* apBackground, void* apForeground) noexcept
{
    if (apTarget)
        AppendTarget(apTarget, apBackground, apForeground);
}

uint32_t HudMarker::GetHudColour() noexcept
{
    return HudColour();
}

void HudMarker::Show(Icon aIcon, const NiPoint3& acWorldPoint, float aDelta) noexcept
{
    float x = 0.f;
    float y = 0.f;
    if (aIcon == Icon::kNone || !WorldToScreen(acWorldPoint, x, y))
    {
        Hide();
        return;
    }

    float scale = 1.f;
    POINTER_FALLOUT4(uint8_t*, s_playerCamera, 1171981);
    if (uint8_t* pCamera = *s_playerCamera.Get(); pCamera)
    {
        if (uint8_t* pRoot = *reinterpret_cast<uint8_t**>(pCamera + kCameraRoot))
        {
            const float* pEye = reinterpret_cast<const float*>(pRoot + 0xA0);
            const float distance = std::hypot(acWorldPoint.x - pEye[0], acWorldPoint.y - pEye[1], acWorldPoint.z - pEye[2]);
            scale = std::clamp(kNearDistance / std::max(distance, 1.f), kMinScale, kMaxScale);
        }
    }

    s_shown = std::min(1.f, s_shown + aDelta / kFadeTime);
    s_pulse += aDelta * (aIcon == Icon::kNoAp ? kNoApPulseRate : kPulseRate);
    const float wave = 0.5f + 0.5f * std::cos(2.f * 3.14159265f * s_pulse);

    std::scoped_lock _(s_wantedLock);
    s_wanted.icon = aIcon;
    // a point off the screen in front (a ledge over the view, the closer the more often) keeps the marker at the edge
    // on its side
    s_wanted.x = std::clamp(x, kEdgeX, 1.f - kEdgeX);
    s_wanted.y = std::clamp(y, kEdgeY, 1.f - kEdgeY);
    s_wanted.scale = scale;
    s_wanted.alpha = s_shown * (kPulseLow + (1.f - kPulseLow) * wave);
}

void HudMarker::Hide() noexcept
{
    s_shown = 0.f;
    s_pulse = 0.f;
    std::scoped_lock _(s_wantedLock);
    s_wanted.icon = Icon::kNone;
}
