#pragma once

#include <cstdint>

struct ID3D11Device;
struct ID3D11DeviceContext;

// What the renderer features share. SunShadowCache.cpp owns the hooks on the shadow pass and calls the others from them;
// each feature has its own file, its own switch on the MCM page Performance, and turns itself off with a log line if the
// code it needs isn't what was read from 1.10.163. docs/RENDERING.md has what each step is in the game.
namespace ShadowFeatures
{
// Reclamation Day ids are CommonLibF4's + 1 (VersionDb::LoadFallout4)

// [Performance] on our MCM page; only read when something changes it (every few seconds at most)
uint32_t ReadSetting(const wchar_t* acpName, uint32_t aDefault) noexcept;

// The game's first instructions at an id, as read from 1.10.163; false with a log line when they differ
bool Matches(const char* acpFeature, const char* acpWhat, uint32_t aId, const uint8_t* acpBytes, size_t aCount) noexcept;

ID3D11Device* Device() noexcept;
ID3D11DeviceContext* Context() noexcept;
// The context the game draws with on this thread
ID3D11DeviceContext* ThreadContext() noexcept;
// Applies what the game has set but not yet told D3D11: render target, depth target, viewport, rasterizer, blend
void FlushRenderState() noexcept;
// The state the game is drawing with on this thread
uint8_t* GameRenderState() noexcept;

// Counts frames of the render pass
uint32_t FrameNumber() noexcept;

// --- the accumulate table: what the game does per object added to an accumulator, by the accumulator's mode. Swapping an
// entry needs no patch, and nothing runs when it's swapped back.
constexpr uint32_t kModeMain = 0x00;
constexpr uint32_t kModeDepth = 0x0D;
constexpr uint32_t kModeFrustum = 0x0F;
constexpr uint32_t kModeDirectional = 0x10;
constexpr uint32_t kModeParabolic = 0x11;
constexpr uint32_t kModeMainAfterDepth = 0x18;
using TRegister = uint64_t(void* apAccumulator, void* apGeometry, void* apProperty);
TRegister* SwapRegister(uint32_t aMode, TRegister* apNew, TRegister* apExpected) noexcept;

// --- the shadow register: small casters, local light fingerprints and GPU-driven shadows all look at a caster here
void RefreshShadowRegister() noexcept;
void RefreshMainRegister() noexcept;

// --- a cascade of the sun as the others need it
struct SunCascade
{
    void* pAccumulator = nullptr;
    void* pDynamicAccumulator = nullptr;
    void* pCamera = nullptr;
    float WorldPerTexel = 0.f;
};
constexpr uint32_t kMaxCascades = 3;
const SunCascade* SunCascades() noexcept;
uint32_t SunCascadeCount() noexcept;

// Which of the sun's cascades an accumulator is (static or moving), or -1
int CascadeOfAccumulator(const void* apAccumulator) noexcept;
} // namespace ShadowFeatures

// Small shadow caster culling (SmallCasterCull.cpp)
namespace SmallCasterCull
{
bool Enabled() noexcept;
void Refresh() noexcept;
// The caster is smaller than the threshold in this accumulator's map
bool ShouldSkip(void* apAccumulator, void* apGeometry) noexcept;
void Report(double aSeconds) noexcept;
} // namespace SmallCasterCull

// The depth of shadow maps kept in our own textures (ShadowDepthStore.cpp). D3D11 can only copy a depth texture's whole slice.
namespace ShadowDepthStore
{
// Copies between the depth slice bound now and a slot; each slot is one slice's worth of memory, made on first use
constexpr uint32_t kCascadeSlots = 2;
constexpr uint32_t kMaxSlots = 16 + kCascadeSlots;
bool Save(uint32_t aSlot) noexcept;
bool Restore(uint32_t aSlot) noexcept;
// Drops the textures (a new resolution, or a setting that needs fewer)
void Reset() noexcept;
} // namespace ShadowDepthStore

// Local light shadow caching (LocalLightCache.cpp)
namespace LocalLightCache
{
bool Enabled() noexcept;
void Refresh() noexcept;
void NoteCaster(void* apAccumulator, void* apGeometry, void* apProperty) noexcept;
// Instead of drawing the accumulator into a local light's map; true when it dealt with the draw. The caller's function does the real draw.
using TRender = void(void* apCamera, void* apAccumulator, void* apFlag);
bool OnRender(void* apCamera, void* apAccumulator, void* apFlag, TRender* apRender) noexcept;
void Report(double aSeconds) noexcept;
} // namespace LocalLightCache

// Occlusion culling (OcclusionCull.cpp)
namespace OcclusionCull
{
void Refresh() noexcept;
// Called with the main depth finished, before the shadows and the lighting
void OnDepthReady() noexcept;
// Called before the frame's objects are added, with last frame's depth to go by
void OnFrameStart() noexcept;
void Report(double aSeconds) noexcept;
} // namespace OcclusionCull

// GPU-driven shadows (GpuShadows.cpp)
namespace GpuShadows
{
bool Enabled() noexcept;
void Refresh() noexcept;
// Takes the caster if it can be drawn the merged way (the game doesn't see it then); false to leave it to the game
bool TryAdd(void* apAccumulator, void* apGeometry, void* apProperty) noexcept;
// Draws what was taken for this accumulator; the depth target is bound
void Draw(void* apAccumulator, void* apCamera) noexcept;
// A new frame's shadows start: anything gathered and never drawn is dropped
void BeginFrame() noexcept;
// A load: nothing kept of the meshes can be trusted any more
void Forget() noexcept;
void Report(double aSeconds) noexcept;
} // namespace GpuShadows

// More visibility workers (VisibilityWorkers.cpp)
namespace VisibilityWorkers
{
void Report(double aSeconds) noexcept;
} // namespace VisibilityWorkers
