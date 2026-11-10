#pragma once

#include <wrl.h>
#include <mutex>
#include <vector>

#include <OverlayRenderHandler.hpp>
#include <TiltedCore/Signal.hpp>

namespace DirectX
{
    class SpriteBatch;
    class CommonStates;
}

struct IDXGISwapChain;
struct ID3D11Texture2D;
struct ID3D11ShaderResourceView;
struct ID3D11DeviceContext;
struct ID3D11Device;

namespace TiltedPhoques
{
    struct OverlayRenderHandlerD3D11 : OverlayRenderHandler
    {
        struct Renderer
        {
            Renderer() = default;
            virtual ~Renderer() = default;
            [[nodiscard]] virtual IDXGISwapChain* GetSwapChain() const noexcept = 0;

            TP_NOCOPYMOVE(Renderer);
        };

        explicit OverlayRenderHandlerD3D11(Renderer* apRenderer) noexcept;
        virtual ~OverlayRenderHandlerD3D11();

        TP_NOCOPYMOVE(OverlayRenderHandlerD3D11);

        void Create() override;
        void Render() override;
        void Reset() override;
        void ClearFrame() override;

        void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
        void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type, const RectList& dirtyRects, const void* buffer, int width, int height) override;

        IMPLEMENT_REFCOUNTING(OverlayRenderHandlerD3D11);

    protected:

        void GetRenderTargetSize();
        void CreateRenderTexture();
        // Into the texture: what CEF painted since the game's last frame (m_textureLock held)
        void UploadFrame();
        void MarkDirty(uint32_t aLeft, uint32_t aTop, uint32_t aRight, uint32_t aBottom);

    private:
        uint32_t m_width{ 0 };
        uint32_t m_height{ 0 };

        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_pTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pCursorTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_pTextureView;
        std::mutex m_textureLock;
        std::mutex m_createLock;
        bool m_isCreateLockLocked = false;
        Renderer* m_pRenderer;

        Microsoft::WRL::ComPtr<ID3D11Device> m_pDevice;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_pImmediateContext;

        // The page as CEF last painted it, and the part of it the texture doesn't have yet
        std::vector<uint8_t> m_frame;
        uint32_t m_frameWidth{ 0 };
        uint32_t m_frameHeight{ 0 };
        bool m_frameDirty{ false };
        uint32_t m_dirtyLeft{ 0 };
        uint32_t m_dirtyTop{ 0 };
        uint32_t m_dirtyRight{ 0 };
        uint32_t m_dirtyBottom{ 0 };

        std::unique_ptr<DirectX::SpriteBatch> m_pSpriteBatch;
        std::unique_ptr<DirectX::CommonStates> m_pStates;
    };
}
