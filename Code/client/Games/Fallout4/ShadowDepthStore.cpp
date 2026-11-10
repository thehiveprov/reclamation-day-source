#include <ShadowFeatures.h>

#include <d3d11.h>
#include <wrl/client.h>

// The depth of shadow maps kept in our own texture array. D3D11 copies a depth texture only as a whole slice, so each slot is
// as big as the game's shadow map, and is only made once a feature that needs it is on.

using Microsoft::WRL::ComPtr;

namespace
{
ComPtr<ID3D11Texture2D> s_store;
D3D11_TEXTURE2D_DESC s_storeDesc{};
bool s_failed = false;

// The depth slice bound now, as D3D11 sees it
bool Bound(ComPtr<ID3D11Texture2D>& arTexture, D3D11_TEXTURE2D_DESC& arDesc, UINT& arSubresource) noexcept
{
    auto* pContext = ShadowFeatures::Context();
    if (!pContext)
        return false;
    ShadowFeatures::FlushRenderState();

    ComPtr<ID3D11DepthStencilView> view;
    pContext->OMGetRenderTargets(0, nullptr, &view);
    if (!view)
        return false;
    D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc{};
    view->GetDesc(&viewDesc);
    ComPtr<ID3D11Resource> resource;
    view->GetResource(&resource);
    if (!resource || FAILED(resource.As(&arTexture)))
        return false;
    arTexture->GetDesc(&arDesc);
    if (arDesc.SampleDesc.Count != 1)
        return false;

    switch (viewDesc.ViewDimension)
    {
    case D3D11_DSV_DIMENSION_TEXTURE2D: arSubresource = D3D11CalcSubresource(viewDesc.Texture2D.MipSlice, 0, arDesc.MipLevels); return true;
    case D3D11_DSV_DIMENSION_TEXTURE2DARRAY:
        arSubresource = D3D11CalcSubresource(viewDesc.Texture2DArray.MipSlice, viewDesc.Texture2DArray.FirstArraySlice, arDesc.MipLevels);
        return true;
    default: return false;
    }
}

bool Ensure(const D3D11_TEXTURE2D_DESC& acBound) noexcept
{
    if (s_failed)
        return false;
    if (s_store && s_storeDesc.Width == acBound.Width && s_storeDesc.Height == acBound.Height && s_storeDesc.Format == acBound.Format)
        return true;
    s_store.Reset();

    auto* pDevice = ShadowFeatures::Device();
    if (!pDevice)
        return false;
    D3D11_TEXTURE2D_DESC desc = acBound;
    desc.MipLevels = 1;
    desc.ArraySize = ShadowDepthStore::kMaxSlots;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = 0;
    if (FAILED(pDevice->CreateTexture2D(&desc, nullptr, &s_store)))
    {
        s_failed = true;
        spdlog::warn("Shadow depth store: {} slices of {}x{} couldn't be made, the features that keep shadow maps are off", desc.ArraySize, desc.Width, desc.Height);
        return false;
    }
    s_storeDesc = desc;
    spdlog::info("Shadow depth store: {} slices of {}x{} (format {})", desc.ArraySize, desc.Width, desc.Height, static_cast<int>(desc.Format));
    return true;
}
} // namespace

bool ShadowDepthStore::Save(uint32_t aSlot) noexcept
{
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    UINT subresource = 0;
    if (aSlot >= kMaxSlots || !Bound(texture, desc, subresource) || !Ensure(desc))
        return false;
    ShadowFeatures::Context()->CopySubresourceRegion(s_store.Get(), D3D11CalcSubresource(0, aSlot, 1), 0, 0, 0, texture.Get(), subresource, nullptr);
    return true;
}

bool ShadowDepthStore::Restore(uint32_t aSlot) noexcept
{
    ComPtr<ID3D11Texture2D> texture;
    D3D11_TEXTURE2D_DESC desc{};
    UINT subresource = 0;
    if (aSlot >= kMaxSlots || !s_store || !Bound(texture, desc, subresource) || !Ensure(desc))
        return false;
    ShadowFeatures::Context()->CopySubresourceRegion(texture.Get(), subresource, 0, 0, 0, s_store.Get(), D3D11CalcSubresource(0, aSlot, 1), nullptr);
    return true;
}

void ShadowDepthStore::Reset() noexcept
{
    s_store.Reset();
    s_failed = false;
}
