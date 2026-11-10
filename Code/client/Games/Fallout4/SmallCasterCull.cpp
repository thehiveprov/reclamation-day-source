#include <ShadowFeatures.h>

#include <atomic>
#include <cmath>

// Small shadow caster culling. A caster that covers less than a few texels of a shadow map adds a draw call and leaves a
// speck at most. When a caster reaches the accumulate step of a shadow map, its bound is compared to the size of a texel
// there. This changes the picture a little (the specks go), so it's a choice: off until asked for, the threshold in texels.
// Nothing runs when it's 0: the accumulate table's entries are the game's own then.

namespace
{
// NiAVObject's world bound: centre, radius
constexpr size_t kWorldBoundCenter = 0xB0;
constexpr size_t kWorldBoundRadius = 0xBC;
// BSShaderAccumulator: its camera, and which draw it's used for
constexpr size_t kAccumulatorCamera = 0x10;
constexpr size_t kAccumulatorMode = 0x560;
// NiCamera: position, frustum left and right, whether it's orthographic, viewport as parts of the target
constexpr size_t kCameraPosition = 0xA0;
constexpr size_t kCameraFrustumLeft = 0x160;
constexpr size_t kCameraFrustumRight = 0x164;
constexpr size_t kCameraOrtho = 0x178;
constexpr size_t kCameraViewportLeft = 0x184;
constexpr size_t kCameraViewportRight = 0x188;
// The size local lights' maps are worked out from (iShadowMapResolution)
constexpr uint32_t kShadowResolution = 1239716;

template <class T> T Read(const void* apObject, size_t aOffset) noexcept
{
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(apObject) + aOffset);
}

uint32_t s_texels = 0;
std::atomic<uint64_t> s_seen{0};
std::atomic<uint64_t> s_culled{0};
} // namespace

bool SmallCasterCull::Enabled() noexcept
{
    return s_texels != 0;
}

void SmallCasterCull::Refresh() noexcept
{
    const uint32_t texels = std::min<uint32_t>(ShadowFeatures::ReadSetting(L"iSmallCasterTexels", 0), 16);
    if (texels != s_texels)
        spdlog::info("Small shadow casters: {}", texels ? fmt::format("under {} texels aren't drawn", texels) : std::string("all drawn"));
    s_texels = texels;
}

bool SmallCasterCull::ShouldSkip(void* apAccumulator, void* apGeometry) noexcept
{
    const float radius = Read<float>(apGeometry, kWorldBoundRadius);
    if (radius <= 0.f)
        return false; // the game's way of saying it has no bound yet
    s_seen.fetch_add(1, std::memory_order_relaxed);

    float worldPerTexel = 0.f;
    const int cascade = ShadowFeatures::CascadeOfAccumulator(apAccumulator);
    if (cascade >= 0)
    {
        worldPerTexel = ShadowFeatures::SunCascades()[cascade].WorldPerTexel;
    }
    else
    {
        // A local light's map: a perspective camera, so a texel grows with the distance to the near side of the bound
        const void* pCamera = Read<const void*>(apAccumulator, kAccumulatorCamera);
        if (!pCamera || Read<uint8_t>(pCamera, kCameraOrtho))
            return false;
        const VersionDbPtr<uint32_t> resolution(kShadowResolution);
        const float size = (Read<float>(pCamera, kCameraViewportRight) - Read<float>(pCamera, kCameraViewportLeft)) * static_cast<float>(*resolution.Get());
        if (size < 16.f || size > 16384.f)
            return false;
        const float* pCenter = reinterpret_cast<const float*>(static_cast<const uint8_t*>(apGeometry) + kWorldBoundCenter);
        const float* pEye = reinterpret_cast<const float*>(static_cast<const uint8_t*>(pCamera) + kCameraPosition);
        const float x = pCenter[0] - pEye[0], y = pCenter[1] - pEye[1], z = pCenter[2] - pEye[2];
        const float distance = std::sqrt(x * x + y * y + z * z) - radius;
        if (distance <= 1.f)
            return false;
        const float width = Read<float>(pCamera, kCameraFrustumRight) - Read<float>(pCamera, kCameraFrustumLeft);
        worldPerTexel = distance * width / size;
    }

    if (worldPerTexel <= 0.f || radius * 2.f >= worldPerTexel * static_cast<float>(s_texels))
        return false;
    s_culled.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void SmallCasterCull::Report(double aSeconds) noexcept
{
    const uint64_t seen = s_seen.exchange(0, std::memory_order_relaxed);
    const uint64_t culled = s_culled.exchange(0, std::memory_order_relaxed);
    if (s_texels && seen)
        spdlog::info("Small shadow casters: {} of {} left out ({:.0f}%), {:.0f} draws a second fewer", culled, seen, static_cast<double>(culled) * 100.0 / static_cast<double>(seen),
                     static_cast<double>(culled) / aSeconds);
}
