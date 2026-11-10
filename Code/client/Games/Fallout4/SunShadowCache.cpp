#include <ShadowFeatures.h>
#include <Renderer.h>

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>

// Sun shadow caching. Every frame the game fits the sun's cascades to the view, gathers what casts a shadow in each, and
// draws each cascade's depth map, even when nothing in it changed. The cascades past the nearest are now kept between
// updates: on a frame that doesn't redraw one, its map and the matrix the game stored with it stay as they are, and the
// game's lighting goes on reading them (nothing else writes them). The nearest is drawn every frame. Of the others one a
// frame is due, so the work stays even instead of spiking; one the view has moved out of is drawn at once. A cascade that
// isn't redrawn also keeps the camera and viewport it was drawn with: the fit rewrites them every frame, and the volumetric
// light and the lighting's constants read them next to the kept map. See docs/RENDERING.md for what each step is in the game.
//
// This file also holds what the other renderer features share, since they hang off the same places in the pass: parallel
// gathering (the fit and the gathering run on a job from the start of the main draws) and static and dynamic separation (a kept
// cascade's static casters stay in a copy of its map, its moving casters are drawn over that every frame).

namespace
{
// Reclamation Day ids are CommonLibF4's + 1 (VersionDb::LoadFallout4)
constexpr uint32_t kUpdateCascades = 1242205; // BSShadowDirectionalLight, slot 0xE: fits the cascades' cameras to the view
constexpr uint32_t kGatherCasters = 1390076;  // gathers each cascade's casters into its accumulator
constexpr uint32_t kDrawMaps = 871922;        // BSShadowDirectionalLight, slot 0xA: draws the cascades in a bit mask
constexpr uint32_t kDrawMap = 1144069;        // draws one map of any shadow light
constexpr uint32_t kMainPass = 1108522;       // the frame's render pass, shadows and the main view (timed for the report)
constexpr uint32_t kRenderAccumulator = 1310229; // draws an accumulator with a camera, inside a map's draw
constexpr uint32_t kMainDraws = 56597;           // the frame's main draws start, the first thing after the main culling's done with the culler's flags
constexpr uint32_t kCollectCasters = 1142693;    // puts what can cast a shadow into a culler: the pre-culled statics, then what can move
constexpr uint32_t kRegisterTable = 1279939;     // what's done per object added to an accumulator, by the accumulator's mode
constexpr uint32_t kShadowRegister = 1071290;    // the entry for the shadow modes
constexpr uint32_t kFlushState = 1557285;        // applies what the game has set but not yet told D3D11
constexpr uint32_t kRenderStateMain = 33540;     // the main thread's render state when the thread has none of its own
constexpr uint32_t kTlsIndex = 842565;           // the exe's TLS slot
constexpr size_t kTlsRenderState = 0xB20;
constexpr size_t kTlsContext = 0xB18;
// The job the gathering runs on: the culling's own list, idle unless bCullingBatch is on
constexpr uint32_t kAddJob = 546315;
constexpr uint32_t kStartJobs = 970924;
constexpr uint32_t kWaitJobs = 266373;
constexpr uint32_t kCullManager = 1574717;
constexpr size_t kCullManagerList = 0x40;
constexpr uint32_t kCullingBatch = 938586;       // bCullingBatch's value
constexpr uint32_t kUsesPreCulled = 917970;      // the pre-culled objects are in use (a function)
constexpr uint32_t kSunShadowsOn = 241043;
constexpr uint32_t kShadowSceneNode = 879299;
constexpr uint32_t kMainShadowSceneNode = 1327070;
constexpr size_t kSceneNodeSun = 0x208;
constexpr uint32_t kMainCamera = 81407;
constexpr uint32_t kMainCuller = 1117783;
// An accumulator of our own for a cascade's moving casters
constexpr uint32_t kGameAllocate = 898180;
constexpr uint32_t kAccumulatorConstruct = 690953;
constexpr size_t kAccumulatorSize = 0x590;
constexpr size_t kAccumulatorRefCount = 8;
constexpr size_t kAccumulatorSceneNode = 0x558;
constexpr size_t kAccumulatorMode = 0x560;
constexpr size_t kAccumulatorCascade = 0x580;
// What the casters are collected from
constexpr uint32_t kPreCulledLock = 1151401;
constexpr uint32_t kPreCulledLockCount = 469766;
constexpr uint32_t kPreCulledIds = 446403;
constexpr uint32_t kPreCulledCount = 885457;
constexpr uint32_t kMovingCasters = 932020;
constexpr uint32_t kFileCount = 943706;
constexpr uint32_t kLookUpId = 1087701;
constexpr uint32_t kAddToCuller = 1175494;
constexpr uint32_t kTakeLock = 1425658;
constexpr size_t kObjectFlags = 0x108;
constexpr size_t kObjectBound = 0xB0;

// BSShadowLight: its maps
constexpr size_t kMapCount = 0x190;
constexpr size_t kMaps = 0x198;

// One map of a shadow light. The matrix is written only as the map is drawn; the lighting reads it from here.
struct ShadowMap
{
    float Matrix[16];    // 00
    void* pCamera;       // 40
    void* pAccumulator;  // 48
    int32_t Target;      // 50
    int32_t Slice;       // 54
    uint8_t Planes[0x78]; // 58, the cascade's split planes and more, as this frame's fit leaves them
    int32_t Viewport[4]; // D0
    uint8_t Rest[0x10];  // E0
};
static_assert(sizeof(ShadowMap) == 0xF0);
static_assert(offsetof(ShadowMap, Target) == 0x50);
static_assert(offsetof(ShadowMap, Viewport) == 0xD0);

// NiCamera: its world transform (rotation rows: forward, up, right; then the position) and its view box
constexpr size_t kCameraRight = 0x90;
constexpr size_t kCameraUp = 0x80;
constexpr size_t kCameraPosition = 0xA0;
constexpr size_t kCameraFrustum = 0x160; // left, right, top, bottom, ...
// What a kept cascade's camera goes back to after the fit: the world transform, and the world-to-camera matrix with the frustum
constexpr size_t kCameraWorld = 0x70;
constexpr size_t kCameraWorldSize = 0x40;
constexpr size_t kCameraView = 0x120;
constexpr size_t kCameraViewSize = 0x5C;

using ShadowFeatures::kMaxCascades;

// The light as the gathering reads it: copied for the length of a gathering so the real one is never touched
constexpr size_t kLightCopySize = 0x2D8;

struct Preset
{
    uint32_t Frames[kMaxCascades]; // a cascade is redrawn at least every this many frames
    float Slack;                   // how far the view may leave a kept map before it's redrawn, as a share of the map
};
constexpr Preset kOff{{1, 1, 1}, 0.f};
constexpr Preset kQuality{{1, 2, 4}, 0.01f};
constexpr Preset kBalanced{{1, 3, 6}, 0.02f};
constexpr Preset kPerformance{{1, 4, 8}, 0.03f};

// The settings MCM writes for our page, looked at this often (s)
constexpr wchar_t kSettingsFile[] = L"MCM\\Settings\\ReclamationDay.ini";
constexpr wchar_t kSettingsSection[] = L"Performance";
constexpr double kSettingsInterval = 3.0;
// A gap this long without the cascades being fitted (a load, an interior, a menu): everything is drawn again (s)
constexpr double kGapForRedraw = 0.25;
constexpr double kReportInterval = 30.0;
// Slices of the game's shadow texture array we track the last drawer of
constexpr uint32_t kTrackedSlices = 8;

using TUpdateCascades = void(void* apLight, void* apCamera);
using TGatherCasters = void(void* apLight, void* apCuller);
using TDrawMaps = void(void* apLight, uint32_t aMask);
using TDrawMap = void(void* apLight, ShadowMap* apMap);
using TMainPass = void(void* apFirst, void* apSecond, void* apThird, void* apFourth);
TUpdateCascades* RealUpdateCascades = nullptr;
TGatherCasters* RealGatherCasters = nullptr;
TDrawMaps* RealDrawMaps = nullptr;
TDrawMap* RealDrawMap = nullptr;
TMainPass* RealMainPass = nullptr;

using TRenderAccumulator = void(void* apCamera, void* apAccumulator, void* apFlag);
using TMainDraws = void();
using TCollectCasters = void*(void* apCuller);
TRenderAccumulator* RealRenderAccumulator = nullptr;
TMainDraws* RealMainDraws = nullptr;
TCollectCasters* RealCollectCasters = nullptr;

// What's kept of a drawn cascade: its matrix, the box of the map in what that matrix gives, and the camera and viewport it
// was drawn with
struct Kept
{
    bool Valid = false;
    uint32_t Age = 0;
    float Matrix[16]{};
    float MinX = 0.f, MaxX = 0.f, MinY = 0.f, MaxY = 0.f;
    uint8_t CameraWorld[kCameraWorldSize]{};
    uint8_t CameraView[kCameraViewSize]{};
    int32_t Viewport[4]{};
};

std::array<Kept, kMaxCascades> s_kept;
Preset s_preset = kBalanced;
const char* s_pPresetName = "Balanced";
bool s_enabled = true;
uint32_t s_mask = 0xFFFFFFFF;
bool s_armed = false;
const void* s_pMaps = nullptr;
uint32_t s_count = 0;
bool s_matrixByColumns = false;
bool s_matrixChecked = false;
bool s_matrixFailed = false;
const void* s_pDrawing = nullptr;

// What last drew each slice of the shadow texture: a point light's second map goes into the slice of the sun's second cascade
const void* s_sliceOwner[kTrackedSlices]{};
const void* s_pCurrentLight = nullptr;
ShadowMap* s_pCurrentMap = nullptr;

// What the features ask for
ShadowFeatures::SunCascade s_cascades[kMaxCascades]{};
uint32_t s_cascadeCount = 0;
uint32_t s_frame = 0;

// Static and dynamic separation
bool s_separateWanted = false;
bool s_separateBroken = false;
void* s_pDynamic[kMaxCascades]{};
bool s_staticStored[kMaxCascades]{};
enum class Collect
{
    Both,
    Statics,
    Moving,
};
thread_local Collect t_collect = Collect::Both;

// Parallel gathering
bool s_parallelWanted = false;
enum class ParallelState
{
    Idle,
    Running,
    Done,
};
ParallelState s_parallel = ParallelState::Idle;
struct ParallelJob
{
    void* pLight = nullptr;
    void* pCamera = nullptr;
    void* pCuller = nullptr;
};
ParallelJob s_job;

// Measured, reset at each report
struct Timing
{
    uint64_t Update = 0;
    uint64_t Gather = 0;
    uint64_t Maps[kMaxCascades]{};
    uint64_t OtherLights = 0;
    uint64_t MainPass = 0;
    uint64_t ParallelWait = 0;
    uint32_t ParallelFrames = 0;
    uint32_t SliceTaken = 0;
    uint32_t MapDraws[kMaxCascades]{};
    uint32_t OtherDraws = 0;
    uint32_t Frames = 0;
    uint32_t Forced = 0;
};
Timing s_timing;
LARGE_INTEGER s_frequency{};
LARGE_INTEGER s_lastFit{};
LARGE_INTEGER s_lastSettings{};
LARGE_INTEGER s_lastReport{};

uint64_t Now() noexcept
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<uint64_t>(now.QuadPart);
}

double Seconds(uint64_t aFrom, uint64_t aTo) noexcept
{
    return static_cast<double>(aTo - aFrom) / static_cast<double>(s_frequency.QuadPart);
}

template <class T> T& At(void* apObject, size_t aOffset) noexcept
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(apObject) + aOffset);
}

void LoadSettings() noexcept
{
    const auto path = (std::filesystem::current_path() / L"Data" / kSettingsFile).wstring();
    const UINT preset = GetPrivateProfileIntW(kSettingsSection, L"iSunShadowCache", 2, path.c_str());
    switch (preset)
    {
    case 0: s_preset = kOff; s_pPresetName = "Off"; break;
    case 1: s_preset = kQuality; s_pPresetName = "Quality"; break;
    case 3: s_preset = kPerformance; s_pPresetName = "Performance"; break;
    case 4:
        s_preset = kBalanced;
        s_pPresetName = "Custom";
        s_preset.Frames[1] = std::clamp<uint32_t>(GetPrivateProfileIntW(kSettingsSection, L"iSunShadowMiddleFrames", 3, path.c_str()), 1, 30);
        s_preset.Frames[2] = std::clamp<uint32_t>(GetPrivateProfileIntW(kSettingsSection, L"iSunShadowFarFrames", 6, path.c_str()), 1, 30);
        break;
    default: s_preset = kBalanced; s_pPresetName = "Balanced"; break;
    }
    s_enabled = preset != 0;
    s_parallelWanted = GetPrivateProfileIntW(kSettingsSection, L"bParallelSunGather", 0, path.c_str()) != 0;
    s_separateWanted = GetPrivateProfileIntW(kSettingsSection, L"bStaticDynamicShadows", 0, path.c_str()) != 0;
}

// The separation needs kept cascades and their own accumulators
bool Separated() noexcept
{
    return s_separateWanted && !s_separateBroken && s_enabled;
}

void* MakeDynamicAccumulator(uint32_t aCascade, const void* apSceneNode) noexcept
{
    using TAllocate = void*(size_t aSize);
    using TConstruct = void*(void* apAccumulator, uint32_t aKind);
    const VersionDbPtr<uint8_t> allocate(kGameAllocate);
    const VersionDbPtr<uint8_t> construct(kAccumulatorConstruct);
    void* pMemory = reinterpret_cast<TAllocate*>(allocate.Get())(kAccumulatorSize);
    if (!pMemory)
        return nullptr;
    void* pAccumulator = reinterpret_cast<TConstruct*>(construct.Get())(pMemory, 99);
    // never released, so the game never frees it
    *reinterpret_cast<int32_t*>(static_cast<uint8_t*>(pAccumulator) + kAccumulatorRefCount) = 1;
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pAccumulator) + kAccumulatorMode) = ShadowFeatures::kModeDirectional;
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(pAccumulator) + kAccumulatorCascade) = aCascade;
    *reinterpret_cast<const void**>(static_cast<uint8_t*>(pAccumulator) + kAccumulatorSceneNode) = apSceneNode;
    return pAccumulator;
}

// Where the matrix puts a world point
void Apply(const float* apMatrix, bool aByColumns, float aX, float aY, float aZ, float& arX, float& arY, float& arW) noexcept
{
    if (aByColumns)
    {
        arX = apMatrix[0] * aX + apMatrix[4] * aY + apMatrix[8] * aZ + apMatrix[12];
        arY = apMatrix[1] * aX + apMatrix[5] * aY + apMatrix[9] * aZ + apMatrix[13];
        arW = apMatrix[3] * aX + apMatrix[7] * aY + apMatrix[11] * aZ + apMatrix[15];
    }
    else
    {
        arX = apMatrix[0] * aX + apMatrix[1] * aY + apMatrix[2] * aZ + apMatrix[3];
        arY = apMatrix[4] * aX + apMatrix[5] * aY + apMatrix[6] * aZ + apMatrix[7];
        arW = apMatrix[12] * aX + apMatrix[13] * aY + apMatrix[14] * aZ + apMatrix[15];
    }
}

// The four corners of what a cascade's camera sees, in the world
void CameraCorners(void* apCamera, float (&arCorners)[4][3]) noexcept
{
    const auto* pRight = &At<float>(apCamera, kCameraRight);
    const auto* pUp = &At<float>(apCamera, kCameraUp);
    const auto* pPosition = &At<float>(apCamera, kCameraPosition);
    const auto* pBox = &At<float>(apCamera, kCameraFrustum);
    for (int corner = 0; corner < 4; ++corner)
    {
        const float across = pBox[corner & 1];
        const float upward = pBox[2 + (corner >> 1)];
        for (int axis = 0; axis < 3; ++axis)
            arCorners[corner][axis] = pPosition[axis] + pRight[axis] * across + pUp[axis] * upward;
    }
}

// As a cascade is drawn: its matrix, and where it puts the map's own corners
void Keep(Kept& arKept, const ShadowMap& acMap) noexcept
{
    if (!acMap.pCamera)
    {
        arKept.Valid = false;
        return;
    }
    std::memcpy(arKept.Matrix, acMap.Matrix, sizeof(arKept.Matrix));
    std::memcpy(arKept.CameraWorld, &At<uint8_t>(acMap.pCamera, kCameraWorld), kCameraWorldSize);
    std::memcpy(arKept.CameraView, &At<uint8_t>(acMap.pCamera, kCameraView), kCameraViewSize);
    std::memcpy(arKept.Viewport, acMap.Viewport, sizeof(arKept.Viewport));
    float corners[4][3];
    CameraCorners(acMap.pCamera, corners);

    // The stored order of the game's matrix, from what it does to its own map: an affine matrix keeps w at 1
    if (!s_matrixChecked)
    {
        s_matrixChecked = true;
        const auto spread = [&](bool aByColumns)
        {
            float worst = 0.f;
            for (const auto& corner : corners)
            {
                float x, y, w;
                Apply(arKept.Matrix, aByColumns, corner[0], corner[1], corner[2], x, y, w);
                worst = std::max(worst, std::abs(w - 1.f));
            }
            return worst;
        };
        const float byRows = spread(false);
        const float byColumns = spread(true);
        s_matrixByColumns = byColumns < byRows;
        s_matrixFailed = std::min(byRows, byColumns) > 0.01f;
        if (s_matrixFailed)
            spdlog::warn("Sun shadow cache: the cascade's matrix isn't what was expected (w off by {} / {}), everything is drawn every frame", byRows, byColumns);
        else
            spdlog::info("Sun shadow cache: the cascade's matrix is stored by {}", s_matrixByColumns ? "columns" : "rows");
    }

    float minX = FLT_MAX, maxX = -FLT_MAX, minY = FLT_MAX, maxY = -FLT_MAX;
    for (const auto& corner : corners)
    {
        float x, y, w;
        Apply(arKept.Matrix, s_matrixByColumns, corner[0], corner[1], corner[2], x, y, w);
        minX = std::min(minX, x);
        maxX = std::max(maxX, x);
        minY = std::min(minY, y);
        maxY = std::max(maxY, y);
    }
    arKept.MinX = minX;
    arKept.MaxX = maxX;
    arKept.MinY = minY;
    arKept.MaxY = maxY;
    arKept.Valid = true;
    arKept.Age = 0;
}

// Has this frame's fit of the cascade left what its kept map covers?
bool HasLeft(const Kept& acKept, void* apCamera, float aSlack) noexcept
{
    const float slackX = (acKept.MaxX - acKept.MinX) * aSlack;
    const float slackY = (acKept.MaxY - acKept.MinY) * aSlack;
    float corners[4][3];
    CameraCorners(apCamera, corners);
    for (const auto& corner : corners)
    {
        float x, y, w;
        Apply(acKept.Matrix, s_matrixByColumns, corner[0], corner[1], corner[2], x, y, w);
        if (x < acKept.MinX - slackX || x > acKept.MaxX + slackX || y < acKept.MinY - slackY || y > acKept.MaxY + slackY)
            return true;
    }
    return false;
}

void Report(uint64_t aNow) noexcept
{
    const double elapsed = Seconds(static_cast<uint64_t>(s_lastReport.QuadPart), aNow);
    if (elapsed < kReportInterval)
        return;
    s_lastReport.QuadPart = static_cast<LONGLONG>(aNow);

    const double frames = std::max<uint32_t>(s_timing.Frames, 1);
    const auto ms = [&](uint64_t aTicks) { return static_cast<double>(aTicks) * 1000.0 / static_cast<double>(s_frequency.QuadPart) / frames; };
    if (s_timing.Frames)
        spdlog::info("Shadows, ms per frame over {} frames ({}, a frame takes {:.1f}, the render pass {:.2f}): sun fit {:.2f}, gather {:.2f}, draw near {:.2f} (drawn {:.0f}% of frames), middle {:.2f} ({:.0f}%), far {:.2f} ({:.0f}%), "
                     "{} redrawn early; other lights {:.2f} ({:.1f} maps a frame)",
                     s_timing.Frames, s_pPresetName, elapsed * 1000.0 / frames, ms(s_timing.MainPass), ms(s_timing.Update), ms(s_timing.Gather), ms(s_timing.Maps[0]), s_timing.MapDraws[0] * 100.0 / frames, ms(s_timing.Maps[1]),
                     s_timing.MapDraws[1] * 100.0 / frames, ms(s_timing.Maps[2]), s_timing.MapDraws[2] * 100.0 / frames, s_timing.Forced, ms(s_timing.OtherLights),
                     s_timing.OtherDraws / frames);
    if (s_timing.ParallelFrames)
        spdlog::info("Parallel sun shadow gathering: on {} of {} frames, the main thread waited {:.2f} ms a frame for it", s_timing.ParallelFrames, s_timing.Frames, ms(s_timing.ParallelWait));
    if (s_timing.SliceTaken)
        spdlog::info("Sun shadow cache: another light drew over a kept cascade's slice {} times, those were redrawn", s_timing.SliceTaken);
    s_timing = {};

    SmallCasterCull::Report(elapsed);
    LocalLightCache::Report(elapsed);
    OcclusionCull::Report(elapsed);
    GpuShadows::Report(elapsed);
    VisibilityWorkers::Report(elapsed);
}

// Each cascade as the features need it; after the kept ones are back to the camera they were drawn with
void UpdateCascadeTable(void* apLight) noexcept
{
    const uint32_t count = At<uint32_t>(apLight, kMapCount);
    auto* pMaps = At<ShadowMap*>(apLight, kMaps);
    if (!pMaps || count > kMaxCascades)
    {
        s_cascadeCount = 0;
        return;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        auto& cascade = s_cascades[i];
        cascade.pAccumulator = pMaps[i].pAccumulator;
        cascade.pDynamicAccumulator = s_pDynamic[i];
        cascade.pCamera = pMaps[i].pCamera;
        const int32_t width = pMaps[i].Viewport[1] - pMaps[i].Viewport[0];
        const float* pFrustum = pMaps[i].pCamera ? &At<float>(pMaps[i].pCamera, kCameraFrustum) : nullptr;
        cascade.WorldPerTexel = pFrustum && width > 0 ? (pFrustum[1] - pFrustum[0]) / static_cast<float>(width) : 0.f;
    }
    s_cascadeCount = count;
}

// Which cascades this frame draws: the nearest, any the view has left, and otherwise the distant one most overdue
uint32_t Choose(void* apLight) noexcept
{
    const uint32_t count = At<uint32_t>(apLight, kMapCount);
    auto* pMaps = At<ShadowMap*>(apLight, kMaps);
    if (!s_enabled || s_matrixFailed || !pMaps || count == 0 || count > kMaxCascades)
        return 0xFFFFFFFF;

    if (pMaps != s_pMaps || count != s_count)
    {
        s_pMaps = pMaps;
        s_count = count;
        for (auto& kept : s_kept)
            kept.Valid = false;
    }

    uint32_t mask = 1;
    uint32_t forced = 0;
    int due = -1;
    float dueBy = 0.f;
    for (uint32_t i = 1; i < count; ++i)
    {
        Kept& kept = s_kept[i];
        ++kept.Age;
        // A kept map only stands if nothing else drew into its slice, or its statics are in the store
        const bool separated = Separated();
        if (kept.Valid && !separated)
        {
            const auto slice = static_cast<uint32_t>(pMaps[i].Slice);
            if (slice < kTrackedSlices && s_sliceOwner[slice] != &pMaps[i])
            {
                ++s_timing.SliceTaken;
                kept.Valid = false;
            }
        }
        if (!kept.Valid || !pMaps[i].pCamera || HasLeft(kept, pMaps[i].pCamera, s_preset.Slack) || (separated && !s_staticStored[i]))
        {
            forced |= 1u << i;
            continue;
        }
        const float overdue = static_cast<float>(kept.Age) / static_cast<float>(s_preset.Frames[i]);
        if (overdue >= 1.f && overdue > dueBy)
        {
            due = static_cast<int>(i);
            dueBy = overdue;
        }
    }
    if (forced)
    {
        s_timing.Forced += __popcnt(forced);
        return mask | forced;
    }
    if (due >= 0)
        mask |= 1u << due;
    return mask;
}

// The cascades this frame doesn't draw go back to the camera and viewport their kept map was drawn with
void RestoreKept(void* apLight, uint32_t aMask) noexcept
{
    const uint32_t count = At<uint32_t>(apLight, kMapCount);
    auto* pMaps = At<ShadowMap*>(apLight, kMaps);
    if (!pMaps || count > kMaxCascades)
        return;
    for (uint32_t i = 0; i < count; ++i)
    {
        const Kept& kept = s_kept[i];
        if (((aMask >> i) & 1) || !kept.Valid || !pMaps[i].pCamera)
            continue;
        std::memcpy(&At<uint8_t>(pMaps[i].pCamera, kCameraWorld), kept.CameraWorld, kCameraWorldSize);
        std::memcpy(&At<uint8_t>(pMaps[i].pCamera, kCameraView), kept.CameraView, kCameraViewSize);
        std::memcpy(pMaps[i].Viewport, kept.Viewport, sizeof(kept.Viewport));
    }
}

// The fit: on the main thread in the pass as the game does it, or on the job
void DoFit(void* apLight, void* apCamera) noexcept
{
    const uint64_t start = Now();
    if (Seconds(static_cast<uint64_t>(s_lastFit.QuadPart), start) >= kGapForRedraw)
    {
        for (auto& kept : s_kept)
            kept.Valid = false;
        for (auto& stored : s_staticStored)
            stored = false;
        GpuShadows::Forget();
    }
    s_lastFit.QuadPart = static_cast<LONGLONG>(start);
    GpuShadows::BeginFrame();

    RealUpdateCascades(apLight, apCamera);

    s_mask = Choose(apLight);
    RestoreKept(apLight, s_mask);
    UpdateCascadeTable(apLight);
    s_armed = true;
    ++s_timing.Frames;

    s_timing.Update += Now() - start;
}

// One run of the game's gathering over these maps, on a copy of the light that lists only them
void GatherFor(const void* apLight, void* apCuller, ShadowMap* apMaps, uint32_t aCount) noexcept
{
    alignas(16) uint8_t lightCopy[kLightCopySize];
    std::memcpy(lightCopy, apLight, sizeof(lightCopy));
    At<uint32_t>(lightCopy, kMapCount) = aCount;
    At<ShadowMap*>(lightCopy, kMaps) = apMaps;
    RealGatherCasters(lightCopy, apCuller);
}

void DoGather(void* apLight, void* apCuller) noexcept
{
    const uint64_t start = Now();
    const uint32_t count = At<uint32_t>(apLight, kMapCount);
    auto* pMaps = At<ShadowMap*>(apLight, kMaps);
    ShadowMap selected[kMaxCascades];

    if (Separated() && s_armed && pMaps && count > 1 && count <= kMaxCascades && RealCollectCasters)
    {
        // The nearest is drawn every frame, as it was; of the others, statics only when they're redrawn, what can move always
        std::memcpy(&selected[0], &pMaps[0], sizeof(ShadowMap));
        GatherFor(apLight, apCuller, selected, 1);

        uint32_t used = 0;
        for (uint32_t i = 1; i < count; ++i)
        {
            if ((s_mask >> i) & 1)
                std::memcpy(&selected[used++], &pMaps[i], sizeof(ShadowMap));
        }
        if (used)
        {
            t_collect = Collect::Statics;
            GatherFor(apLight, apCuller, selected, used);
        }

        used = 0;
        for (uint32_t i = 1; i < count; ++i)
        {
            if (!s_pDynamic[i])
                continue;
            std::memcpy(&selected[used], &pMaps[i], sizeof(ShadowMap));
            selected[used].pAccumulator = s_pDynamic[i];
            ++used;
        }
        if (used)
        {
            t_collect = Collect::Moving;
            GatherFor(apLight, apCuller, selected, used);
        }
        t_collect = Collect::Both;
    }
    else
    {
        uint32_t chosen = 0;
        if (s_armed && pMaps && count <= kMaxCascades)
        {
            for (uint32_t i = 0; i < count; ++i)
                chosen += (s_mask >> i) & 1;
        }

        if (chosen == 0 || chosen == count || !pMaps)
        {
            RealGatherCasters(apLight, apCuller);
        }
        else
        {
            // It goes through the light's list of maps: for this call the list is only those it should gather for. These are
            // copies for the length of the call, the map data it reads and the accumulators it fills through them are the real ones.
            uint32_t used = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                if ((s_mask >> i) & 1)
                    std::memcpy(&selected[used++], &pMaps[i], sizeof(ShadowMap));
            }
            GatherFor(apLight, apCuller, selected, used);
        }
    }
    s_timing.Gather += Now() - start;
}

void* CullList() noexcept
{
    const VersionDbPtr<uint8_t*> manager(kCullManager);
    uint8_t* pManager = *manager.Get();
    return pManager ? pManager + kCullManagerList : nullptr;
}

void WaitForJob() noexcept
{
    if (s_parallel != ParallelState::Running)
        return;
    using TWait = void(void* apList);
    const VersionDbPtr<uint8_t> wait(kWaitJobs);
    const uint64_t start = Now();
    reinterpret_cast<TWait*>(wait.Get())(CullList());
    s_timing.ParallelWait += Now() - start;
    s_parallel = ParallelState::Done;
}

void RunParallelJob(void* apArgument)
{
    auto* pJob = static_cast<ParallelJob*>(apArgument);
    DoFit(pJob->pLight, pJob->pCamera);
    DoGather(pJob->pLight, pJob->pCuller);
}

// The main culling is done and the main draws are about to start: the fit and the gathering can run beside them. The game's
// own culling job list carries it; it's what its bCullingBatch path uses for the same work.
void HookMainDraws()
{
    if (s_parallel != ParallelState::Idle)
    {
        WaitForJob();
        s_parallel = ParallelState::Idle;
    }

    if (s_parallelWanted)
    {
        using TUsesPreCulled = bool();
        const VersionDbPtr<uint8_t> usesPreCulled(kUsesPreCulled);
        const VersionDbPtr<uint8_t> cullingBatch(kCullingBatch);
        const VersionDbPtr<uint8_t> sunShadowsOn(kSunShadowsOn);
        const VersionDbPtr<uint8_t*> sceneNode(kShadowSceneNode);
        const VersionDbPtr<uint8_t*> mainSceneNode(kMainShadowSceneNode);
        const VersionDbPtr<void*> camera(kMainCamera);
        const VersionDbPtr<uint8_t> culler(kMainCuller);
        void* pList = CullList();

        void* pLight = *sceneNode.Get() ? At<void*>(*sceneNode.Get(), kSceneNodeSun) : nullptr;
        // Only where the game would do this itself in the pass, on the path it's safe for
        if (pList && pLight && *sunShadowsOn.Get() && !*cullingBatch.Get() && *sceneNode.Get() == *mainSceneNode.Get() && reinterpret_cast<TUsesPreCulled*>(usesPreCulled.Get())())
        {
            using TAdd = void(void* apList, void (*apJob)(void*), void* apArgument, void* apNothing);
            using TStart = void(void* apList, void* apNothing, uint8_t aNothing);
            using TWait = void(void* apList);
            const VersionDbPtr<uint8_t> add(kAddJob);
            const VersionDbPtr<uint8_t> start(kStartJobs);
            const VersionDbPtr<uint8_t> wait(kWaitJobs);

            s_job = {pLight, *camera.Get(), culler.Get()};
            reinterpret_cast<TWait*>(wait.Get())(pList);
            reinterpret_cast<TAdd*>(add.Get())(pList, &RunParallelJob, &s_job, nullptr);
            reinterpret_cast<TStart*>(start.Get())(pList, nullptr, 0);
            s_parallel = ParallelState::Running;
            ++s_timing.ParallelFrames;
        }
    }

    RealMainDraws();
}

// What goes into the culler. Both is the game's own; the other two are its two loops, apart.
void* HookCollectCasters(void* apCuller)
{
    if (t_collect == Collect::Both)
        return RealCollectCasters(apCuller);

    using TLookUp = uint8_t*(uint32_t aId);
    using TAdd = void(void* apCuller, void* apObject, void* apBound, uint64_t aFlags);
    const VersionDbPtr<uint8_t> lookUpFunction(kLookUpId);
    const VersionDbPtr<uint8_t> addFunction(kAddToCuller);
    auto* lookUp = reinterpret_cast<TLookUp*>(lookUpFunction.Get());
    auto* add = reinterpret_cast<TAdd*>(addFunction.Get());
    constexpr uint64_t kHidden = 1;
    constexpr uint64_t kNoShadow = 1ull << 0x28;

    if (t_collect == Collect::Statics)
    {
        using TTakeLock = void(void* apLock, uint64_t aNothing);
        const VersionDbPtr<uint8_t> takeLock(kTakeLock);
        const VersionDbPtr<uint8_t> lock(kPreCulledLock);
        const VersionDbPtr<uint32_t> lockCount(kPreCulledLockCount);
        const VersionDbPtr<uint32_t*> ids(kPreCulledIds);
        const VersionDbPtr<uint32_t> idCount(kPreCulledCount);
        const VersionDbPtr<uint8_t> files(kFileCount);

        reinterpret_cast<TTakeLock*>(takeLock.Get())(lock.Get(), 0);
        const auto addOne = [&](uint32_t aId)
        {
            uint8_t* pObject = lookUp(aId);
            if (!pObject)
                return;
            const uint64_t flags = At<uint64_t>(pObject, kObjectFlags);
            if (!(flags & kHidden) && !(flags & kNoShadow))
                add(apCuller, pObject, pObject + kObjectBound, 0);
        };
        const uint32_t* pIds = *ids.Get();
        const uint32_t count = pIds ? *idCount.Get() : 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t id = pIds[i];
            if ((id >> 24) == 0 || (id >> 24) == 0xFD)
            {
                addOne(id);
            }
            else
            {
                // an id for any of the loaded files
                const uint32_t last = *files.Get();
                for (uint32_t file = 1; file <= last; ++file)
                {
                    if (file != 0xFD)
                        addOne((id & 0xFFFFFF) | (file << 24));
                }
            }
        }
        // the game's own unlock
        uint32_t& held = *lockCount.Get();
        if (held == 1)
        {
            *reinterpret_cast<uint32_t*>(lock.Get()) = 0;
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&held), 0);
        }
        else
        {
            InterlockedDecrement(reinterpret_cast<volatile LONG*>(&held));
        }
        return nullptr;
    }

    // What can move: a list of chunks (0x2000 bytes of object and flags, then the next chunk)
    const VersionDbPtr<uint8_t*> table(kMovingCasters);
    uint8_t* pTable = *table.Get();
    if (!pTable)
        return nullptr;
    constexpr size_t kChunk = 0x2000;
    auto* pWrite = At<uint64_t*>(pTable, 0x28);
    auto* pFirst = At<uint64_t*>(pTable, 0x30);
    auto* pLastChunk = At<uint8_t*>(pTable, 0x18);
    uint64_t* pEntry;
    uint8_t* pChunk;
    if (pFirst == pWrite)
    {
        pEntry = pWrite;
        if (pLastChunk && reinterpret_cast<uint8_t*>(pWrite) >= pLastChunk + kChunk && At<uint64_t*>(pLastChunk, kChunk))
            pEntry = At<uint64_t*>(pLastChunk, kChunk);
        pChunk = nullptr;
    }
    else
    {
        pChunk = At<uint8_t*>(pTable, 0x08);
        pEntry = pChunk ? pFirst : nullptr;
    }
    uint64_t* pEnd = pWrite;
    if (pLastChunk && reinterpret_cast<uint8_t*>(pWrite) >= pLastChunk + kChunk && At<uint64_t*>(pLastChunk, kChunk))
        pEnd = At<uint64_t*>(pLastChunk, kChunk);
    while (pEntry != pEnd)
    {
        auto* pObject = reinterpret_cast<uint8_t*>(pEntry[0]);
        if (!(At<uint64_t>(pObject, kObjectFlags) & kNoShadow))
            add(apCuller, pObject, pObject + kObjectBound, static_cast<uint32_t>(pEntry[1]));
        pEntry += 2;
        if (pChunk && reinterpret_cast<uint8_t*>(pEntry) >= pChunk + kChunk)
        {
            auto* pNext = At<uint8_t*>(pChunk, kChunk);
            if (pNext)
            {
                pChunk = pNext;
                pEntry = reinterpret_cast<uint64_t*>(pNext);
            }
        }
    }
    return nullptr;
}

void HookUpdateCascades(void* apLight, void* apCamera)
{
    // The job's doing this frame's, or has done it
    if (s_parallel != ParallelState::Idle)
    {
        WaitForJob();
        return;
    }
    DoFit(apLight, apCamera);
}

void HookGatherCasters(void* apLight, void* apCuller)
{
    if (s_parallel != ParallelState::Idle)
    {
        WaitForJob();
        s_parallel = ParallelState::Idle;
        return;
    }
    DoGather(apLight, apCuller);
}

void HookDrawMaps(void* apLight, uint32_t aMask)
{
    if (s_parallel != ParallelState::Idle)
    {
        WaitForJob();
        s_parallel = ParallelState::Idle;
    }

    const uint32_t count = At<uint32_t>(apLight, kMapCount);
    auto* pMaps = At<ShadowMap*>(apLight, kMaps);

    // With statics kept apart every cascade is drawn every frame: the kept ones from their copy, plus what moves
    const bool separated = Separated() && s_armed && pMaps && count <= kMaxCascades;
    if (separated)
    {
        const VersionDbPtr<uint8_t*> sceneNode(kShadowSceneNode);
        for (uint32_t i = 1; i < count; ++i)
        {
            if (!s_pDynamic[i])
                s_pDynamic[i] = MakeDynamicAccumulator(i, *sceneNode.Get());
        }
        UpdateCascadeTable(apLight);
    }

    const uint32_t mask = s_armed ? aMask & s_mask : aMask;
    s_armed = false;

    s_pDrawing = apLight;
    RealDrawMaps(apLight, separated ? aMask : mask);
    s_pDrawing = nullptr;

    if (!pMaps || count > kMaxCascades)
        return;
    for (uint32_t i = 0; i < count; ++i)
    {
        if ((mask >> i) & 1)
            Keep(s_kept[i], pMaps[i]);
    }
}

// Drawing an accumulator inside a map's draw: where the features replace or add to the draw calls
void HookRenderAccumulator(void* apCamera, void* apAccumulator, void* apFlag)
{
    ShadowMap* pMap = s_pCurrentMap;
    if (!pMap || apAccumulator != pMap->pAccumulator || !apCamera)
    {
        RealRenderAccumulator(apCamera, apAccumulator, apFlag);
        return;
    }

    if (s_pCurrentLight == s_pDrawing)
    {
        const int32_t cascade = static_cast<int32_t>(pMap - At<ShadowMap*>(const_cast<void*>(s_pDrawing), kMaps));
        void* pDynamic = cascade >= 1 && cascade < static_cast<int32_t>(kMaxCascades) ? s_pDynamic[cascade] : nullptr;
        if (Separated() && pDynamic && static_cast<uint32_t>(cascade) <= ShadowDepthStore::kCascadeSlots)
        {
            const uint32_t slot = static_cast<uint32_t>(cascade) - 1;
            if ((s_mask >> cascade) & 1)
            {
                // Redrawn: statics, kept, then what moves on top
                RealRenderAccumulator(apCamera, apAccumulator, apFlag);
                GpuShadows::Draw(apAccumulator, apCamera);
                s_staticStored[cascade] = ShadowDepthStore::Save(slot);
                if (!s_staticStored[cascade])
                {
                    s_separateBroken = true;
                    spdlog::warn("Static and dynamic shadows: off, a cascade's map couldn't be kept");
                }
            }
            else
            {
                // Kept: nothing was gathered for it, the camera is the kept one; statics from the copy (no camera: only finishes the accumulator)
                RealRenderAccumulator(nullptr, apAccumulator, apFlag);
                if (!ShadowDepthStore::Restore(slot))
                {
                    s_staticStored[cascade] = false;
                    s_kept[cascade].Valid = false;
                }
            }
            RealRenderAccumulator(apCamera, pDynamic, apFlag);
            GpuShadows::Draw(pDynamic, apCamera);
            return;
        }

        RealRenderAccumulator(apCamera, apAccumulator, apFlag);
        GpuShadows::Draw(apAccumulator, apCamera);
        return;
    }

    if (!LocalLightCache::OnRender(apCamera, apAccumulator, apFlag, RealRenderAccumulator))
        RealRenderAccumulator(apCamera, apAccumulator, apFlag);
}

void HookDrawMap(void* apLight, ShadowMap* apMap)
{
    const uint64_t start = Now();
    s_pCurrentLight = apLight;
    s_pCurrentMap = apMap;
    RealDrawMap(apLight, apMap);
    s_pCurrentMap = nullptr;
    s_pCurrentLight = nullptr;
    const uint64_t ticks = Now() - start;

    if (apMap && static_cast<uint32_t>(apMap->Slice) < kTrackedSlices)
        s_sliceOwner[apMap->Slice] = apMap;

    if (apLight == s_pDrawing)
    {
        const auto index = static_cast<size_t>(apMap - At<ShadowMap*>(apLight, kMaps));
        if (index < kMaxCascades)
        {
            s_timing.Maps[index] += ticks;
            ++s_timing.MapDraws[index];
            return;
        }
    }
    s_timing.OtherLights += ticks;
    ++s_timing.OtherDraws;
}

void RefreshFeatures() noexcept
{
    LoadSettings();
    SmallCasterCull::Refresh();
    LocalLightCache::Refresh();
    GpuShadows::Refresh();
    OcclusionCull::Refresh();
    ShadowFeatures::RefreshShadowRegister();
    ShadowFeatures::RefreshMainRegister();
    if (!Separated())
    {
        for (auto& stored : s_staticStored)
            stored = false;
    }
    // Its own line when it goes on or off (the 11-06 run had it on with nothing in the log to say so)
    static bool s_separatedWas = false;
    if (Separated() != s_separatedWas)
    {
        s_separatedWas = Separated();
        spdlog::info("Static and dynamic shadows: {}", s_separatedWas ? "on (a kept cascade's statics come back from the store, what moves is drawn on top)" : "off");
    }
}

void HookMainPass(void* apFirst, void* apSecond, void* apThird, void* apFourth)
{
    const uint64_t start = Now();
    ++s_frame;
    if (Seconds(static_cast<uint64_t>(s_lastSettings.QuadPart), start) >= kSettingsInterval)
    {
        s_lastSettings.QuadPart = static_cast<LONGLONG>(start);
        RefreshFeatures();
    }

    RealMainPass(apFirst, apSecond, apThird, apFourth);

    // This frame's main depth, read with this frame's camera: done drawing, both are of the same frame (read before the pass, the
    // depth was the last frame's and the camera maybe this one's)
    OcclusionCull::OnDepthReady();

    const uint64_t end = Now();
    s_timing.MainPass += end - start;
    Report(end);
}

// --- what's shared

ShadowFeatures::TRegister* s_realShadowRegister[3]{};
constexpr uint32_t kShadowModes[3] = {ShadowFeatures::kModeFrustum, ShadowFeatures::kModeDirectional, ShadowFeatures::kModeParabolic};
bool s_shadowRegisterSwapped = false;

// A caster reaches a shadow map's accumulator
uint64_t CommonShadowRegister(void* apAccumulator, void* apGeometry, void* apProperty, ShadowFeatures::TRegister* apReal)
{
    if (SmallCasterCull::Enabled() && SmallCasterCull::ShouldSkip(apAccumulator, apGeometry))
        return 1;
    if (ShadowFeatures::CascadeOfAccumulator(apAccumulator) >= 0)
    {
        if (GpuShadows::Enabled() && GpuShadows::TryAdd(apAccumulator, apGeometry, apProperty))
            return 1;
    }
    else if (LocalLightCache::Enabled())
    {
        LocalLightCache::NoteCaster(apAccumulator, apGeometry, apProperty);
    }
    return apReal(apAccumulator, apGeometry, apProperty);
}
template <int Index> uint64_t HookShadowRegister(void* apAccumulator, void* apGeometry, void* apProperty)
{
    return CommonShadowRegister(apAccumulator, apGeometry, apProperty, s_realShadowRegister[Index]);
}

template <class T> bool Find(uint32_t aId, T*& arReal, std::initializer_list<uint8_t> aStart, const char* acpName) noexcept
{
    const VersionDbPtr<uint8_t> function(aId);
    const uint8_t* pCode = function.Get();
    if (!pCode || std::memcmp(pCode, aStart.begin(), aStart.size()) != 0)
    {
        spdlog::warn("Sun shadow cache: off, {} isn't what's expected there (or another mod patched it)", acpName);
        return false;
    }
    arReal = reinterpret_cast<T*>(function.Get());
    return true;
}
} // namespace

static TiltedPhoques::Initializer s_sunShadowCacheHooks(
    []()
    {
        QueryPerformanceFrequency(&s_frequency);

        // Each function's first instructions, as read from 1.10.163
        if (!Find(kUpdateCascades, RealUpdateCascades, {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x53, 0x10, 0x55, 0x41, 0x57}, "the cascade fit") ||
            !Find(kGatherCasters, RealGatherCasters, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20}, "the gathering") ||
            !Find(kDrawMaps, RealDrawMaps, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20}, "the cascades' draw") ||
            !Find(kDrawMap, RealDrawMap, {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57}, "a map's draw"))
            return;

        LoadSettings();
        QueryPerformanceCounter(&s_lastReport);

        TP_HOOK(&RealUpdateCascades, HookUpdateCascades);
        TP_HOOK(&RealGatherCasters, HookGatherCasters);
        TP_HOOK(&RealDrawMaps, HookDrawMaps);
        TP_HOOK(&RealDrawMap, HookDrawMap);
        if (Find(kMainPass, RealMainPass, {0x48, 0x8B, 0xC4, 0x55, 0x48, 0x8D, 0xA8, 0x88, 0xF9, 0xFF, 0xFF}, "the render pass"))
            TP_HOOK(&RealMainPass, HookMainPass);

        // What the other features hang off. Each one that's missing only turns off what needs it, with its line in the log.
        if (Find(kRenderAccumulator, RealRenderAccumulator, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xDA}, "an accumulator's draw"))
            TP_HOOK(&RealRenderAccumulator, HookRenderAccumulator);
        if (Find(kMainDraws, RealMainDraws, {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18}, "the main draws"))
            TP_HOOK(&RealMainDraws, HookMainDraws);
        if (Find(kCollectCasters, RealCollectCasters, {0x40, 0x53, 0x55, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x40, 0x48, 0x8B, 0xE9}, "the casters' collection"))
            TP_HOOK(&RealCollectCasters, HookCollectCasters);
        else
            s_separateBroken = true;
        spdlog::info("Sun shadow cache: {}", s_enabled ? "on" : "off (MCM setting), still measuring");
    });

ID3D11Device* ShadowFeatures::Device() noexcept
{
    auto* pRenderer = BGSRenderer::Get();
    return pRenderer ? pRenderer->pD3dDevice : nullptr;
}

ID3D11DeviceContext* ShadowFeatures::Context() noexcept
{
    auto* pRenderer = BGSRenderer::Get();
    return pRenderer ? pRenderer->pD3dContext : nullptr;
}

ID3D11DeviceContext* ShadowFeatures::ThreadContext() noexcept
{
    const VersionDbPtr<uint32_t> tlsIndex(kTlsIndex);
    auto* const* pTls = reinterpret_cast<uint8_t* const*>(__readgsqword(0x58));
    return At<ID3D11DeviceContext*>(pTls[*tlsIndex.Get()], kTlsContext);
}

uint8_t* ShadowFeatures::GameRenderState() noexcept
{
    const VersionDbPtr<uint32_t> tlsIndex(kTlsIndex);
    auto* const* pTls = reinterpret_cast<uint8_t* const*>(__readgsqword(0x58));
    uint8_t* pState = At<uint8_t*>(pTls[*tlsIndex.Get()], kTlsRenderState);
    if (pState)
        return pState;
    const VersionDbPtr<uint8_t*> main(kRenderStateMain);
    return *main.Get();
}

void ShadowFeatures::FlushRenderState() noexcept
{
    using TFlush = void(uint8_t aOnlyTargets, uint8_t aNothing);
    const VersionDbPtr<uint8_t> flush(kFlushState);
    reinterpret_cast<TFlush*>(flush.Get())(1, 0);
}

uint32_t ShadowFeatures::FrameNumber() noexcept
{
    return s_frame;
}

uint32_t ShadowFeatures::ReadSetting(const wchar_t* acpName, uint32_t aDefault) noexcept
{
    const auto path = (std::filesystem::current_path() / L"Data" / kSettingsFile).wstring();
    return GetPrivateProfileIntW(kSettingsSection, acpName, aDefault, path.c_str());
}

bool ShadowFeatures::Matches(const char* acpFeature, const char* acpWhat, uint32_t aId, const uint8_t* acpBytes, size_t aCount) noexcept
{
    const VersionDbPtr<uint8_t> function(aId);
    const uint8_t* pCode = function.Get();
    if (!pCode || std::memcmp(pCode, acpBytes, aCount) != 0)
    {
        spdlog::warn("{}: off, {} isn't what's expected there (or another mod patched it)", acpFeature, acpWhat);
        return false;
    }
    return true;
}

ShadowFeatures::TRegister* ShadowFeatures::SwapRegister(uint32_t aMode, TRegister* apNew, TRegister* apExpected) noexcept
{
    const VersionDbPtr<TRegister*> table(kRegisterTable);
    TRegister** pEntry = table.Get() + aMode;
    if (*pEntry != apExpected)
        return nullptr;
    *pEntry = apNew;
    return apExpected;
}

void ShadowFeatures::RefreshShadowRegister() noexcept
{
    static bool s_broken = false;
    if (s_broken)
        return;

    const bool wanted = SmallCasterCull::Enabled() || LocalLightCache::Enabled() || GpuShadows::Enabled();
    if (wanted == s_shadowRegisterSwapped)
        return;

    const VersionDbPtr<uint8_t> real(kShadowRegister);
    auto* pReal = reinterpret_cast<TRegister*>(real.Get());
    static TRegister* const kHooks[3] = {&HookShadowRegister<0>, &HookShadowRegister<1>, &HookShadowRegister<2>};
    for (int i = 0; i < 3; ++i)
    {
        if (wanted)
        {
            s_realShadowRegister[i] = pReal;
            if (!SwapRegister(kShadowModes[i], kHooks[i], pReal))
            {
                spdlog::warn("Shadow casters' accumulate entry {} isn't the game's, the features that look at casters are off", i);
                s_broken = true;
                for (int j = 0; j < i; ++j)
                    SwapRegister(kShadowModes[j], pReal, kHooks[j]);
                return;
            }
        }
        else
        {
            SwapRegister(kShadowModes[i], pReal, kHooks[i]);
        }
    }
    s_shadowRegisterSwapped = wanted;
}

const ShadowFeatures::SunCascade* ShadowFeatures::SunCascades() noexcept
{
    return s_cascades;
}

uint32_t ShadowFeatures::SunCascadeCount() noexcept
{
    return s_cascadeCount;
}

int ShadowFeatures::CascadeOfAccumulator(const void* apAccumulator) noexcept
{
    for (uint32_t i = 0; i < s_cascadeCount; ++i)
    {
        if (s_cascades[i].pAccumulator == apAccumulator || (s_cascades[i].pDynamicAccumulator && s_cascades[i].pDynamicAccumulator == apAccumulator))
            return static_cast<int>(i);
    }
    return -1;
}
