#include <ShadowFeatures.h>
#include <Renderer.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

// Occlusion culling. The game decides what's hidden from data baked into each area (previs), which mods break and which knows
// nothing of what they add. This goes by what's actually on screen: last frames' main depth, made small on the GPU (each cell
// holds the farthest depth under it) and read back without waiting. Moved to the current camera on the CPU, it says for an
// object's bound whether something nearer covers everything behind it; if so the object isn't drawn. Things under a few hundred
// units away don't count as cover (first person arms and weapon, the player, close characters: they move in ways the old depth
// doesn't show). Something hidden that comes into view can show a few frames late, so this is a choice, off until asked for.

using Microsoft::WRL::ComPtr;

namespace
{
constexpr uint32_t kMainCull = 718912;            // the main view's culling, before anything's added this frame
constexpr uint32_t kMainAccumulator = 1211382;    // the main view's accumulator
constexpr uint32_t kRenderCamera = 1444213;       // the camera the main depth is drawn with
constexpr uint32_t kRegisterTableForRead = 1279939;
// Where the main depth is, read from 1.10.163 (11-06): the main pass sets depth target 1 before the scene (0x14285306F:
// SetDepthStencilTarget(manager, 1, mode 3, slice 0)). SetDepthStencilTarget (704517 CommonLib's, 0x141D31940) turns the target
// into the renderer's own depth image through the manager's table at +0xF54; the renderer (RendererData, BGSRenderer here) keeps
// 13 of them at +0x1D48, 0x98 each, the texture first (CommonLibF4's BSGraphics.h, the same layout).
constexpr uint32_t kRenderTargetManager = 1508458; // BSGraphics::RenderTargetManager, 0x14384FD30
constexpr size_t kDepthTargetImages = 0xF54;
constexpr uint32_t kMainDepthTarget = 1;
constexpr size_t kRendererDepthImages = 0x1D48;
constexpr size_t kRendererDepthImageSize = 0x98;
constexpr uint32_t kRendererDepthImageCount = 13;

// NiCamera, as the game's own projection is built from it (141D216F0, standard depth: 0 near, 1 far)
constexpr size_t kCameraForward = 0x70;
constexpr size_t kCameraUp = 0x80;
constexpr size_t kCameraRight = 0x90;
constexpr size_t kCameraPosition = 0xA0;
constexpr size_t kCameraFrustum = 0x160; // left, right, top, bottom, near, far
constexpr size_t kCameraOrtho = 0x178;
// NiAVObject's world bound and flags; BSGeometry's skin
constexpr size_t kBoundCenter = 0xB0;
constexpr size_t kBoundRadius = 0xBC;
constexpr size_t kObjectFlags = 0x108;
constexpr size_t kGeometrySkin = 0x140;
constexpr uint64_t kObjectDynamicMesh = 1ull << 0x22;

// The small depth's size
constexpr uint32_t kGridWidth = 160;
constexpr uint32_t kCoarse = 4;
// Nearer than this isn't cover (world units)
constexpr float kNearestCover = 300.f;
// Past the farthest depth under it by this much before an object counts as hidden
constexpr float kMarginUnits = 24.f;
constexpr float kMarginShare = 0.01f;
// Depth this many frames old isn't used; nor if the camera has since moved this far
constexpr uint32_t kOldestFrames = 8;
constexpr float kFarthestMove = 250.f;
constexpr uint32_t kCopies = 4;

constexpr char kDownsampleShader[] = R"(
Texture2D<float> gDepth : register(t0);
RWTexture2D<float> gOut : register(u0);
cbuffer Values : register(b0) { uint2 gBlock; uint2 gSize; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gSize.x || id.y >= gSize.y)
        return;
    float farthest = 0.0;
    uint2 start = id.xy * gBlock;
    [loop] for (uint y = 0; y < gBlock.y; ++y)
    {
        [loop] for (uint x = 0; x < gBlock.x; ++x)
            farthest = max(farthest, gDepth.Load(int3(start + uint2(x, y), 0)));
    }
    gOut[id.xy] = farthest;
}
)";

struct CameraShot
{
    float Eye[3]{};
    float Forward[3]{};
    float Up[3]{};
    float Right[3]{};
    float Left = 0.f, RightEdge = 0.f, Top = 0.f, Bottom = 0.f, Near = 0.f, Far = 0.f;
    bool Valid = false;
};

template <class T> T Read(const void* apObject, size_t aOffset) noexcept
{
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(apObject) + aOffset);
}

CameraShot Shoot(const void* apCamera) noexcept
{
    CameraShot shot;
    if (!apCamera || Read<uint8_t>(apCamera, kCameraOrtho))
        return shot;
    const auto copy3 = [apCamera](size_t aOffset, float* apOut)
    {
        std::memcpy(apOut, static_cast<const uint8_t*>(apCamera) + aOffset, 12);
    };
    copy3(kCameraPosition, shot.Eye);
    copy3(kCameraForward, shot.Forward);
    copy3(kCameraUp, shot.Up);
    copy3(kCameraRight, shot.Right);
    const float* pFrustum = reinterpret_cast<const float*>(static_cast<const uint8_t*>(apCamera) + kCameraFrustum);
    shot.Left = pFrustum[0];
    shot.RightEdge = pFrustum[1];
    shot.Top = pFrustum[2];
    shot.Bottom = pFrustum[3];
    shot.Near = pFrustum[4];
    shot.Far = pFrustum[5];
    shot.Valid = shot.Far > shot.Near && shot.Near > 0.f && shot.RightEdge > shot.Left && shot.Top > shot.Bottom;
    return shot;
}

float Dot(const float* apA, const float* apB) noexcept
{
    return apA[0] * apB[0] + apA[1] * apB[1] + apA[2] * apB[2];
}

// --- on the GPU

struct Readback
{
    ComPtr<ID3D11Texture2D> Copy;
    CameraShot Camera;
    uint32_t Frame = 0;
    bool Waiting = false;
};

bool s_wanted = false;
bool s_broken = false;
ComPtr<ID3D11ComputeShader> s_shader;
ComPtr<ID3D11Buffer> s_values;
ComPtr<ID3D11Texture2D> s_small;
ComPtr<ID3D11UnorderedAccessView> s_smallView;
ComPtr<ID3D11Texture2D> s_depthTexture;
ComPtr<ID3D11ShaderResourceView> s_depthView;
Readback s_readbacks[kCopies];
uint32_t s_nextReadback = 0;
uint32_t s_width = 0, s_height = 0, s_block = 0;

// --- on the CPU

std::vector<float> s_latest;
CameraShot s_latestCamera;
uint32_t s_latestFrame = 0;
bool s_haveLatest = false;

std::vector<float> s_grid;
std::vector<float> s_gridCoarse;
CameraShot s_gridCamera;
bool s_gridValid = false;
uint32_t s_coarseWidth = 0, s_coarseHeight = 0;

// The main view's depth texture, as the renderer keeps it (none bound is the usual case where this is read: the 11-06 run had
// "0 frames with depth" from looking only at what was bound)
ID3D11Texture2D* MainDepth() noexcept
{
    auto* pRenderer = BGSRenderer::Get();
    if (!pRenderer)
        return nullptr;
    const VersionDbPtr<uint8_t> manager(kRenderTargetManager);
    if (!manager.Get())
        return nullptr;
    const uint32_t image = *reinterpret_cast<const uint32_t*>(manager.Get() + kDepthTargetImages + kMainDepthTarget * sizeof(uint32_t));
    if (image >= kRendererDepthImageCount)
        return nullptr;
    return *reinterpret_cast<ID3D11Texture2D* const*>(reinterpret_cast<const uint8_t*>(pRenderer) + kRendererDepthImages + image * kRendererDepthImageSize);
}

std::atomic<uint32_t> s_noDepth{0};

ID3D11ComputeShader* ShaderOf(ID3D11Device* apDevice) noexcept
{
    if (s_shader)
        return s_shader.Get();
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kDownsampleShader, sizeof(kDownsampleShader) - 1, "occlusion", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        spdlog::warn("Occlusion culling: off, its shader didn't compile: {}", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        s_broken = true;
        return nullptr;
    }
    if (FAILED(apDevice->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s_shader)))
    {
        spdlog::warn("Occlusion culling: off, its shader couldn't be made");
        s_broken = true;
        return nullptr;
    }
    D3D11_BUFFER_DESC values{};
    values.ByteWidth = 16;
    values.Usage = D3D11_USAGE_DYNAMIC;
    values.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    values.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(apDevice->CreateBuffer(&values, nullptr, &s_values)))
    {
        s_broken = true;
        return nullptr;
    }
    return s_shader.Get();
}

bool EnsureTargets(ID3D11Device* apDevice, ID3D11Texture2D* apDepth, const D3D11_TEXTURE2D_DESC& acDesc) noexcept
{
    if (s_depthTexture.Get() == apDepth && s_small)
        return true;

    DXGI_FORMAT viewFormat;
    switch (acDesc.Format)
    {
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: viewFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: viewFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: viewFormat = DXGI_FORMAT_R32_FLOAT; break;
    default:
        spdlog::warn("Occlusion culling: off, the main depth's format is {}", static_cast<int>(acDesc.Format));
        s_broken = true;
        return false;
    }
    if (!(acDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) || acDesc.SampleDesc.Count != 1)
    {
        spdlog::warn("Occlusion culling: off, the main depth can't be read");
        s_broken = true;
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = viewFormat;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    s_depthView.Reset();
    if (FAILED(apDevice->CreateShaderResourceView(apDepth, &view, &s_depthView)))
    {
        spdlog::warn("Occlusion culling: off, couldn't view the main depth");
        s_broken = true;
        return false;
    }
    s_depthTexture = apDepth;

    s_block = std::max<uint32_t>(acDesc.Width / kGridWidth, 1);
    s_width = (acDesc.Width + s_block - 1) / s_block;
    s_height = (acDesc.Height + s_block - 1) / s_block;

    D3D11_TEXTURE2D_DESC smallDesc{};
    smallDesc.Width = s_width;
    smallDesc.Height = s_height;
    smallDesc.MipLevels = 1;
    smallDesc.ArraySize = 1;
    smallDesc.Format = DXGI_FORMAT_R32_FLOAT;
    smallDesc.SampleDesc.Count = 1;
    smallDesc.Usage = D3D11_USAGE_DEFAULT;
    smallDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    s_small.Reset();
    s_smallView.Reset();
    if (FAILED(apDevice->CreateTexture2D(&smallDesc, nullptr, &s_small)) || FAILED(apDevice->CreateUnorderedAccessView(s_small.Get(), nullptr, &s_smallView)))
    {
        spdlog::warn("Occlusion culling: off, its {}x{} depth couldn't be made", s_width, s_height);
        s_broken = true;
        return false;
    }
    smallDesc.BindFlags = 0;
    smallDesc.Usage = D3D11_USAGE_STAGING;
    smallDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& readback : s_readbacks)
    {
        readback = {};
        if (FAILED(apDevice->CreateTexture2D(&smallDesc, nullptr, &readback.Copy)))
        {
            s_broken = true;
            return false;
        }
    }
    s_haveLatest = false;
    s_gridValid = false;
    s_latest.assign(static_cast<size_t>(s_width) * s_height, 1.f);
    s_grid.assign(static_cast<size_t>(s_width) * s_height, FLT_MAX);
    s_coarseWidth = (s_width + kCoarse - 1) / kCoarse;
    s_coarseHeight = (s_height + kCoarse - 1) / kCoarse;
    s_gridCoarse.assign(static_cast<size_t>(s_coarseWidth) * s_coarseHeight, FLT_MAX);
    spdlog::info("Occlusion culling: the main depth {}x{} (format {}) makes a {}x{} grid", acDesc.Width, acDesc.Height, static_cast<int>(acDesc.Format), s_width, s_height);
    return true;
}

// Each cell of the old depth, at where it is under the current camera. Cells nothing lands in stay open (nothing hides there).
void Reproject(const CameraShot& acNow) noexcept
{
    std::fill(s_grid.begin(), s_grid.end(), FLT_MAX);
    std::vector<uint8_t> reached(s_grid.size(), 0);

    const CameraShot& then = s_latestCamera;
    const float widthThen = then.RightEdge - then.Left, heightThen = then.Top - then.Bottom;
    const float widthNow = acNow.RightEdge - acNow.Left, heightNow = acNow.Top - acNow.Bottom;
    const float depthRange = then.Far - then.Near;
    const float nearFar = then.Near * then.Far;

    for (uint32_t j = 0; j < s_height; ++j)
    {
        const float tangentY = then.Top - (static_cast<float>(j) + 0.5f) / static_cast<float>(s_height) * heightThen;
        for (uint32_t i = 0; i < s_width; ++i)
        {
            const float depth = s_latest[static_cast<size_t>(j) * s_width + i];
            if (depth >= 0.99999f)
                continue; // sky
            const float z = nearFar / (then.Far - depth * depthRange);
            if (z < kNearestCover)
                continue;
            const float tangentX = then.Left + (static_cast<float>(i) + 0.5f) / static_cast<float>(s_width) * widthThen;
            float world[3];
            for (int a = 0; a < 3; ++a)
                world[a] = then.Eye[a] + then.Right[a] * tangentX * z + then.Up[a] * tangentY * z + then.Forward[a] * z - acNow.Eye[a];

            const float zNow = Dot(world, acNow.Forward);
            if (zNow <= kNearestCover)
                continue;
            const float u = (Dot(world, acNow.Right) / zNow - acNow.Left) / widthNow;
            const float v = (acNow.Top - Dot(world, acNow.Up) / zNow) / heightNow;
            if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f)
                continue;
            const auto cell = static_cast<size_t>(v * static_cast<float>(s_height)) * s_width + static_cast<size_t>(u * static_cast<float>(s_width));
            if (!reached[cell])
            {
                reached[cell] = 1;
                s_grid[cell] = zNow;
            }
            else
            {
                s_grid[cell] = std::max(s_grid[cell], zNow);
            }
        }
    }

    // Spread the far side one cell around: what moved a little between frames doesn't count as covering past its edge
    static std::vector<float> s_spread;
    s_spread = s_grid;
    for (uint32_t j = 0; j < s_height; ++j)
    {
        for (uint32_t i = 0; i < s_width; ++i)
        {
            float farthest = 0.f;
            for (uint32_t y = j ? j - 1 : 0; y <= std::min(j + 1, s_height - 1); ++y)
            {
                for (uint32_t x = i ? i - 1 : 0; x <= std::min(i + 1, s_width - 1); ++x)
                    farthest = std::max(farthest, s_spread[static_cast<size_t>(y) * s_width + x]);
            }
            s_grid[static_cast<size_t>(j) * s_width + i] = farthest;
        }
    }

    std::fill(s_gridCoarse.begin(), s_gridCoarse.end(), 0.f);
    for (uint32_t j = 0; j < s_height; ++j)
    {
        for (uint32_t i = 0; i < s_width; ++i)
        {
            float& coarse = s_gridCoarse[static_cast<size_t>(j / kCoarse) * s_coarseWidth + i / kCoarse];
            coarse = std::max(coarse, s_grid[static_cast<size_t>(j) * s_width + i]);
        }
    }

    s_gridCamera = acNow;
    s_gridValid = true;
}

// --- per object

const void* s_pMain = nullptr;
std::atomic<uint64_t> s_tested{0};
std::atomic<uint64_t> s_hidden{0};
std::atomic<uint32_t> s_framesWith{0};

bool IsHidden(const void* apGeometry) noexcept
{
    if (Read<const void*>(apGeometry, kGeometrySkin) || (Read<uint64_t>(apGeometry, kObjectFlags) & kObjectDynamicMesh))
        return false; // moving meshes' bounds don't follow them closely enough to hide them by
    const float radius = Read<float>(apGeometry, kBoundRadius);
    if (radius <= 0.f)
        return false;

    const CameraShot& camera = s_gridCamera;
    const float* pCenter = reinterpret_cast<const float*>(static_cast<const uint8_t*>(apGeometry) + kBoundCenter);
    const float toObject[3] = {pCenter[0] - camera.Eye[0], pCenter[1] - camera.Eye[1], pCenter[2] - camera.Eye[2]};
    const float z = Dot(toObject, camera.Forward);
    const float nearSide = z - radius;
    if (nearSide <= kNearestCover)
        return false;
    s_tested.fetch_add(1, std::memory_order_relaxed);

    const float x = Dot(toObject, camera.Right);
    const float y = Dot(toObject, camera.Up);
    const float farSide = z + radius;
    const float tangentLeft = std::min((x - radius) / nearSide, (x - radius) / farSide);
    const float tangentRight = std::max((x + radius) / nearSide, (x + radius) / farSide);
    const float tangentLow = std::min((y - radius) / nearSide, (y - radius) / farSide);
    const float tangentHigh = std::max((y + radius) / nearSide, (y + radius) / farSide);

    const float width = camera.RightEdge - camera.Left, height = camera.Top - camera.Bottom;
    const auto toCellX = [&](float aTangent) { return (aTangent - camera.Left) / width * static_cast<float>(s_width); };
    const auto toCellY = [&](float aTangent) { return (camera.Top - aTangent) / height * static_cast<float>(s_height); };
    const int left = std::max(static_cast<int>(std::floor(toCellX(tangentLeft))) - 1, 0);
    const int right = std::min(static_cast<int>(std::floor(toCellX(tangentRight))) + 1, static_cast<int>(s_width) - 1);
    const int top = std::max(static_cast<int>(std::floor(toCellY(tangentHigh))) - 1, 0);
    const int bottom = std::min(static_cast<int>(std::floor(toCellY(tangentLow))) + 1, static_cast<int>(s_height) - 1);
    if (left > right || top > bottom)
        return false;

    const float limit = nearSide * (1.f - kMarginShare) - kMarginUnits;
    if (right - left >= static_cast<int>(kCoarse) * 2 || bottom - top >= static_cast<int>(kCoarse) * 2)
    {
        for (int j = top / static_cast<int>(kCoarse); j <= bottom / static_cast<int>(kCoarse); ++j)
        {
            const float* pRow = &s_gridCoarse[static_cast<size_t>(j) * s_coarseWidth];
            for (int i = left / static_cast<int>(kCoarse); i <= right / static_cast<int>(kCoarse); ++i)
            {
                if (pRow[i] >= limit)
                    return false;
            }
        }
    }
    else
    {
        for (int j = top; j <= bottom; ++j)
        {
            const float* pRow = &s_grid[static_cast<size_t>(j) * s_width];
            for (int i = left; i <= right; ++i)
            {
                if (pRow[i] >= limit)
                    return false;
            }
        }
    }
    s_hidden.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// The main view's accumulate table entries: the first frame's mode, the depth pre-pass and the pass after it
constexpr uint32_t kModes[3] = {ShadowFeatures::kModeMain, ShadowFeatures::kModeDepth, ShadowFeatures::kModeMainAfterDepth};
ShadowFeatures::TRegister* s_real[3]{};
bool s_swapped = false;

template <int Index> uint64_t HookRegister(void* apAccumulator, void* apGeometry, void* apProperty)
{
    if (apAccumulator == s_pMain && s_gridValid && IsHidden(apGeometry))
        return 1;
    return s_real[Index](apAccumulator, apGeometry, apProperty);
}
ShadowFeatures::TRegister* const kHooks[3] = {&HookRegister<0>, &HookRegister<1>, &HookRegister<2>};

using TMainCull = void();
TMainCull* RealMainCull = nullptr;

void HookMainCull()
{
    OcclusionCull::OnFrameStart();
    RealMainCull();
}
} // namespace

void ShadowFeatures::RefreshMainRegister() noexcept
{
    const bool wanted = s_wanted && !s_broken;
    if (wanted == s_swapped)
        return;
    const VersionDbPtr<TRegister*> table(kRegisterTableForRead);
    for (int i = 0; i < 3; ++i)
    {
        TRegister** pEntry = table.Get() + kModes[i];
        if (wanted)
        {
            s_real[i] = *pEntry;
            *pEntry = kHooks[i];
        }
        else if (*pEntry == kHooks[i])
        {
            *pEntry = s_real[i];
        }
    }
    s_swapped = wanted;
    s_gridValid = false;
    const VersionDbPtr<void*> main(kMainAccumulator);
    s_pMain = *main.Get();
    spdlog::info("Occlusion culling: {}", wanted ? "on" : "off");
}

void OcclusionCull::Refresh() noexcept
{
    s_wanted = RealMainCull && ShadowFeatures::ReadSetting(L"bOcclusionCull", 0) != 0;
}

void OcclusionCull::OnDepthReady() noexcept
{
    if (!s_wanted || s_broken)
        return;
    auto* pContext = ShadowFeatures::Context();
    auto* pDevice = ShadowFeatures::Device();
    if (!pContext || !pDevice || !ShaderOf(pDevice))
        return;

    const VersionDbPtr<void*> renderCamera(kRenderCamera);
    const CameraShot camera = Shoot(*renderCamera.Get());
    if (!camera.Valid)
        return;

    ShadowFeatures::FlushRenderState();
    ID3D11RenderTargetView* pTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ComPtr<ID3D11DepthStencilView> depthView;
    pContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, pTargets, &depthView);

    const auto releaseTargets = [&pTargets]()
    {
        for (auto*& pTarget : pTargets)
        {
            if (pTarget)
                pTarget->Release();
            pTarget = nullptr;
        }
    };
    // (what's bound is put back as it was after; the main depth itself comes from the renderer's table)
    ID3D11Texture2D* const pDepth = MainDepth();
    if (!pDepth)
    {
        s_noDepth.fetch_add(1, std::memory_order_relaxed);
        releaseTargets();
        return;
    }
    ComPtr<ID3D11Texture2D> texture = pDepth;
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    if (desc.ArraySize != 1 || !EnsureTargets(pDevice, texture.Get(), desc))
    {
        releaseTargets();
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(pContext->Map(s_values.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        const uint32_t values[4] = {s_block, s_block, s_width, s_height};
        std::memcpy(mapped.pData, values, sizeof(values));
        pContext->Unmap(s_values.Get(), 0);
    }

    // What's bound for the compute stage goes back as it was; the depth target comes off while it's read
    ComPtr<ID3D11ComputeShader> oldShader;
    ID3D11ClassInstance* pOldInstances[256]{};
    UINT oldInstanceCount = 256;
    ComPtr<ID3D11ShaderResourceView> oldResource;
    ComPtr<ID3D11UnorderedAccessView> oldAccess;
    ComPtr<ID3D11Buffer> oldValues;
    pContext->CSGetShader(&oldShader, pOldInstances, &oldInstanceCount);
    pContext->CSGetShaderResources(0, 1, &oldResource);
    pContext->CSGetUnorderedAccessViews(0, 1, &oldAccess);
    pContext->CSGetConstantBuffers(0, 1, &oldValues);

    pContext->OMSetRenderTargets(0, nullptr, nullptr);
    pContext->CSSetShader(s_shader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* pResource = s_depthView.Get();
    ID3D11UnorderedAccessView* pAccess = s_smallView.Get();
    ID3D11Buffer* pValues = s_values.Get();
    pContext->CSSetShaderResources(0, 1, &pResource);
    pContext->CSSetUnorderedAccessViews(0, 1, &pAccess, nullptr);
    pContext->CSSetConstantBuffers(0, 1, &pValues);
    pContext->Dispatch((s_width + 7) / 8, (s_height + 7) / 8, 1);

    // Hand everything back
    ID3D11ShaderResourceView* pNoResource = nullptr;
    pContext->CSSetShaderResources(0, 1, &pNoResource);
    ID3D11UnorderedAccessView* pOldAccess = oldAccess.Get();
    pContext->CSSetUnorderedAccessViews(0, 1, &pOldAccess, nullptr);
    ID3D11ShaderResourceView* pOldResource = oldResource.Get();
    pContext->CSSetShaderResources(0, 1, &pOldResource);
    ID3D11Buffer* pOldValues = oldValues.Get();
    pContext->CSSetConstantBuffers(0, 1, &pOldValues);
    pContext->CSSetShader(oldShader.Get(), pOldInstances, oldInstanceCount);
    for (UINT i = 0; i < oldInstanceCount; ++i)
    {
        if (pOldInstances[i])
            pOldInstances[i]->Release();
    }
    pContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, pTargets, depthView.Get());
    releaseTargets();

    Readback& readback = s_readbacks[s_nextReadback];
    if (!readback.Waiting)
    {
        pContext->CopyResource(readback.Copy.Get(), s_small.Get());
        readback.Camera = camera;
        readback.Frame = ShadowFeatures::FrameNumber();
        readback.Waiting = true;
        s_nextReadback = (s_nextReadback + 1) % kCopies;
    }
}

void OcclusionCull::OnFrameStart() noexcept
{
    if (!s_wanted || s_broken || !s_small)
    {
        s_gridValid = false;
        return;
    }
    auto* pContext = ShadowFeatures::Context();
    if (!pContext)
        return;

    // The newest copy the GPU has finished; never waited for
    for (uint32_t i = 0; i < kCopies; ++i)
    {
        Readback& readback = s_readbacks[(s_nextReadback + i) % kCopies];
        if (!readback.Waiting)
            continue;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT result = pContext->Map(readback.Copy.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (result == DXGI_ERROR_WAS_STILL_DRAWING)
            continue;
        if (SUCCEEDED(result))
        {
            for (uint32_t row = 0; row < s_height; ++row)
                std::memcpy(&s_latest[static_cast<size_t>(row) * s_width], static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch, s_width * sizeof(float));
            pContext->Unmap(readback.Copy.Get(), 0);
            s_latestCamera = readback.Camera;
            s_latestFrame = readback.Frame;
            s_haveLatest = true;
        }
        readback.Waiting = false;
    }

    s_gridValid = false;
    if (!s_haveLatest || ShadowFeatures::FrameNumber() - s_latestFrame > kOldestFrames)
        return;
    const VersionDbPtr<void*> renderCamera(kRenderCamera);
    const CameraShot now = Shoot(*renderCamera.Get());
    if (!now.Valid)
        return;
    float moved[3];
    for (int a = 0; a < 3; ++a)
        moved[a] = now.Eye[a] - s_latestCamera.Eye[a];
    if (Dot(moved, moved) > kFarthestMove * kFarthestMove)
        return;
    Reproject(now);
    s_framesWith.fetch_add(1, std::memory_order_relaxed);
}

void OcclusionCull::Report(double aSeconds) noexcept
{
    const uint64_t tested = s_tested.exchange(0, std::memory_order_relaxed);
    const uint64_t hidden = s_hidden.exchange(0, std::memory_order_relaxed);
    const uint32_t frames = s_framesWith.exchange(0, std::memory_order_relaxed);
    const uint32_t noDepth = s_noDepth.exchange(0, std::memory_order_relaxed);
    if (s_wanted && !s_broken)
        spdlog::info("Occlusion culling: {} frames with depth to go by of about {:.0f}, {:.0f} objects a frame tested, {:.0f} hidden ({:.1f}%){}", frames, aSeconds * 60.0, frames ? static_cast<double>(tested) / frames : 0.0,
                     frames ? static_cast<double>(hidden) / frames : 0.0, tested ? static_cast<double>(hidden) * 100.0 / static_cast<double>(tested) : 0.0,
                     noDepth ? fmt::format("; no main depth in {} frames", noDepth) : std::string());
}

static TiltedPhoques::Initializer s_occlusionCullHooks(
    []()
    {
        static constexpr uint8_t kCullStart[] = {0x40, 0x53, 0x56, 0x41, 0x55, 0x48, 0x83, 0xEC, 0x50, 0x48};
        if (!ShadowFeatures::Matches("Occlusion culling", "the main culling", kMainCull, kCullStart, sizeof(kCullStart)))
            return;
        const VersionDbPtr<uint8_t> cull(kMainCull);
        RealMainCull = reinterpret_cast<TMainCull*>(cull.Get());
        TP_HOOK(&RealMainCull, HookMainCull);
    });
