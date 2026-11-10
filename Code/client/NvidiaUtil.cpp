#include "NvidiaUtil.h"
#include <d3d11.h>
#include <string>

bool IsNvidiaOverlayLoaded()
{
    return GetModuleHandleW(L"nvspcap64.dll");
}

// This makes the Nvidia overlay happy.
// The call to D3D11CreateDevice probably causes some of their
// internal hooks to be called and do the required init work before the game window opens.
// Windows' own d3d11.dll, by its full path: the game may run a graphics mod's copy from its folder (ENB), which
// shouldn't see this device. Nothing of ours imports d3d11.dll, so the game finds that copy first, as it does alone.
HRESULT CreateEarlyDxDevice(ID3D11Device* apOutDevice, D3D_FEATURE_LEVEL* apOutFeatureLevel)
{
    wchar_t systemPath[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(systemPath, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return E_FAIL;

    const HMODULE hD3D11 = LoadLibraryW((std::wstring(systemPath) + L"\\d3d11.dll").c_str());
    const auto createDevice = hD3D11 ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(GetProcAddress(hD3D11, "D3D11CreateDevice")) : nullptr;
    if (!createDevice)
        return E_FAIL;

    return createDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &apOutDevice, apOutFeatureLevel, nullptr);
}
