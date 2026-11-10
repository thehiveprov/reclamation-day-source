#include <ShadowFeatures.h>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

// GPU-driven shadows. For each caster in a sun cascade the game sets its shader and constants and makes a draw call of its own.
// D3D11 has no multi-draw without vendor extensions, so the casters that use the same mesh go through one instanced draw here,
// from a small vertex shader that only places positions, with what places each one in a buffer: that's the per-object state setup
// gone, and one draw for all the copies of a mesh. The depth that comes out is the same as the game's: the same vertices, the same
// transform, and the same depth state its shadow technique sets (1428BA230, bit 0x4000).
//
// Only what's drawn that plainly is taken: a BSTriShape, nothing skinned or moving by vertices, no alpha test or transparency, one
// sided, no vertex animation. Anything else goes to the game as it always has. It's a choice, off until asked for; docs/RENDERING.md.

using Microsoft::WRL::ComPtr;

namespace
{
constexpr uint32_t kPrepareBuffer = 1071711;  // waits for a mesh buffer that's still being filled, before it's bound (141D09810)
constexpr uint32_t kLightingVtable = 241916;  // BSLightingShaderProperty
constexpr uint32_t kFrameStamp = 1500400;     // what the game's accumulate step stamps a property with

// BSGeometry
constexpr size_t kGeometryWorldRows = 0x70;
constexpr size_t kGeometryWorldTranslate = 0xA0;
constexpr size_t kGeometryWorldScale = 0xAC;
constexpr size_t kGeometryFlags = 0x108;
constexpr size_t kGeometryAlpha = 0x130;
constexpr size_t kGeometrySkin = 0x140;
constexpr size_t kGeometryData = 0x148;
constexpr size_t kGeometryType = 0x158;
constexpr size_t kGeometryTriangles = 0x160;
constexpr size_t kGeometrySkinVirtual = 0x1F8; // non-null: its vertices come from elsewhere (1428BAB50 goes by this)
constexpr uint8_t kTypeTriShape = 3;
constexpr uint64_t kDynamicMesh = 1ull << 0x22;
// NiAlphaProperty's flags
constexpr size_t kAlphaFlags = 0x28;
constexpr uint16_t kAlphaBlend = 1;
constexpr uint16_t kAlphaTest = 1 << 9;

// BSLightingShaderProperty, as its shadow passes are chosen (1427CEF20)
constexpr size_t kPropertyFlags = 0x30;
constexpr size_t kPropertyMaterial = 0x48;
constexpr size_t kPropertySpecial = 0x50;
constexpr size_t kPropertyAlphaHolder = 0x58;
constexpr size_t kPropertyStamp = 0x60;
constexpr size_t kMaterialAlpha = 0x1A0;
constexpr size_t kHolderAlpha = 0x80;
constexpr uint64_t kFlagCastsShadow = 1ull << 9;
// None of these: not drawn as a shadow at all, or drawn with something more than its positions (two sided, skinned, vertex
// animation, projected UVs, its own technique...)
constexpr uint64_t kFlagsRefused = 0xC008004ull | 0x4000000ull | 0x1000000000ull | 0x400000000ull | 0x2000000000ull | 2ull | 0x4000ull | 0x20000ull | 0x2000000000000000ull | 0x1000000000000000ull |
                                   0x10000000000000ull | 0x200000000000000ull;

// The mesh's data: the vertex format, then the two buffers. A buffer's first member is the D3D11 buffer, its place in that at +0x48
constexpr size_t kDataVertices = 0x8;
constexpr size_t kDataIndices = 0x10;
constexpr size_t kBufferOffset = 0x48;
constexpr uint64_t kVertexHasPosition = 1ull << 44;
constexpr uint64_t kVertexFullPrecision = 1ull << 54;

// NiCamera
constexpr size_t kCameraForward = 0x70;
constexpr size_t kCameraUp = 0x80;
constexpr size_t kCameraRight = 0x90;
constexpr size_t kCameraPosition = 0xA0;
constexpr size_t kCameraFrustum = 0x160; // left, right, top, bottom, near, far
constexpr size_t kCameraOrtho = 0x178;

// The game's render state: what its flush applies, and the bit in its dirty mask that says so
constexpr size_t kStateDirty = 0x1B70;
constexpr size_t kStateCull = 0x1C2C;
constexpr size_t kStateDepthBias = 0x1C30;
constexpr size_t kStateBlendFirst = 0x1C38;
constexpr size_t kStateBlendAlpha = 0x1C40;
constexpr uint32_t kDirtyRaster = 8;
constexpr uint32_t kDirtyBlend = 0x10;
constexpr uint32_t kShadowDepthBias = 8; // as the shadow technique sets it
constexpr uint32_t kCullBack = 1;        // the default its geometry step puts back

constexpr UINT kInstanceStride = 12 * sizeof(float);
constexpr UINT kFirstCapacity = 4096;

constexpr char kVertexShader[] = R"(
struct In
{
    float4 Position : POSITION;
    float4 X : PLACE0;
    float4 Y : PLACE1;
    float4 Z : PLACE2;
};
float4 main(In aIn) : SV_POSITION
{
    float4 position = float4(aIn.Position.xyz, 1.0);
    return float4(dot(aIn.X, position), dot(aIn.Y, position), dot(aIn.Z, position), 1.0);
}
)";

template <class T> T Read(const void* apObject, size_t aOffset) noexcept
{
    return *reinterpret_cast<const T*>(static_cast<const uint8_t*>(apObject) + aOffset);
}

template <class T> T& Field(void* apObject, size_t aOffset) noexcept
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(apObject) + aOffset);
}

struct Caster
{
    const uint8_t* pData;
    uint32_t Triangles;
    float Rows[3][3];
    float Translate[3];
    float Scale;
};

// One for each cascade's accumulator and for its moving casters' accumulator. Casters can arrive from the accumulation's worker threads.
struct CasterList
{
    SRWLOCK Lock = SRWLOCK_INIT;
    std::vector<Caster> Casters;
};
CasterList s_lists[ShadowFeatures::kMaxCascades * 2];

bool s_wanted = false;
bool s_broken = false;
void* s_lightingVtable = nullptr;

ComPtr<ID3D11VertexShader> s_shader;
ComPtr<ID3D11InputLayout> s_layoutHalf;
ComPtr<ID3D11InputLayout> s_layoutFull;
ComPtr<ID3D11Buffer> s_instances;
UINT s_capacity = 0;

std::atomic<uint64_t> s_taken{0};
std::atomic<uint64_t> s_draws{0};
std::vector<float> s_scratch;

CasterList* ListOf(const void* apAccumulator) noexcept
{
    const int cascade = ShadowFeatures::CascadeOfAccumulator(apAccumulator);
    if (cascade < 0)
        return nullptr;
    const bool moving = ShadowFeatures::SunCascades()[cascade].pDynamicAccumulator == apAccumulator;
    return &s_lists[cascade * 2 + (moving ? 1 : 0)];
}

bool MakeShader(ID3D11Device* apDevice) noexcept
{
    if (s_shader)
        return true;
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(kVertexShader, sizeof(kVertexShader) - 1, "gpushadows", nullptr, nullptr, "main", "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        spdlog::warn("GPU-driven shadows: off, its shader didn't compile: {}", errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return false;
    }
    if (FAILED(apDevice->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &s_shader)))
    {
        spdlog::warn("GPU-driven shadows: off, its shader couldn't be made");
        return false;
    }
    // The position formats the game's own layouts use for this flag (141D15E90)
    D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"PLACE", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"PLACE", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"PLACE", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    if (FAILED(apDevice->CreateInputLayout(elements, 4, code->GetBufferPointer(), code->GetBufferSize(), &s_layoutHalf)))
    {
        spdlog::warn("GPU-driven shadows: off, its vertex layout couldn't be made");
        s_shader.Reset();
        return false;
    }
    elements[0].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (FAILED(apDevice->CreateInputLayout(elements, 4, code->GetBufferPointer(), code->GetBufferSize(), &s_layoutFull)))
    {
        spdlog::warn("GPU-driven shadows: off, its vertex layout couldn't be made");
        s_shader.Reset();
        return false;
    }
    return true;
}

bool EnsureInstances(ID3D11Device* apDevice, UINT aCount) noexcept
{
    if (aCount <= s_capacity && s_instances)
        return true;
    const UINT capacity = std::max<UINT>(aCount * 2, kFirstCapacity);
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = capacity * kInstanceStride;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    s_instances.Reset();
    s_capacity = 0;
    if (FAILED(apDevice->CreateBuffer(&desc, nullptr, &s_instances)))
    {
        spdlog::warn("GPU-driven shadows: off, a buffer for {} casters couldn't be made", capacity);
        return false;
    }
    s_capacity = capacity;
    return true;
}

float Dot3(const float* apA, const float* apB) noexcept
{
    return apA[0] * apB[0] + apA[1] * apB[1] + apA[2] * apB[2];
}

// The game's state, set its own way so its flush applies it and its later draws see what it has to change back
void SetState(uint8_t* apState, size_t aField, uint32_t aValue, uint32_t aDirty) noexcept
{
    if (Field<uint32_t>(apState, aField) != aValue)
    {
        Field<uint32_t>(apState, aField) = aValue;
        Field<uint32_t>(apState, kStateDirty) |= aDirty;
    }
}
} // namespace

bool GpuShadows::Enabled() noexcept
{
    return s_wanted && !s_broken;
}

void GpuShadows::Refresh() noexcept
{
    const bool wanted = ShadowFeatures::ReadSetting(L"bGpuShadows", 0) != 0;
    if (wanted && !s_lightingVtable && !s_broken)
    {
        static constexpr uint8_t kPrepareStart[] = {0x48, 0x85, 0xC9, 0x74, 0x79, 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC};
        if (!ShadowFeatures::Matches("GPU-driven shadows", "the mesh buffers' wait", kPrepareBuffer, kPrepareStart, sizeof(kPrepareStart)))
        {
            s_broken = true;
            return;
        }
        const VersionDbPtr<uint8_t> vtable(kLightingVtable);
        s_lightingVtable = vtable.Get();
    }
    if (wanted != s_wanted)
        spdlog::info("GPU-driven shadows: {}", wanted ? "on" : "off");
    s_wanted = wanted;
}

bool GpuShadows::TryAdd(void* apAccumulator, void* apGeometry, void* apProperty) noexcept
{
    if (!apGeometry || !apProperty || *static_cast<void**>(apProperty) != s_lightingVtable)
        return false;

    const uint64_t flags = Read<uint64_t>(apProperty, kPropertyFlags);
    if (!(flags & kFlagCastsShadow) || (flags & kFlagsRefused))
        return false;
    // (seen through: the game has no shadow from it)
    const void* pMaterial = Read<const void*>(apProperty, kPropertyMaterial);
    const void* pHolder = Read<const void*>(apProperty, kPropertyAlphaHolder);
    if (!pMaterial || !pHolder || Read<float>(pMaterial, kMaterialAlpha) * Read<float>(pHolder, kHolderAlpha) < 1.f)
        return false;
    const void* pSpecial = Read<const void*>(apProperty, kPropertySpecial);
    if (pSpecial && Read<const void*>(pSpecial, 0x20))
        return false;

    if (Read<uint8_t>(apGeometry, kGeometryType) != kTypeTriShape || Read<const void*>(apGeometry, kGeometrySkin) || (Read<uint64_t>(apGeometry, kGeometryFlags) & kDynamicMesh))
        return false;
    const void* pAlpha = Read<const void*>(apGeometry, kGeometryAlpha);
    if (pAlpha && (Read<uint16_t>(pAlpha, kAlphaFlags) & (kAlphaBlend | kAlphaTest)))
        return false;
    using TSkinVirtual = void*(void*);
    if ((*reinterpret_cast<TSkinVirtual***>(apGeometry))[kGeometrySkinVirtual / sizeof(void*)](apGeometry))
        return false;

    const auto* pData = Read<const uint8_t*>(apGeometry, kGeometryData);
    const uint32_t triangles = Read<uint32_t>(apGeometry, kGeometryTriangles);
    if (!pData || !triangles)
        return false;
    const uint64_t format = Read<uint64_t>(pData, 0);
    const void* pVertices = Read<const void*>(pData, kDataVertices);
    const void* pIndices = Read<const void*>(pData, kDataIndices);
    if (!(format & kVertexHasPosition) || !((format << 2) & 0x3C) || !pVertices || !pIndices || !Read<const void*>(pVertices, 0) || !Read<const void*>(pIndices, 0))
        return false;

    CasterList* pList = ListOf(apAccumulator);
    if (!pList)
        return false;
    const auto& cascade = ShadowFeatures::SunCascades()[ShadowFeatures::CascadeOfAccumulator(apAccumulator)];
    if (!cascade.pCamera || !Read<uint8_t>(cascade.pCamera, kCameraOrtho))
        return false;

    Caster caster;
    caster.pData = pData;
    caster.Triangles = triangles;
    const auto* pRows = reinterpret_cast<const float*>(static_cast<const uint8_t*>(apGeometry) + kGeometryWorldRows);
    for (int row = 0; row < 3; ++row)
    {
        for (int axis = 0; axis < 3; ++axis)
            caster.Rows[row][axis] = pRows[row * 4 + axis];
    }
    std::memcpy(caster.Translate, static_cast<const uint8_t*>(apGeometry) + kGeometryWorldTranslate, sizeof(caster.Translate));
    caster.Scale = Read<float>(apGeometry, kGeometryWorldScale);

    AcquireSRWLockExclusive(&pList->Lock);
    pList->Casters.push_back(caster);
    ReleaseSRWLockExclusive(&pList->Lock);

    // What the game's own step leaves on the property
    const VersionDbPtr<uint32_t> stamp(kFrameStamp);
    Field<uint32_t>(apProperty, kPropertyStamp) = *stamp.Get();
    s_taken.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void GpuShadows::Draw(void* apAccumulator, void* apCamera) noexcept
{
    CasterList* pList = ListOf(apAccumulator);
    if (!pList || pList->Casters.empty())
        return;
    auto& casters = pList->Casters;

    auto* pDevice = ShadowFeatures::Device();
    auto* pContext = ShadowFeatures::ThreadContext();
    if (!pDevice || !pContext || !apCamera || !Read<uint8_t>(apCamera, kCameraOrtho) || !MakeShader(pDevice) || !EnsureInstances(pDevice, static_cast<UINT>(casters.size())))
    {
        // (the next frame's casters go to the game: better a shadow the game draws than none)
        s_broken = !s_shader || !s_instances;
        casters.clear();
        return;
    }

    std::sort(casters.begin(), casters.end(), [](const Caster& a, const Caster& b) { return a.pData != b.pData ? a.pData < b.pData : a.Triangles < b.Triangles; });

    // This camera's projection (141D216F0, orthographic), worked out relative to it so a place far out in the world keeps its precision
    float eye[3], forward[3], up[3], right[3];
    std::memcpy(eye, static_cast<const uint8_t*>(apCamera) + kCameraPosition, sizeof(eye));
    std::memcpy(forward, static_cast<const uint8_t*>(apCamera) + kCameraForward, sizeof(forward));
    std::memcpy(up, static_cast<const uint8_t*>(apCamera) + kCameraUp, sizeof(up));
    std::memcpy(right, static_cast<const uint8_t*>(apCamera) + kCameraRight, sizeof(right));
    const auto* pFrustum = reinterpret_cast<const float*>(static_cast<const uint8_t*>(apCamera) + kCameraFrustum);
    const float widthInverse = 1.f / (pFrustum[1] - pFrustum[0]);
    const float heightInverse = 1.f / (pFrustum[2] - pFrustum[3]);
    const float depthInverse = 1.f / (pFrustum[5] - pFrustum[4]);
    const float scaleX = 2.f * widthInverse, moveX = -(pFrustum[0] + pFrustum[1]) * widthInverse;
    const float scaleY = 2.f * heightInverse, moveY = -(pFrustum[2] + pFrustum[3]) * heightInverse;
    const float scaleZ = depthInverse, moveZ = -pFrustum[4] * depthInverse;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(pContext->Map(s_instances.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        casters.clear();
        return;
    }
    auto* pOut = static_cast<float*>(mapped.pData);
    for (const Caster& caster : casters)
    {
        const float toObject[3] = {caster.Translate[0] - eye[0], caster.Translate[1] - eye[1], caster.Translate[2] - eye[2]};
        const auto writeRow = [&](const float* apAxis, float aScale, float aMove)
        {
            // (a rotation's rows are where the object's axes point in the world)
            for (int column = 0; column < 3; ++column)
                *pOut++ = aScale * caster.Scale * Dot3(apAxis, caster.Rows[column]);
            *pOut++ = aScale * Dot3(apAxis, toObject) + aMove;
        };
        writeRow(right, scaleX, moveX);
        writeRow(up, scaleY, moveY);
        writeRow(forward, scaleZ, moveZ);
    }
    pContext->Unmap(s_instances.Get(), 0);

    // The depth state the game's shadow technique sets. Its accumulator has finished by here, and put those back.
    uint8_t* pState = ShadowFeatures::GameRenderState();
    const uint32_t oldCull = Field<uint32_t>(pState, kStateCull);
    const uint32_t oldBias = Field<uint32_t>(pState, kStateDepthBias);
    const uint32_t oldBlendFirst = Field<uint32_t>(pState, kStateBlendFirst);
    const uint32_t oldBlendAlpha = Field<uint32_t>(pState, kStateBlendAlpha);
    SetState(pState, kStateCull, kCullBack, kDirtyRaster);
    SetState(pState, kStateDepthBias, kShadowDepthBias, kDirtyRaster);
    SetState(pState, kStateBlendFirst, 0, kDirtyBlend);
    SetState(pState, kStateBlendAlpha, 0, kDirtyBlend);
    ShadowFeatures::FlushRenderState();

    // What's bound goes back as it was: the game keeps track of what it last set, and doesn't set it again
    ComPtr<ID3D11VertexShader> oldVertexShader;
    ComPtr<ID3D11PixelShader> oldPixelShader;
    ComPtr<ID3D11InputLayout> oldLayout;
    ComPtr<ID3D11Buffer> oldIndices;
    DXGI_FORMAT oldIndexFormat = DXGI_FORMAT_UNKNOWN;
    UINT oldIndexOffset = 0;
    ID3D11Buffer* pOldStreams[2]{};
    UINT oldStrides[2]{}, oldOffsets[2]{};
    D3D11_PRIMITIVE_TOPOLOGY oldTopology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    pContext->VSGetShader(&oldVertexShader, nullptr, nullptr);
    pContext->PSGetShader(&oldPixelShader, nullptr, nullptr);
    pContext->IAGetInputLayout(&oldLayout);
    pContext->IAGetIndexBuffer(&oldIndices, &oldIndexFormat, &oldIndexOffset);
    pContext->IAGetVertexBuffers(0, 2, pOldStreams, oldStrides, oldOffsets);
    pContext->IAGetPrimitiveTopology(&oldTopology);

    pContext->VSSetShader(s_shader.Get(), nullptr, 0);
    pContext->PSSetShader(nullptr, nullptr, 0);
    pContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* pInstances = s_instances.Get();
    const UINT instanceStride = kInstanceStride, instanceOffset = 0;
    pContext->IASetVertexBuffers(1, 1, &pInstances, &instanceStride, &instanceOffset);

    using TPrepare = void(const void* apBuffer, ID3D11DeviceContext* apContext);
    const VersionDbPtr<uint8_t> prepareFunction(kPrepareBuffer);
    auto* prepare = reinterpret_cast<TPrepare*>(prepareFunction.Get());

    ID3D11InputLayout* pBoundLayout = nullptr;
    size_t first = 0;
    uint64_t draws = 0;
    while (first < casters.size())
    {
        const Caster& caster = casters[first];
        size_t last = first + 1;
        while (last < casters.size() && casters[last].pData == caster.pData && casters[last].Triangles == caster.Triangles)
            ++last;

        const uint64_t format = Read<uint64_t>(caster.pData, 0);
        const void* pVertices = Read<const void*>(caster.pData, kDataVertices);
        const void* pIndices = Read<const void*>(caster.pData, kDataIndices);
        prepare(pIndices, pContext);
        prepare(pVertices, pContext);

        auto* pLayout = (format & kVertexFullPrecision) ? s_layoutFull.Get() : s_layoutHalf.Get();
        if (pLayout != pBoundLayout)
        {
            pContext->IASetInputLayout(pLayout);
            pBoundLayout = pLayout;
        }
        auto* pVertexBuffer = Read<ID3D11Buffer*>(pVertices, 0);
        const UINT stride = static_cast<UINT>((format << 2) & 0x3C);
        const UINT vertexOffset = Read<uint32_t>(pVertices, kBufferOffset);
        pContext->IASetVertexBuffers(0, 1, &pVertexBuffer, &stride, &vertexOffset);
        pContext->IASetIndexBuffer(Read<ID3D11Buffer*>(pIndices, 0), DXGI_FORMAT_R16_UINT, Read<uint32_t>(pIndices, kBufferOffset));
        pContext->DrawIndexedInstanced(caster.Triangles * 3, static_cast<UINT>(last - first), 0, 0, static_cast<UINT>(first));
        ++draws;
        first = last;
    }
    s_draws.fetch_add(draws, std::memory_order_relaxed);

    pContext->VSSetShader(oldVertexShader.Get(), nullptr, 0);
    pContext->PSSetShader(oldPixelShader.Get(), nullptr, 0);
    pContext->IASetInputLayout(oldLayout.Get());
    pContext->IASetIndexBuffer(oldIndices.Get(), oldIndexFormat, oldIndexOffset);
    pContext->IASetVertexBuffers(0, 2, pOldStreams, oldStrides, oldOffsets);
    pContext->IASetPrimitiveTopology(oldTopology);
    for (auto*& pStream : pOldStreams)
    {
        if (pStream)
            pStream->Release();
        pStream = nullptr;
    }

    SetState(pState, kStateCull, oldCull, kDirtyRaster);
    SetState(pState, kStateDepthBias, oldBias, kDirtyRaster);
    SetState(pState, kStateBlendFirst, oldBlendFirst, kDirtyBlend);
    SetState(pState, kStateBlendAlpha, oldBlendAlpha, kDirtyBlend);
    casters.clear();
}

void GpuShadows::BeginFrame() noexcept
{
    // Gathered and not drawn (a frame cut short): the meshes those point at may not last to the next
    for (auto& list : s_lists)
    {
        AcquireSRWLockExclusive(&list.Lock);
        list.Casters.clear();
        ReleaseSRWLockExclusive(&list.Lock);
    }
}

void GpuShadows::Forget() noexcept
{
    BeginFrame();
}

void GpuShadows::Report(double aSeconds) noexcept
{
    const uint64_t taken = s_taken.exchange(0, std::memory_order_relaxed);
    const uint64_t draws = s_draws.exchange(0, std::memory_order_relaxed);
    if (!s_wanted || s_broken)
        return;
    const double perSecond = aSeconds > 0.0 ? 1.0 / aSeconds : 0.0;
    spdlog::info("GPU-driven shadows: {:.0f} casters a second went through {:.0f} draws a second ({:.1f} to a draw)", static_cast<double>(taken) * perSecond, static_cast<double>(draws) * perSecond, draws ? static_cast<double>(taken) / static_cast<double>(draws) : 0.0);
}
