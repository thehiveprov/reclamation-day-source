
#include <SpriteBatch.h>
#include <DirectXColors.h>
#include <SimpleMath.h>
#include <CommonStates.h>

#include <WICTextureLoader.h>
#include <DDSTextureLoader.h>

#include <OverlayClient.hpp>
#include <OverlayRenderHandlerD3D11.hpp>

#include <algorithm>

namespace TiltedPhoques
{
    OverlayRenderHandlerD3D11::OverlayRenderHandlerD3D11(Renderer* apRenderer) noexcept
        : m_pRenderer(apRenderer)
    {
        // So we need to lock this until we have the window dimension as a background CEF thread will attempt to get it before we have it
        m_createLock.lock();
        m_isCreateLockLocked = true;
    }

    OverlayRenderHandlerD3D11::~OverlayRenderHandlerD3D11() = default;

    void OverlayRenderHandlerD3D11::Render()
    {
        // We need a context first
        if (!m_pImmediateContext)
        {
            Create();

            if (!m_pImmediateContext)
                return;
        }

        GetRenderTargetSize();

        // First of all what CEF painted since the last frame, once however many times it painted
        {
            std::unique_lock _(m_textureLock);

            UploadFrame();
        }

        if (IsVisible())
        {
            m_pSpriteBatch->Begin(DirectX::SpriteSortMode_Deferred, m_pStates->NonPremultiplied());

            {
                std::unique_lock<std::mutex> _(m_textureLock);

                if (m_pTextureView)
                    m_pSpriteBatch->Draw(m_pTextureView.Get(), DirectX::SimpleMath::Vector2(0.f, 0.f), nullptr, DirectX::Colors::White, 0.f);
            }

            if (IsCursorVisible() && m_pCursorTexture)
            {
                m_pSpriteBatch->Draw(m_pCursorTexture.Get(), DirectX::SimpleMath::Vector2(m_cursorX, m_cursorY), nullptr, DirectX::Colors::White, 0.f, DirectX::SimpleMath::Vector2(0, 0), m_width / 1920.f);
            }

            m_pSpriteBatch->End();
        }
    }

    void OverlayRenderHandlerD3D11::Reset()
    {
        m_pTexture.Reset();
        m_pCursorTexture.Reset();
        m_pTextureView.Reset();
        m_pDevice.Reset();
        m_pImmediateContext.Reset();

        m_pSpriteBatch = nullptr;
        m_pStates = nullptr;

        Create();
    }

    void OverlayRenderHandlerD3D11::ClearFrame()
    {
        std::unique_lock _(m_textureLock);

        if (m_frame.empty())
            return;

        std::fill(m_frame.begin(), m_frame.end(), uint8_t{0});
        MarkDirty(0, 0, m_frameWidth, m_frameHeight);
    }

    void OverlayRenderHandlerD3D11::Create()
    {
        const auto hr = m_pRenderer->GetSwapChain()->GetDevice(IID_ID3D11Device, reinterpret_cast<void**>(m_pDevice.ReleaseAndGetAddressOf()));

        if (FAILED(hr))
            return;

        m_pDevice->GetImmediateContext(m_pImmediateContext.ReleaseAndGetAddressOf());

        if (!m_pImmediateContext)
            return;

        GetRenderTargetSize();

        m_pSpriteBatch = std::make_unique<DirectX::SpriteBatch>(m_pImmediateContext.Get());
        m_pStates = std::make_unique<DirectX::CommonStates>(m_pDevice.Get());

        if (FAILED(DirectX::CreateWICTextureFromFile(m_pDevice.Get(), m_pParent->GetCursorPathPNG().c_str(), nullptr, m_pCursorTexture.ReleaseAndGetAddressOf())))
        {
            DirectX::CreateDDSTextureFromFile(m_pDevice.Get(), m_pParent->GetCursorPathDDS().c_str(), nullptr, m_pCursorTexture.ReleaseAndGetAddressOf());
        }

        std::unique_lock _(m_textureLock);

        if (!m_pTexture)
            CreateRenderTexture();
    }

    void OverlayRenderHandlerD3D11::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect)
    {
        std::scoped_lock _(m_createLock);

        rect = CefRect(0, 0, m_width, m_height);
    }

    void OverlayRenderHandlerD3D11::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
        const RectList& dirtyRects, const void* buffer, int width, int height)
    {
        if (type != PET_VIEW || width <= 0 || height <= 0 || m_width != static_cast<uint32_t>(width) || m_height != static_cast<uint32_t>(height))
            return;

        // Only what changed, into our copy of the page; the game's thread uploads it at its next frame. All of the page
        // was copied into a dynamic texture through a deferred context instead: 20 MB at 3440 x 1440, ~20 ms a paint on
        // CEF's thread, which then painted the overlay far less often (5 times a second with a heavy ENB preset).
        std::unique_lock _(m_textureLock);

        const size_t pitch = static_cast<size_t>(width) * 4;
        const bool newSize = m_frameWidth != static_cast<uint32_t>(width) || m_frameHeight != static_cast<uint32_t>(height);
        if (newSize)
        {
            m_frame.assign(pitch * height, 0);
            m_frameWidth = width;
            m_frameHeight = height;
        }

        const auto* pSource = static_cast<const uint8_t*>(buffer);
        const auto copy = [&](int aX, int aY, int aWidth, int aHeight)
        {
            const int left = std::clamp(aX, 0, width);
            const int top = std::clamp(aY, 0, height);
            const int right = std::clamp(aX + aWidth, left, width);
            const int bottom = std::clamp(aY + aHeight, top, height);
            if (right <= left || bottom <= top)
                return;

            const size_t rowBytes = static_cast<size_t>(right - left) * 4;
            for (int y = top; y < bottom; ++y)
            {
                const size_t offset = static_cast<size_t>(y) * pitch + static_cast<size_t>(left) * 4;
                std::memcpy(m_frame.data() + offset, pSource + offset, rowBytes);
            }
            MarkDirty(left, top, right, bottom);
        };

        if (newSize || dirtyRects.empty())
            copy(0, 0, width, height);
        else
        {
            for (const CefRect& rect : dirtyRects)
                copy(rect.x, rect.y, rect.width, rect.height);
        }
    }

    void OverlayRenderHandlerD3D11::MarkDirty(uint32_t aLeft, uint32_t aTop, uint32_t aRight, uint32_t aBottom)
    {
        if (!m_frameDirty)
        {
            m_dirtyLeft = aLeft;
            m_dirtyTop = aTop;
            m_dirtyRight = aRight;
            m_dirtyBottom = aBottom;
            m_frameDirty = true;
            return;
        }

        m_dirtyLeft = std::min(m_dirtyLeft, aLeft);
        m_dirtyTop = std::min(m_dirtyTop, aTop);
        m_dirtyRight = std::max(m_dirtyRight, aRight);
        m_dirtyBottom = std::max(m_dirtyBottom, aBottom);
    }

    void OverlayRenderHandlerD3D11::UploadFrame()
    {
        if (m_frame.empty() || m_frameWidth != m_width || m_frameHeight != m_height)
            return;

        if (!m_pTexture)
        {
            // A new texture: all of the page goes in (CreateRenderTexture marks it)
            CreateRenderTexture();
            if (!m_pTexture)
                return;
        }

        if (!m_frameDirty)
            return;

        const UINT pitch = m_frameWidth * 4;
        const D3D11_BOX box{m_dirtyLeft, m_dirtyTop, 0, m_dirtyRight, m_dirtyBottom, 1};
        m_pImmediateContext->UpdateSubresource(m_pTexture.Get(), 0, &box, m_frame.data() + static_cast<size_t>(m_dirtyTop) * pitch + static_cast<size_t>(m_dirtyLeft) * 4, pitch, 0);
        m_frameDirty = false;
    }

    void OverlayRenderHandlerD3D11::GetRenderTargetSize()
    {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> pRenderTargetView;

        m_pImmediateContext->OMGetRenderTargets(1, pRenderTargetView.ReleaseAndGetAddressOf(), nullptr);
        if (pRenderTargetView)
        {
            Microsoft::WRL::ComPtr<ID3D11Resource> pSrcResource;
            pRenderTargetView->GetResource(pSrcResource.ReleaseAndGetAddressOf());

            if (pSrcResource)
            {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> pSrcBuffer;
                pSrcResource.As(&pSrcBuffer);

                D3D11_TEXTURE2D_DESC desc;
                pSrcBuffer->GetDesc(&desc);

                if ((m_width != desc.Width || m_height != desc.Height) && m_pParent)
                {
                    m_width = desc.Width;
                    m_height = desc.Height;

                    // We now know the size of the viewport, we can let CEF get it
                    if (m_isCreateLockLocked)
                    {
                        m_createLock.unlock();
                        m_isCreateLockLocked = false;
                    }

                    {
                        std::unique_lock<std::mutex> _(m_textureLock);

                        m_pTexture.Reset();
                        m_pTextureView.Reset();
                    }

                    if (m_pParent->GetBrowser())
                        m_pParent->GetBrowser()->GetHost()->WasResized();
                }
            }
        }
    }

    void OverlayRenderHandlerD3D11::CreateRenderTexture()
    {
        D3D11_TEXTURE2D_DESC textDesc;
        textDesc.Width = m_width;
        textDesc.Height = m_height;
        textDesc.MipLevels = textDesc.ArraySize = 1;
        textDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        textDesc.SampleDesc.Count = 1;
        textDesc.SampleDesc.Quality = 0;
        // Filled by UpdateSubresource on the game's thread (UploadFrame)
        textDesc.Usage = D3D11_USAGE_DEFAULT;
        textDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        textDesc.CPUAccessFlags = 0;
        textDesc.MiscFlags = 0;

        if (FAILED(m_pDevice->CreateTexture2D(&textDesc, nullptr, m_pTexture.ReleaseAndGetAddressOf())))
            return;

        D3D11_SHADER_RESOURCE_VIEW_DESC sharedResourceViewDesc = {};
        sharedResourceViewDesc.Format = textDesc.Format;
        sharedResourceViewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sharedResourceViewDesc.Texture2D.MipLevels = 1;

        if (FAILED(m_pDevice->CreateShaderResourceView(m_pTexture.Get(), &sharedResourceViewDesc, m_pTextureView.ReleaseAndGetAddressOf())))
            return;

        // It has none of the page yet
        if (!m_frame.empty() && m_frameWidth == m_width && m_frameHeight == m_height)
            MarkDirty(0, 0, m_frameWidth, m_frameHeight);
    }
}
