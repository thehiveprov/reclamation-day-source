#include <SelectionOutline.h>

#include <TESObjectREFR.h>
#include <Renderer.h>
#include <BSGraphics/BSGraphicsRenderer.h>
#include <Interface/HudMarker.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

// The outline Better Interact draws around what it picked (BetterInteract.cpp), based on Zzyxzz's Smart Selection's
// outline: the meshes are drawn into a mask, the distance to the mask is spread out from it across and then down, and
// the band within the width is blended over the frame. Ours draws the meshes from the game's own buffers with our own
// shaders, instead of having the engine render them again.

using Microsoft::WRL::ComPtr;

namespace
{
// Checked against 1.10.163 (BSTriShape::LoadBinary, BSGeometry's destructor and renderer data setter, the renderer's
// DrawTriShape 1533774, BSSkin::Instance::LinkObject, BSSkin::BoneData::LoadBinary, NiSwitchNode::LoadBinary)

// NiAVObject: flags (bit 0: hidden), world transform (rotation rows of 4 floats, translation, scale), world bound
constexpr size_t kObjectFlags = 0x108;
constexpr uint64_t kObjectHidden = 1;
constexpr size_t kWorldTransform = 0x70;
constexpr size_t kWorldBound = 0xB0;
// NiNode's children (an array and its capacity, empty slots null); NiSwitchNode's child that's shown
constexpr size_t kNodeChildren = 0x128;
constexpr size_t kNodeChildCapacity = 0x130;
constexpr size_t kSwitchIndex = 0x144;
// BSGeometry: its properties (the second is the shader), its skin (BSSkin::Instance), the renderer's data for it
// (BSGraphics::TriShape: the vertex description, the vertex buffer, the index buffer); BSTriShape's triangles
constexpr size_t kGeometryProperties = 0x130;
constexpr size_t kGeometrySkin = 0x140;
constexpr size_t kGeometryRendererData = 0x148;
constexpr size_t kTriShapeTriangles = 0x160;
constexpr size_t kRendererVertices = 0x08;
constexpr size_t kRendererIndices = 0x10;
// A BSGraphics buffer: its ID3D11Buffer first, where in it the shape's data starts (buffers are shared), and whether it's
// still being uploaded (the game waits for that before drawing; we leave it out for a frame)
constexpr size_t kBufferOffset = 0x48;
constexpr size_t kBufferPending = 0x44;
constexpr size_t kBufferUploading = 0x4E;
// BSSkin::Instance: its bones (count), the bones' world transforms (pointers into each bone), the bind data
// (BSSkin::BoneData: per bone its bound and its skin to bone transform)
constexpr size_t kSkinBoneCount = 0x20;
constexpr size_t kSkinBoneWorlds = 0x28;
constexpr size_t kSkinBoneData = 0x40;
constexpr size_t kBoneDataEntries = 0x10;
constexpr size_t kBoneDataCount = 0x20;
constexpr size_t kBoneDataStride = 0x50;
constexpr size_t kBoneDataTransform = 0x10;
// Lit surfaces only: effects (light cones, glow planes, screens' glare) would widen the silhouette
constexpr uint32_t kLightingShaderPropertyVtable = 241916;

// TESObjectREFR's 3D (as Better Interact picks it); NiObject's casts by their slot
constexpr size_t kGet3D = 0x8C;
constexpr size_t kIsNode = 0x03;
constexpr size_t kIsSwitchNode = 0x05;
constexpr size_t kIsTriShape = 0x0A;
constexpr size_t kIsDynamicTriShape = 0x0B;

// The vertex description: its size in 4 bytes, the skinning data's offset, its flags
constexpr uint64_t kVertexSkinned = 0x40;
constexpr uint64_t kVertexFullPrecision = 0x400;

constexpr size_t kMaxBones = 256;
constexpr size_t kMaxMeshes = 256;
constexpr size_t kMaxNodes = 4096;
// The width is in pixels at this many lines, the band no wider than this many pixels
constexpr float kReferenceHeight = 1080.f;
constexpr int kMaxReach = 12;
// A target not set for this long isn't drawn (the pick stopped running)
constexpr auto kFresh = std::chrono::milliseconds(250);

template <class T> T Read(const void* apObject, size_t aOffset) noexcept
{
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(apObject) + aOffset);
}

template <class TResult> TResult CallVirtual(const void* apObject, size_t aSlot) noexcept
{
    using TFunction = TResult(const void*);
    return (*reinterpret_cast<TFunction* const* const*>(apObject))[aSlot](apObject);
}

// --- shaders

// A mesh without skin: the object to clip matrix (by rows). Depth isn't tested; anything a unit past the eye is drawn.
constexpr char kStaticVertexShader[] = R"(
cbuffer Object : register(b0) { float4 gToClip[4]; };
float4 main(float3 position : POSITION) : SV_Position
{
    float4 p = float4(position, 1.0);
    float4 clip = float4(dot(gToClip[0], p), dot(gToClip[1], p), 0.0, dot(gToClip[3], p));
    clip.z = clip.w - 1.0;
    return clip;
}
)";

// A skinned mesh: the world to clip matrix and the bones' skin to world matrices (3 rows each)
constexpr char kSkinnedVertexShader[] = R"(
cbuffer Object : register(b0) { float4 gToClip[4]; };
cbuffer Bones : register(b1) { float4 gBones[768]; };
struct Input
{
    float3 position : POSITION;
    float4 weights : BLENDWEIGHT;
    uint4 bones : BLENDINDICES;
};
float4 main(Input input) : SV_Position
{
    float4 p = float4(input.position, 1.0);
    float3 world = 0.0;
    float total = 0.0;
    [unroll] for (int i = 0; i < 4; ++i)
    {
        uint row = min(input.bones[i], 255) * 3;
        world += input.weights[i] * float3(dot(gBones[row], p), dot(gBones[row + 1], p), dot(gBones[row + 2], p));
        total += input.weights[i];
    }
    float4 w = float4(total > 1e-4 ? world / total : float3(dot(gBones[0], p), dot(gBones[1], p), dot(gBones[2], p)), 1.0);
    float4 clip = float4(dot(gToClip[0], w), dot(gToClip[1], w), 0.0, dot(gToClip[3], w));
    clip.z = clip.w - 1.0;
    return clip;
}
)";

constexpr char kMaskPixelShader[] = R"(
float main() : SV_Target { return 1.0; }
)";

// One triangle over the whole target
constexpr char kScreenVertexShader[] = R"(
float4 main(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);
}
)";

constexpr char kPassConstants[] = R"(
cbuffer PassValues : register(b0)
{
    float4 gColour;
    float2 gTexel;
    float gRadius;
    int gReach;
    int2 gSize;
    int2 gUnused;
};
)";

// Across: how far the nearest covered pixel on this row is (reach + 1: none within reach)
constexpr char kAcrossPixelShader[] = R"(
Texture2D<float> gMask : register(t0);
float main(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    float nearest = gReach + 1.0;
    [loop] for (int x = -gReach; x <= gReach; ++x)
    {
        int column = p.x + x;
        if (column >= 0 && column < gSize.x && gMask.Load(int3(column, p.y, 0)) >= 0.5)
            nearest = min(nearest, abs((float)x));
    }
    return nearest;
}
)";

// Down, then the band: the distance to the nearest covered pixel from the rows' distances across; full within the
// width, its last pixel blended, nothing inside the object (four bilinear taps half a pixel apart soften that edge)
constexpr char kBandPixelShader[] = R"(
Texture2D<float> gMask : register(t0);
Texture2D<float> gAcross : register(t1);
SamplerState gLinear : register(s0);
float4 main(float4 position : SV_Position) : SV_Target
{
    int2 p = int2(position.xy);
    float nearest = (gReach + 1.0) * (gReach + 1.0);
    [loop] for (int y = -gReach; y <= gReach; ++y)
    {
        float across = gAcross.Load(int3(p.x, clamp(p.y + y, 0, gSize.y - 1), 0));
        nearest = min(nearest, across * across + (float)(y * y));
    }
    float band = saturate(gRadius + 1.0 - sqrt(nearest));
    if (band <= 0.0)
        discard;

    float2 uv = position.xy * gTexel;
    float2 o = 0.5 * gTexel;
    float inside = 0.25 * (gMask.Sample(gLinear, uv + float2(-o.x, -o.y)) + gMask.Sample(gLinear, uv + float2(o.x, -o.y)) +
                           gMask.Sample(gLinear, uv + float2(-o.x, o.y)) + gMask.Sample(gLinear, uv + float2(o.x, o.y)));
    return gColour * (band * (1.0 - inside));
}
)";

struct PassConstants
{
    float Colour[4];
    float Texel[2];
    float Radius;
    int32_t Reach;
    int32_t Size[2];
    int32_t Unused[2];
};
static_assert(sizeof(PassConstants) == 48);

// --- the D3D11 state we change, put back as it was (the game keeps its own record of what's bound)

struct SavedState
{
    UINT ScissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT ViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT Scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    D3D11_VIEWPORT Viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    ID3D11RasterizerState* pRasterizer = nullptr;
    ID3D11BlendState* pBlend = nullptr;
    float BlendFactor[4]{};
    UINT SampleMask = 0;
    ID3D11DepthStencilState* pDepthStencil = nullptr;
    UINT StencilRef = 0;
    ID3D11ShaderResourceView* pPixelResources[2]{};
    ID3D11SamplerState* pPixelSampler = nullptr;
    ID3D11PixelShader* pPixelShader = nullptr;
    ID3D11VertexShader* pVertexShader = nullptr;
    ID3D11GeometryShader* pGeometryShader = nullptr;
    ID3D11HullShader* pHullShader = nullptr;
    ID3D11DomainShader* pDomainShader = nullptr;
    ID3D11ClassInstance* pPixelInstances[256]{};
    ID3D11ClassInstance* pVertexInstances[256]{};
    ID3D11ClassInstance* pGeometryInstances[256]{};
    ID3D11ClassInstance* pHullInstances[256]{};
    ID3D11ClassInstance* pDomainInstances[256]{};
    UINT PixelInstanceCount = 256, VertexInstanceCount = 256, GeometryInstanceCount = 256, HullInstanceCount = 256, DomainInstanceCount = 256;
    ID3D11Buffer* pVertexConstants[2]{};
    ID3D11Buffer* pPixelConstants = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY Topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* pIndexBuffer = nullptr;
    DXGI_FORMAT IndexFormat = DXGI_FORMAT_UNKNOWN;
    UINT IndexOffset = 0;
    ID3D11Buffer* pVertexBuffer = nullptr;
    UINT VertexStride = 0;
    UINT VertexOffset = 0;
    ID3D11InputLayout* pInputLayout = nullptr;
    ID3D11RenderTargetView* pTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* pDepth = nullptr;

    void Save(ID3D11DeviceContext* apContext) noexcept
    {
        apContext->RSGetScissorRects(&ScissorCount, Scissors);
        apContext->RSGetViewports(&ViewportCount, Viewports);
        apContext->RSGetState(&pRasterizer);
        apContext->OMGetBlendState(&pBlend, BlendFactor, &SampleMask);
        apContext->OMGetDepthStencilState(&pDepthStencil, &StencilRef);
        apContext->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, pTargets, &pDepth);
        apContext->PSGetShaderResources(0, 2, pPixelResources);
        apContext->PSGetSamplers(0, 1, &pPixelSampler);
        apContext->PSGetShader(&pPixelShader, pPixelInstances, &PixelInstanceCount);
        apContext->VSGetShader(&pVertexShader, pVertexInstances, &VertexInstanceCount);
        apContext->GSGetShader(&pGeometryShader, pGeometryInstances, &GeometryInstanceCount);
        apContext->HSGetShader(&pHullShader, pHullInstances, &HullInstanceCount);
        apContext->DSGetShader(&pDomainShader, pDomainInstances, &DomainInstanceCount);
        apContext->VSGetConstantBuffers(0, 2, pVertexConstants);
        apContext->PSGetConstantBuffers(0, 1, &pPixelConstants);
        apContext->IAGetPrimitiveTopology(&Topology);
        apContext->IAGetIndexBuffer(&pIndexBuffer, &IndexFormat, &IndexOffset);
        apContext->IAGetVertexBuffers(0, 1, &pVertexBuffer, &VertexStride, &VertexOffset);
        apContext->IAGetInputLayout(&pInputLayout);
    }

    void Restore(ID3D11DeviceContext* apContext) noexcept
    {
        apContext->RSSetScissorRects(ScissorCount, Scissors);
        apContext->RSSetViewports(ViewportCount, Viewports);
        apContext->RSSetState(pRasterizer);
        apContext->OMSetBlendState(pBlend, BlendFactor, SampleMask);
        apContext->OMSetDepthStencilState(pDepthStencil, StencilRef);
        apContext->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, pTargets, pDepth);
        apContext->PSSetShaderResources(0, 2, pPixelResources);
        apContext->PSSetSamplers(0, 1, &pPixelSampler);
        apContext->PSSetShader(pPixelShader, pPixelInstances, PixelInstanceCount);
        apContext->VSSetShader(pVertexShader, pVertexInstances, VertexInstanceCount);
        apContext->GSSetShader(pGeometryShader, pGeometryInstances, GeometryInstanceCount);
        apContext->HSSetShader(pHullShader, pHullInstances, HullInstanceCount);
        apContext->DSSetShader(pDomainShader, pDomainInstances, DomainInstanceCount);
        apContext->VSSetConstantBuffers(0, 2, pVertexConstants);
        apContext->PSSetConstantBuffers(0, 1, &pPixelConstants);
        apContext->IASetPrimitiveTopology(Topology);
        apContext->IASetIndexBuffer(pIndexBuffer, IndexFormat, IndexOffset);
        apContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &VertexStride, &VertexOffset);
        apContext->IASetInputLayout(pInputLayout);
    }

    ~SavedState()
    {
        const auto release = [](auto* apObject)
        {
            if (apObject)
                apObject->Release();
        };
        release(pRasterizer);
        release(pBlend);
        release(pDepthStencil);
        for (auto* pTarget : pTargets)
            release(pTarget);
        release(pDepth);
        for (auto* pResource : pPixelResources)
            release(pResource);
        release(pPixelSampler);
        release(pPixelShader);
        release(pVertexShader);
        release(pGeometryShader);
        release(pHullShader);
        release(pDomainShader);
        for (UINT i = 0; i < PixelInstanceCount; ++i)
            release(pPixelInstances[i]);
        for (UINT i = 0; i < VertexInstanceCount; ++i)
            release(pVertexInstances[i]);
        for (UINT i = 0; i < GeometryInstanceCount; ++i)
            release(pGeometryInstances[i]);
        for (UINT i = 0; i < HullInstanceCount; ++i)
            release(pHullInstances[i]);
        for (UINT i = 0; i < DomainInstanceCount; ++i)
            release(pDomainInstances[i]);
        for (auto* pBuffer : pVertexConstants)
            release(pBuffer);
        release(pPixelConstants);
        release(pIndexBuffer);
        release(pVertexBuffer);
        release(pInputLayout);
    }
};

// --- the target (set from the pick, drawn at Present; both the main thread, the lock is only in case)

struct Target
{
    uint32_t Handle = 0;
    SelectionOutline::Style Look{};
    std::chrono::steady_clock::time_point Set{};
};
std::mutex s_targetLock;
Target s_target{};
SelectionOutline::Stats s_stats{};

// --- matrices

// A 3x4 transform (rows: rotation scaled, translation last)
using Rows = std::array<std::array<double, 4>, 3>;

// NiTransform (rotation rows of 4 floats, translation, scale) as rows. The game keeps the rotation transposed: its row i
// is where the local axis i points in the world (the world camera's row 0 is the way it looks), so world = rotation^T *
// local * scale + translation.
Rows ToRows(const uint8_t* apTransform) noexcept
{
    const float* pRotate = reinterpret_cast<const float*>(apTransform);
    const float* pTranslate = reinterpret_cast<const float*>(apTransform + 0x30);
    const double scale = *reinterpret_cast<const float*>(apTransform + 0x3C);
    Rows rows{};
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 3; ++c)
            rows[r][c] = pRotate[c * 4 + r] * scale;
        rows[r][3] = pTranslate[r];
    }
    return rows;
}

// a after b
Rows Combine(const Rows& acA, const Rows& acB) noexcept
{
    Rows rows{};
    for (int r = 0; r < 3; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            double value = c == 3 ? acA[r][3] : 0.0;
            for (int k = 0; k < 3; ++k)
                value += acA[r][k] * acB[k][c];
            rows[r][c] = value;
        }
    }
    return rows;
}

// The world to clip matrix after a 3x4 transform, as the shader's rows
void ToClip(const float (&acWorldToClip)[4][4], const Rows& acObject, float (&aOut)[4][4]) noexcept
{
    for (int r = 0; r < 4; ++r)
    {
        for (int c = 0; c < 4; ++c)
        {
            double value = c == 3 ? acWorldToClip[r][3] : 0.0;
            for (int k = 0; k < 3; ++k)
                value += static_cast<double>(acWorldToClip[r][k]) * acObject[k][c];
            aOut[r][c] = static_cast<float>(value);
        }
    }
}

// --- the meshes of a 3D

struct Mesh
{
    const uint8_t* pGeometry = nullptr;
    bool Lit = false;
};

bool IsLit(const uint8_t* apGeometry) noexcept
{
    static const VersionDbPtr<void> s_vtable(kLightingShaderPropertyVtable);
    for (int i = 0; i < 2; ++i)
    {
        const void* pProperty = Read<const void*>(apGeometry, kGeometryProperties + i * 8);
        if (pProperty && *static_cast<void* const*>(pProperty) == s_vtable.Get())
            return true;
    }
    return false;
}

// The shown tri shapes under a 3D (a switch node: only its shown child), lit ones marked
std::vector<Mesh> MeshesOf(const uint8_t* ap3D, SelectionOutline::Stats& aStats) noexcept
{
    std::vector<Mesh> meshes;
    std::vector<const uint8_t*> pending{ap3D};
    size_t visited = 0;
    while (!pending.empty() && meshes.size() < kMaxMeshes && ++visited < kMaxNodes)
    {
        const uint8_t* pObject = pending.back();
        pending.pop_back();
        if (!pObject || (Read<uint64_t>(pObject, kObjectFlags) & kObjectHidden))
            continue;

        if (CallVirtual<const void*>(pObject, kIsNode))
        {
            uint8_t* const* pChildren = Read<uint8_t* const*>(pObject, kNodeChildren);
            const uint16_t capacity = Read<uint16_t>(pObject, kNodeChildCapacity);
            if (!pChildren)
                continue;
            if (CallVirtual<const void*>(pObject, kIsSwitchNode))
            {
                const int32_t shown = Read<int32_t>(pObject, kSwitchIndex);
                if (shown >= 0 && shown < capacity)
                    pending.push_back(pChildren[shown]);
                continue;
            }
            for (uint16_t i = 0; i < capacity; ++i)
                pending.push_back(pChildren[i]);
            continue;
        }

        // particles, lines and the like aren't meshes; dynamic ones (faces) keep their vertices on the CPU
        if (!CallVirtual<const void*>(pObject, kIsTriShape))
            continue;
        if (CallVirtual<const void*>(pObject, kIsDynamicTriShape))
        {
            ++aStats.Skipped;
            continue;
        }
        meshes.push_back({pObject, IsLit(pObject)});
    }

    // only lit surfaces, unless there are none
    if (std::any_of(meshes.begin(), meshes.end(), [](const Mesh& acMesh) { return acMesh.Lit; }))
    {
        const auto effects = std::remove_if(meshes.begin(), meshes.end(), [](const Mesh& acMesh) { return !acMesh.Lit; });
        aStats.Effects += static_cast<uint32_t>(meshes.end() - effects);
        meshes.erase(effects, meshes.end());
    }
    return meshes;
}

// Where on the target the 3D can be (its bound, generously), in pixels; false when it's off the screen. Behind or around
// the eye: all of it.
bool ScreenRect(const uint8_t* ap3D, const float (&acWorldToClip)[4][4], UINT aWidth, UINT aHeight, int aPadding, D3D11_RECT& aOut) noexcept
{
    const float* pBound = reinterpret_cast<const float*>(ap3D + kWorldBound);
    const float radius = pBound[3] * 1.25f + 1.f;
    if (!std::isfinite(pBound[0]) || !std::isfinite(pBound[1]) || !std::isfinite(pBound[2]) || !std::isfinite(radius))
        return false;

    float left = 1e9f, top = 1e9f, right = -1e9f, bottom = -1e9f;
    // no bound worked out yet: all of it
    bool behind = !(pBound[3] > 0.f);
    for (int corner = 0; corner < 8 && !behind; ++corner)
    {
        const float point[3] = {pBound[0] + (corner & 1 ? radius : -radius), pBound[1] + (corner & 2 ? radius : -radius), pBound[2] + (corner & 4 ? radius : -radius)};
        float clip[4]{};
        for (int r = 0; r < 4; ++r)
            clip[r] = acWorldToClip[r][0] * point[0] + acWorldToClip[r][1] * point[1] + acWorldToClip[r][2] * point[2] + acWorldToClip[r][3];
        if (clip[3] < 1.f)
        {
            behind = true;
            break;
        }
        const float x = (clip[0] / clip[3] + 1.f) * 0.5f * aWidth;
        const float y = (1.f - clip[1] / clip[3]) * 0.5f * aHeight;
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
    }
    if (behind)
    {
        aOut = {0, 0, static_cast<LONG>(aWidth), static_cast<LONG>(aHeight)};
        return true;
    }

    aOut.left = std::clamp(static_cast<LONG>(std::floor(left)) - aPadding, 0L, static_cast<LONG>(aWidth));
    aOut.top = std::clamp(static_cast<LONG>(std::floor(top)) - aPadding, 0L, static_cast<LONG>(aHeight));
    aOut.right = std::clamp(static_cast<LONG>(std::ceil(right)) + aPadding, 0L, static_cast<LONG>(aWidth));
    aOut.bottom = std::clamp(static_cast<LONG>(std::ceil(bottom)) + aPadding, 0L, static_cast<LONG>(aHeight));
    return aOut.right > aOut.left && aOut.bottom > aOut.top;
}

// --- the renderer

struct OutlineRenderer
{
    ID3D11Device* pDevice = nullptr;
    bool Broken = false;

    ComPtr<ID3D11VertexShader> StaticVertexShader, SkinnedVertexShader, ScreenVertexShader;
    ComPtr<ID3DBlob> StaticBytecode, SkinnedBytecode;
    ComPtr<ID3D11PixelShader> MaskPixelShader, AcrossPixelShader, BandPixelShader;
    ComPtr<ID3D11Buffer> ObjectConstants, BoneConstants, PassBuffer;
    ComPtr<ID3D11RasterizerState> Rasterizer;
    ComPtr<ID3D11DepthStencilState> NoDepth;
    ComPtr<ID3D11BlendState> Opaque, Premultiplied;
    ComPtr<ID3D11SamplerState> Linear;
    // input layouts by position precision, skin and the skin data's offset
    std::map<uint32_t, ComPtr<ID3D11InputLayout>> Layouts;

    UINT Width = 0, Height = 0;
    ComPtr<ID3D11Texture2D> MaskTexture, AcrossTexture;
    ComPtr<ID3D11RenderTargetView> MaskTarget, AcrossTarget;
    ComPtr<ID3D11ShaderResourceView> MaskResource, AcrossResource;

    void Reset() noexcept
    {
        *this = OutlineRenderer{};
    }

    ComPtr<ID3DBlob> Compile(const char* acpName, const std::string& acSource, const char* acpTarget) noexcept
    {
        ComPtr<ID3DBlob> code, errors;
        if (FAILED(D3DCompile(acSource.data(), acSource.size(), acpName, nullptr, nullptr, "main", acpTarget, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
        {
            spdlog::error("Better Interact: the outline's {} shader didn't compile: {}", acpName, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no message");
            return nullptr;
        }
        return code;
    }

    bool Create(ID3D11Device* apDevice) noexcept
    {
        Reset();
        pDevice = apDevice;

        const auto staticCode = Compile("mask (static)", kStaticVertexShader, "vs_5_0");
        const auto skinnedCode = Compile("mask (skinned)", kSkinnedVertexShader, "vs_5_0");
        const auto screenCode = Compile("screen", kScreenVertexShader, "vs_5_0");
        const auto maskCode = Compile("mask", kMaskPixelShader, "ps_5_0");
        const auto acrossCode = Compile("across", std::string(kPassConstants) + kAcrossPixelShader, "ps_5_0");
        const auto bandCode = Compile("band", std::string(kPassConstants) + kBandPixelShader, "ps_5_0");
        if (!staticCode || !skinnedCode || !screenCode || !maskCode || !acrossCode || !bandCode)
            return false;
        StaticBytecode = staticCode;
        SkinnedBytecode = skinnedCode;

        bool ok = SUCCEEDED(apDevice->CreateVertexShader(staticCode->GetBufferPointer(), staticCode->GetBufferSize(), nullptr, &StaticVertexShader)) &&
                  SUCCEEDED(apDevice->CreateVertexShader(skinnedCode->GetBufferPointer(), skinnedCode->GetBufferSize(), nullptr, &SkinnedVertexShader)) &&
                  SUCCEEDED(apDevice->CreateVertexShader(screenCode->GetBufferPointer(), screenCode->GetBufferSize(), nullptr, &ScreenVertexShader)) &&
                  SUCCEEDED(apDevice->CreatePixelShader(maskCode->GetBufferPointer(), maskCode->GetBufferSize(), nullptr, &MaskPixelShader)) &&
                  SUCCEEDED(apDevice->CreatePixelShader(acrossCode->GetBufferPointer(), acrossCode->GetBufferSize(), nullptr, &AcrossPixelShader)) &&
                  SUCCEEDED(apDevice->CreatePixelShader(bandCode->GetBufferPointer(), bandCode->GetBufferSize(), nullptr, &BandPixelShader));

        const auto makeConstants = [apDevice](UINT aSize, ComPtr<ID3D11Buffer>& aOut)
        {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = aSize;
            desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            return SUCCEEDED(apDevice->CreateBuffer(&desc, nullptr, &aOut));
        };
        ok = ok && makeConstants(sizeof(float) * 16, ObjectConstants) && makeConstants(static_cast<UINT>(sizeof(float) * 12 * kMaxBones), BoneConstants) &&
             makeConstants(sizeof(PassConstants), PassBuffer);

        D3D11_RASTERIZER_DESC rasterizer{};
        rasterizer.FillMode = D3D11_FILL_SOLID;
        rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        rasterizer.ScissorEnable = TRUE;
        ok = ok && SUCCEEDED(apDevice->CreateRasterizerState(&rasterizer, &Rasterizer));

        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = FALSE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ok = ok && SUCCEEDED(apDevice->CreateDepthStencilState(&depth, &NoDepth));

        D3D11_BLEND_DESC blend{};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = ok && SUCCEEDED(apDevice->CreateBlendState(&blend, &Opaque));
        // over the frame, premultiplied, the frame's alpha left as it was
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
        blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        ok = ok && SUCCEEDED(apDevice->CreateBlendState(&blend, &Premultiplied));

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        ok = ok && SUCCEEDED(apDevice->CreateSamplerState(&sampler, &Linear));

        if (!ok)
            spdlog::error("Better Interact: the outline's shaders or states couldn't be made");
        return ok;
    }

    bool EnsureTargets(UINT aWidth, UINT aHeight) noexcept
    {
        if (MaskTexture && aWidth == Width && aHeight == Height)
            return true;
        MaskTexture.Reset();
        AcrossTexture.Reset();
        MaskTarget.Reset();
        AcrossTarget.Reset();
        MaskResource.Reset();
        AcrossResource.Reset();
        Width = aWidth;
        Height = aHeight;

        const auto make = [this](DXGI_FORMAT aFormat, ComPtr<ID3D11Texture2D>& aTexture, ComPtr<ID3D11RenderTargetView>& aTarget, ComPtr<ID3D11ShaderResourceView>& aResource)
        {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = Width;
            desc.Height = Height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = aFormat;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            return SUCCEEDED(pDevice->CreateTexture2D(&desc, nullptr, &aTexture)) && SUCCEEDED(pDevice->CreateRenderTargetView(aTexture.Get(), nullptr, &aTarget)) &&
                   SUCCEEDED(pDevice->CreateShaderResourceView(aTexture.Get(), nullptr, &aResource));
        };
        if (make(DXGI_FORMAT_R8_UNORM, MaskTexture, MaskTarget, MaskResource) && make(DXGI_FORMAT_R16_FLOAT, AcrossTexture, AcrossTarget, AcrossResource))
        {
            spdlog::info("Better Interact: outline targets {}x{}", Width, Height);
            return true;
        }
        spdlog::error("Better Interact: the outline's {}x{} targets couldn't be made", Width, Height);
        MaskTexture.Reset();
        return false;
    }

    ID3D11InputLayout* LayoutFor(bool aFullPrecision, bool aSkinned, UINT aSkinOffset) noexcept
    {
        const uint32_t key = (aFullPrecision ? 1u : 0u) | (aSkinned ? 2u : 0u) | (aSkinned ? aSkinOffset << 8 : 0u);
        if (const auto it = Layouts.find(key); it != Layouts.end())
            return it->second.Get();

        const DXGI_FORMAT position = aFullPrecision ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT;
        // skinned vertices: four half weights then four byte bone indices
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, position, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BLENDWEIGHT", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, aSkinOffset, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BLENDINDICES", 0, DXGI_FORMAT_R8G8B8A8_UINT, 0, aSkinOffset + 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        ID3DBlob* pCode = aSkinned ? SkinnedBytecode.Get() : StaticBytecode.Get();
        ComPtr<ID3D11InputLayout> layout;
        if (FAILED(pDevice->CreateInputLayout(elements, aSkinned ? 3 : 1, pCode->GetBufferPointer(), pCode->GetBufferSize(), &layout)))
            layout = nullptr;
        Layouts[key] = layout;
        return layout.Get();
    }

    template <class T> bool Upload(ID3D11DeviceContext* apContext, ID3D11Buffer* apBuffer, const T* apData, size_t aSize) noexcept
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(apContext->Map(apBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        std::memcpy(mapped.pData, apData, aSize);
        apContext->Unmap(apBuffer, 0);
        return true;
    }

    // One game buffer as D3D11 sees it: a buffer of ours' kind (a layout we got wrong would show here), big enough
    bool BufferOf(const uint8_t* apGameBuffer, UINT aBind, UINT aBytes, ID3D11Buffer*& aBuffer, UINT& aOffset) const noexcept
    {
        if (!apGameBuffer || Read<uint32_t>(apGameBuffer, kBufferPending) || Read<uint8_t>(apGameBuffer, kBufferUploading))
            return false;
        auto* pBuffer = Read<ID3D11Buffer*>(apGameBuffer, 0);
        if (!pBuffer || *reinterpret_cast<void**>(pBuffer) != *reinterpret_cast<void**>(ObjectConstants.Get()))
            return false;
        D3D11_BUFFER_DESC desc{};
        pBuffer->GetDesc(&desc);
        const UINT offset = Read<uint32_t>(apGameBuffer, kBufferOffset);
        if (!(desc.BindFlags & aBind) || static_cast<uint64_t>(offset) + aBytes > desc.ByteWidth)
            return false;
        aBuffer = pBuffer;
        aOffset = offset;
        return true;
    }

    // Draws a mesh into the mask (its shader and constants set here); false when it can't be
    bool DrawMesh(ID3D11DeviceContext* apContext, const uint8_t* apGeometry, const float (&acWorldToClip)[4][4], SelectionOutline::Stats& aStats) noexcept
    {
        const uint8_t* pData = Read<const uint8_t*>(apGeometry, kGeometryRendererData);
        if (!pData)
            return false;
        const uint64_t vertexDesc = Read<uint64_t>(pData, 0);
        const UINT stride = static_cast<UINT>(vertexDesc & 0xF) * 4;
        const uint64_t flags = vertexDesc >> 44;
        const UINT skinOffset = static_cast<UINT>(vertexDesc >> 26) & 0x3C;
        const uint32_t triangles = Read<uint32_t>(apGeometry, kTriShapeTriangles);
        if (stride == 0 || triangles == 0)
            return false;

        // the vertex buffer's size isn't kept with the shape; the index buffer has the triangles'
        ID3D11Buffer* pVertices = nullptr;
        ID3D11Buffer* pIndices = nullptr;
        UINT vertexOffset = 0, indexOffset = 0;
        if (!BufferOf(Read<const uint8_t*>(pData, kRendererVertices), D3D11_BIND_VERTEX_BUFFER, stride, pVertices, vertexOffset) ||
            !BufferOf(Read<const uint8_t*>(pData, kRendererIndices), D3D11_BIND_INDEX_BUFFER, triangles * 3 * 2, pIndices, indexOffset))
            return false;

        const uint8_t* pSkin = Read<const uint8_t*>(apGeometry, kGeometrySkin);
        const bool skinned = (flags & kVertexSkinned) && pSkin;
        if ((flags & kVertexSkinned) && !pSkin)
            return false;

        if (skinned)
        {
            // each bone's skin to world: its world transform after its bind data
            const uint32_t boneCount = Read<uint32_t>(pSkin, kSkinBoneCount);
            const uint8_t* const* pWorlds = Read<const uint8_t* const*>(pSkin, kSkinBoneWorlds);
            const uint8_t* pBoneData = Read<const uint8_t*>(pSkin, kSkinBoneData);
            if (boneCount == 0 || boneCount > kMaxBones || !pWorlds || !pBoneData || Read<uint32_t>(pBoneData, kBoneDataCount) < boneCount)
                return false;
            const uint8_t* pEntries = Read<const uint8_t*>(pBoneData, kBoneDataEntries);
            if (!pEntries)
                return false;

            static std::array<float, 12 * kMaxBones> s_bones{};
            for (uint32_t i = 0; i < boneCount; ++i)
            {
                if (!pWorlds[i])
                    return false;
                const Rows bone = Combine(ToRows(pWorlds[i]), ToRows(pEntries + i * kBoneDataStride + kBoneDataTransform));
                for (int r = 0; r < 3; ++r)
                {
                    for (int c = 0; c < 4; ++c)
                        s_bones[i * 12 + r * 4 + c] = static_cast<float>(bone[r][c]);
                }
            }
            if (!Upload(apContext, ObjectConstants.Get(), &acWorldToClip[0][0], sizeof(acWorldToClip)) ||
                !Upload(apContext, BoneConstants.Get(), s_bones.data(), sizeof(float) * 12 * boneCount))
                return false;
        }
        else
        {
            float toClip[4][4]{};
            ToClip(acWorldToClip, ToRows(apGeometry + kWorldTransform), toClip);
            if (!Upload(apContext, ObjectConstants.Get(), &toClip[0][0], sizeof(toClip)))
                return false;
        }

        ID3D11InputLayout* pLayout = LayoutFor(flags & kVertexFullPrecision, skinned, skinOffset);
        if (!pLayout)
            return false;

        apContext->VSSetShader(skinned ? SkinnedVertexShader.Get() : StaticVertexShader.Get(), nullptr, 0);
        apContext->IASetInputLayout(pLayout);
        apContext->IASetVertexBuffers(0, 1, &pVertices, &stride, &vertexOffset);
        apContext->IASetIndexBuffer(pIndices, DXGI_FORMAT_R16_UINT, indexOffset);
        apContext->DrawIndexed(triangles * 3, 0, 0);
        ++(skinned ? aStats.Skinned : aStats.Static);
        return true;
    }
};
OutlineRenderer s_renderer{};

bool Fresh(const Target& acTarget) noexcept
{
    return acTarget.Handle && std::chrono::steady_clock::now() - acTarget.Set < kFresh;
}
} // namespace

void SelectionOutline::Set(uint32_t aHandle, const Style& acStyle) noexcept
{
    std::scoped_lock _(s_targetLock);
    s_target = {aHandle, acStyle, std::chrono::steady_clock::now()};
}

void SelectionOutline::Clear() noexcept
{
    std::scoped_lock _(s_targetLock);
    s_target = {};
}

uint32_t SelectionOutline::Current() noexcept
{
    std::scoped_lock _(s_targetLock);
    return Fresh(s_target) ? s_target.Handle : 0;
}

SelectionOutline::Stats SelectionOutline::LastStats() noexcept
{
    std::scoped_lock _(s_targetLock);
    return s_stats;
}

void SelectionOutline::Draw() noexcept
{
    // the shaders are built from the first frame on (the main menu), not on the first pick
    BGSRenderer* pRenderer = BGSRenderer::Get();
    if (!pRenderer || !pRenderer->pD3dDevice || !pRenderer->pD3dContext || s_renderer.Broken)
        return;
    if (s_renderer.pDevice != pRenderer->pD3dDevice)
    {
        if (!s_renderer.Create(pRenderer->pD3dDevice))
        {
            s_renderer.Broken = true;
            return;
        }
        spdlog::info("Better Interact: outline shaders ready");
    }

    Target target{};
    {
        std::scoped_lock _(s_targetLock);
        if (!Fresh(s_target))
        {
            s_stats = {};
            return;
        }
        target = s_target;
    }

    Stats stats{};
    const auto stop = [&stats](const char* acpReason)
    {
        stats.Stopped = acpReason;
        std::scoped_lock _(s_targetLock);
        s_stats = stats;
    };

    TESObjectREFR* pRef = TESObjectREFR::GetByHandle(target.Handle);
    const auto* p3D = pRef ? CallVirtual<const uint8_t*>(pRef, kGet3D) : nullptr;
    BSGraphics::RendererWindow* pWindow = BSGraphics::GetMainWindow();
    // the frame's view: the game keeps no texture with the swap chain's target (0 there), so its size comes from the
    // view's own texture, or else the window's
    ID3D11RenderTargetView* pFrameView = pWindow ? pWindow->SwapChainRenderTarget.pRTView : nullptr;
    float worldToClip[4][4]{};
    if (!p3D)
        return stop("no 3D");
    if (!pFrameView)
        return stop("no frame");
    if (!HudMarker::GetWorldToClip(worldToClip))
        return stop("no camera");

    D3D11_TEXTURE2D_DESC frame{};
    ComPtr<ID3D11Resource> frameResource;
    ComPtr<ID3D11Texture2D> frameTexture;
    pFrameView->GetResource(&frameResource);
    if (frameResource && SUCCEEDED(frameResource.As(&frameTexture)))
        frameTexture->GetDesc(&frame);
    else
    {
        frame.Width = static_cast<UINT>(std::max(0, pWindow->uiWindowWidth));
        frame.Height = static_cast<UINT>(std::max(0, pWindow->uiWindowHeight));
    }
    if (frame.Width == 0 || frame.Height == 0 || !s_renderer.EnsureTargets(frame.Width, frame.Height))
        return stop("no frame size");

    const float radius = std::max(1, target.Look.Width) * std::max(1.f, frame.Height / kReferenceHeight);
    const int reach = std::min(kMaxReach, static_cast<int>(std::ceil(radius)) + 1);
    D3D11_RECT rect{};
    if (!ScreenRect(p3D, worldToClip, frame.Width, frame.Height, reach + 2, rect))
        return stop("off screen");
    const std::vector<Mesh> meshes = MeshesOf(p3D, stats);
    if (meshes.empty())
        return stop("no meshes");

    ID3D11DeviceContext* pContext = pRenderer->pD3dContext;
    SavedState saved{};
    saved.Save(pContext);

    const D3D11_VIEWPORT viewport{0.f, 0.f, static_cast<float>(frame.Width), static_cast<float>(frame.Height), 0.f, 1.f};
    pContext->RSSetViewports(1, &viewport);
    pContext->RSSetScissorRects(1, &rect);
    pContext->RSSetState(s_renderer.Rasterizer.Get());
    pContext->OMSetDepthStencilState(s_renderer.NoDepth.Get(), 0);
    constexpr float kNoFactor[4] = {0.f, 0.f, 0.f, 0.f};
    pContext->OMSetBlendState(s_renderer.Opaque.Get(), kNoFactor, 0xFFFFFFFF);
    pContext->GSSetShader(nullptr, nullptr, 0);
    pContext->HSSetShader(nullptr, nullptr, 0);
    pContext->DSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* const kNoResources[2] = {nullptr, nullptr};
    pContext->PSSetShaderResources(0, 2, kNoResources);

    // the mask: the meshes, covered pixels 1
    constexpr float kEmpty[4] = {0.f, 0.f, 0.f, 0.f};
    pContext->ClearRenderTargetView(s_renderer.MaskTarget.Get(), kEmpty);
    pContext->OMSetRenderTargets(1, s_renderer.MaskTarget.GetAddressOf(), nullptr);
    pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    pContext->PSSetShader(s_renderer.MaskPixelShader.Get(), nullptr, 0);
    ID3D11Buffer* const vertexConstants[2] = {s_renderer.ObjectConstants.Get(), s_renderer.BoneConstants.Get()};
    pContext->VSSetConstantBuffers(0, 2, vertexConstants);
    for (const Mesh& mesh : meshes)
    {
        if (!s_renderer.DrawMesh(pContext, mesh.pGeometry, worldToClip, stats))
            ++stats.Skipped;
    }

    if (stats.Static + stats.Skinned > 0)
    {
        PassConstants pass{};
        const float opacity = std::clamp(target.Look.Opacity, 0.f, 1.f);
        pass.Colour[0] = std::clamp(target.Look.Red, 0.f, 1.f) * opacity;
        pass.Colour[1] = std::clamp(target.Look.Green, 0.f, 1.f) * opacity;
        pass.Colour[2] = std::clamp(target.Look.Blue, 0.f, 1.f) * opacity;
        pass.Colour[3] = opacity;
        pass.Texel[0] = 1.f / frame.Width;
        pass.Texel[1] = 1.f / frame.Height;
        pass.Radius = radius;
        pass.Reach = reach;
        pass.Size[0] = static_cast<int32_t>(frame.Width);
        pass.Size[1] = static_cast<int32_t>(frame.Height);
        s_renderer.Upload(pContext, s_renderer.PassBuffer.Get(), &pass, sizeof(pass));

        ID3D11Buffer* const kNoBuffer = nullptr;
        constexpr UINT kZero = 0;
        pContext->IASetInputLayout(nullptr);
        pContext->IASetVertexBuffers(0, 1, &kNoBuffer, &kZero, &kZero);
        pContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        pContext->VSSetShader(s_renderer.ScreenVertexShader.Get(), nullptr, 0);
        pContext->PSSetConstantBuffers(0, 1, s_renderer.PassBuffer.GetAddressOf());

        // across: rows outside the drawn rectangle stay "nothing within reach"
        const float nothingNear[4] = {static_cast<float>(reach + 1), 0.f, 0.f, 0.f};
        pContext->ClearRenderTargetView(s_renderer.AcrossTarget.Get(), nothingNear);
        pContext->OMSetRenderTargets(1, s_renderer.AcrossTarget.GetAddressOf(), nullptr);
        pContext->PSSetShaderResources(0, 1, s_renderer.MaskResource.GetAddressOf());
        pContext->PSSetShader(s_renderer.AcrossPixelShader.Get(), nullptr, 0);
        pContext->Draw(3, 0);

        // down, and the band over the frame
        pContext->OMSetRenderTargets(1, &pFrameView, nullptr);
        pContext->OMSetBlendState(s_renderer.Premultiplied.Get(), kNoFactor, 0xFFFFFFFF);
        ID3D11ShaderResourceView* const resources[2] = {s_renderer.MaskResource.Get(), s_renderer.AcrossResource.Get()};
        pContext->PSSetShaderResources(0, 2, resources);
        pContext->PSSetSamplers(0, 1, s_renderer.Linear.GetAddressOf());
        pContext->PSSetShader(s_renderer.BandPixelShader.Get(), nullptr, 0);
        pContext->Draw(3, 0);
        stats.Drawn = true;
    }

    pContext->PSSetShaderResources(0, 2, kNoResources);
    saved.Restore(pContext);

    stop(stats.Drawn ? "" : "no mesh drawn");
}
