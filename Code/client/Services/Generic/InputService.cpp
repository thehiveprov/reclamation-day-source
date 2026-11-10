#include <HivemindPCH.h>

#include <Services/InputService.h>
#include <Services/OverlayService.h>

#include <OverlayApp.hpp>

#include <DInputHook.hpp>
#include <WindowsHook.hpp>

#include <include/internal/cef_types.h>
#include <Services/ImguiService.h>
#include <Services/DiscordService.h>
#include <World.h>

#include <bitset>

#include "Games/Skyrim/Interface/MenuControls.h"
#if TP_FALLOUT4
#include <Interface/StartMenuBridge.h>
#endif

static OverlayService* s_pOverlay = nullptr;
static UINT s_currentACP = CP_ACP;

#if TP_FALLOUT4
// user32's ClipCursor. The game (its cursor update, our ClipCursor_H) and High FPS Physics Fix (LockCursor, on the
// window's activation, move and size messages) keep the cursor inside the game's window. While the overlay is up the
// cursor is a desktop cursor: no clip, so it can leave the window (another monitor, another program).
using TClipCursor = BOOL WINAPI(const RECT*);
static TClipCursor* RealClipCursor = nullptr;

static BOOL WINAPI HookClipCursor(const RECT* apRect)
{
    if (apRect && s_pOverlay && s_pOverlay->GetActive())
        return RealClipCursor(nullptr);
    return RealClipCursor(apRect);
}

static TiltedPhoques::Initializer s_clipCursorHook(
    []()
    {
        RealClipCursor = reinterpret_cast<TClipCursor*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "ClipCursor"));
        if (RealClipCursor)
            TP_HOOK(&RealClipCursor, HookClipCursor);
    });

// What the overlay takes from the game while it's up: keys, the mouse in the window, raw input, and WM_SETCURSOR (the
// arrow is set in WndProc). The window's own messages (activation, focus, moving, sizing, the system's) still go to
// the game and on to Windows: answered 0 here, WM_NCACTIVATE kept the window active and nothing could be switched to.
static bool IsOverlayInput(UINT aMessage) noexcept
{
    return aMessage == WM_INPUT || aMessage == WM_SETCURSOR || (aMessage >= WM_KEYFIRST && aMessage <= WM_KEYLAST) ||
           (aMessage >= WM_MOUSEFIRST && aMessage <= WM_MOUSELAST);
}
#endif

void ForceKillAllInput()
{
#if TP_SKYRIM
    MenuControls::GetInstance()->SetToggle(false);
#else
    // TODO! Crash the project so we notice
    int* t = nullptr;
    *t = 42;
#endif
}

uint32_t GetCefModifiers(uint16_t aVirtualKey)
{
    uint32_t modifiers = EVENTFLAG_NONE;

    if (GetKeyState(VK_MENU) & 0x8000)
    {
        modifiers |= EVENTFLAG_ALT_DOWN;
    }

    if (GetKeyState(VK_CONTROL) & 0x8000)
    {
        modifiers |= EVENTFLAG_CONTROL_DOWN;
    }

    if (GetKeyState(VK_SHIFT) & 0x8000)
    {
        modifiers |= EVENTFLAG_SHIFT_DOWN;
    }

    if (GetKeyState(VK_LBUTTON) & 0x8000)
    {
        modifiers |= EVENTFLAG_LEFT_MOUSE_BUTTON;
    }

    if (GetKeyState(VK_RBUTTON) & 0x8000)
    {
        modifiers |= EVENTFLAG_RIGHT_MOUSE_BUTTON;
    }

    if (GetKeyState(VK_MBUTTON) & 0x8000)
    {
        modifiers |= EVENTFLAG_MIDDLE_MOUSE_BUTTON;
    }

    if (GetKeyState(VK_CAPITAL) & 1)
    {
        modifiers |= EVENTFLAG_CAPS_LOCK_ON;
    }

    if (GetKeyState(VK_NUMLOCK) & 1)
    {
        modifiers |= EVENTFLAG_NUM_LOCK_ON;
    }

    if (aVirtualKey)
    {
        if (aVirtualKey == VK_RCONTROL || aVirtualKey == VK_RMENU || aVirtualKey == VK_RSHIFT)
        {
            modifiers |= EVENTFLAG_IS_RIGHT;
        }
        else if (aVirtualKey == VK_LCONTROL || aVirtualKey == VK_LMENU || aVirtualKey == VK_LSHIFT)
        {
            modifiers |= EVENTFLAG_IS_LEFT;
        }
        else if (aVirtualKey >= VK_NUMPAD0 && aVirtualKey <= VK_DIVIDE)
        {
            modifiers |= EVENTFLAG_IS_KEY_PAD;
        }
    }

    return modifiers;
}

// remember to update this when updating toggle keys
bool IsToggleKey(int aKey) noexcept
{
    return aKey == VK_RCONTROL || aKey == VK_F2;
}

bool IsDisableKey(int aKey) noexcept
{
    return aKey == VK_ESCAPE;
}

#if TP_SKYRIM64
void SetUIActive(OverlayService& aOverlay, auto apRenderer, bool aActive)
{
#if defined(TP_SKYRIM)
    TiltedPhoques::DInputHook::Get().SetEnabled(aActive);
    aOverlay.SetActive(aActive);
#else
    pRenderer->SetVisible(aActive);
#endif

    // Ensures the game is actually loaded, in case the initial event was sent too early
    aOverlay.SetVersion(BUILD_COMMIT);
    aOverlay.GetOverlayApp()->ExecuteAsync("enterGame");

    apRenderer->SetCursorVisible(aActive);

    // This is to disable the Windows cursor
    while (ShowCursor(FALSE) >= 0)
        ;
}
#endif

void ProcessKeyboard(uint16_t aKey, uint16_t aScanCode, cef_key_event_type_t aType, bool aE0, bool aE1)
{
    if (aType != KEYEVENT_CHAR)
    {
        if (!aKey || aKey == 255)
        {
            return;
        }

        if (aKey == VK_SHIFT)
        {
            aKey = static_cast<uint16_t>(MapVirtualKey(aScanCode, MAPVK_VSC_TO_VK_EX));
        }
        else if (aKey == VK_NUMLOCK)
        {
            aScanCode = static_cast<uint16_t>(MapVirtualKey(aKey, MAPVK_VK_TO_VSC) | 0x100);
        }

        if (aE1)
        {
            if (aKey == VK_PAUSE)
            {
                aScanCode = 0x45;
            }
            else
            {
                aScanCode = static_cast<uint16_t>(MapVirtualKey(aKey, MAPVK_VK_TO_VSC));
            }
        }

        if (aE0)
        {
            switch (aKey)
            {
            case VK_CONTROL: aKey = VK_RCONTROL; break;
            case VK_MENU: aKey = VK_RMENU; break;
            case VK_RETURN: aKey = VK_SEPARATOR; break;
            }
        }
        else
        {
            switch (aKey)
            {
            case VK_CONTROL: aKey = VK_LCONTROL; break;
            case VK_MENU: aKey = VK_LMENU; break;
            case VK_INSERT: aKey = VK_NUMPAD0; break;
            case VK_DELETE: aKey = VK_DECIMAL; break;
            case VK_HOME: aKey = VK_NUMPAD7; break;
            case VK_END: aKey = VK_NUMPAD1; break;
            case VK_PRIOR: aKey = VK_NUMPAD9; break;
            case VK_NEXT: aKey = VK_NUMPAD3; break;
            case VK_LEFT: aKey = VK_NUMPAD4; break;
            case VK_RIGHT: aKey = VK_NUMPAD6; break;
            case VK_UP: aKey = VK_NUMPAD8; break;
            case VK_DOWN: aKey = VK_NUMPAD2; break;
            case VK_CLEAR: aKey = VK_NUMPAD5; break;
            }
        }
    }

    auto& overlay = *s_pOverlay;

    const auto pApp = overlay.GetOverlayApp();
    if (!pApp)
        return;

    const auto pClient = pApp->GetClient();
    if (!pClient)
        return;

    const auto pRenderer = pClient->GetOverlayRenderHandler();
    if (!pRenderer)
        return;

#if TP_SKYRIM64
    const auto active = overlay.GetActive();

    spdlog::debug("ProcessKey, type: {}, key: {}, active: {}", aType, aKey, active);

    if (aType != KEYEVENT_CHAR && (IsToggleKey(aKey) || (IsDisableKey(aKey) && active)))
    {
        if (!overlay.GetInGame())
        {
            TiltedPhoques::DInputHook::Get().SetEnabled(false);
        }
        else if (aType == KEYEVENT_KEYUP)
        {
            SetUIActive(overlay, pRenderer, !active);
        }
    }
    else if (active)
    {
        pApp->InjectKey(aType, GetCefModifiers(aKey), aKey, aScanCode);
    }

#else
    // Shift+F2: the page's process ended and the page brought back, whatever the overlay is showing. One press however
    // long it's held (the raw input repeats), and not F2's toggle
    static bool s_restartKeyHeld = false;
    if (aKey == VK_F2 && aType == KEYEVENT_KEYUP && s_restartKeyHeld)
    {
        s_restartKeyHeld = false;
        return;
    }
    if (aKey == VK_F2 && aType == KEYEVENT_KEYDOWN && (s_restartKeyHeld || (GetAsyncKeyState(VK_SHIFT) & 0x8000)))
    {
        if (!s_restartKeyHeld)
            overlay.RestartPage();
        s_restartKeyHeld = true;
        return;
    }

    // The overlay is always drawn, OverlayService's active flag is the "UI open" state (it drives input blocking and
    // the viewport lock in WndProc). The UI page itself must also be told to activate, otherwise it renders nothing.
    const auto active = overlay.GetActive();

    // While the overlay draws the game's pause menu, Escape and F2 are the page's (Escape is "back" there)
    if (aType == KEYEVENT_KEYDOWN && overlay.GetInGame() && !StartMenuBridge::IsOpen() && (IsToggleKey(aKey) || (IsDisableKey(aKey) && active)))
    {
        if (!active)
        {
            // Ensures the game is actually loaded, in case the initial event was sent too early
            overlay.SetVersion(BUILD_COMMIT);
            pApp->ExecuteAsync("enterGame");
        }
        overlay.SetActive(!active);

        if (active)
            while (ShowCursor(FALSE) >= 0)
                ;
        else
            while (ShowCursor(TRUE) <= 0)
                ;
    }
    else if (active)
    {
        pApp->InjectKey(aType, GetCefModifiers(aKey), aKey, aScanCode);
    }
#endif
}

void ProcessMouseMove(uint16_t aX, uint16_t aY)
{
    auto& overlay = *s_pOverlay;

    const auto pApp = overlay.GetOverlayApp();
    if (!pApp)
        return;

    const auto pClient = pApp->GetClient();
    if (!pClient)
        return;

    const auto pRenderer = pClient->GetOverlayRenderHandler();
    if (!pRenderer)
        return;

    const auto active = overlay.GetActive();

    if (active)
    {
        pApp->InjectMouseMove(aX, aY, GetCefModifiers(0));
    }
}

void ProcessMouseButton(uint16_t aX, uint16_t aY, cef_mouse_button_type_t aButton, bool aDown)
{
    auto& overlay = *s_pOverlay;

    const auto pApp = overlay.GetOverlayApp();
    if (!pApp)
        return;

    const auto pClient = pApp->GetClient();
    if (!pClient)
        return;

    const auto pRenderer = pClient->GetOverlayRenderHandler();
    if (!pRenderer)
        return;

    const auto active = overlay.GetActive();

    if (active)
    {
        pApp->InjectMouseButton(aX, aY, aButton, !aDown, GetCefModifiers(0));
    }
}

void ProcessMouseWheel(uint16_t aX, uint16_t aY, int16_t aZ)
{
    auto& overlay = *s_pOverlay;

    const auto pApp = overlay.GetOverlayApp();
    if (!pApp)
        return;

    const auto pClient = pApp->GetClient();
    if (!pClient)
        return;

    const auto pRenderer = pClient->GetOverlayRenderHandler();
    if (!pRenderer)
        return;

    const auto active = overlay.GetActive();

    if (active)
    {
        pApp->InjectMouseWheel(aX, aY, aZ, GetCefModifiers(0));
    }
}

UINT GetRealACP()
{
    // Get the keyboard layout for the current thread.
    HKL keybdLayout = GetKeyboardLayout(0);

    // Extract the language ID from it, contained in its low-order word.
    int langID = LOWORD(keybdLayout);

    // Call the GetLocaleInfo function to retrieve the default ANSI code page
    // associated with that language ID.
    UINT acp = CP_ACP;
    GetLocaleInfo(MAKELCID(langID, SORT_DEFAULT),
        LOCALE_IDEFAULTANSICODEPAGE | LOCALE_RETURN_NUMBER,
        (LPTSTR) &acp,
        sizeof(acp) / sizeof(TCHAR));
    return acp;
}

LRESULT CALLBACK InputService::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    const auto pApp = s_pOverlay->GetOverlayApp();
    if (!pApp)
        return 0;

    const auto pClient = pApp->GetClient();
    if (!pClient)
        return 0;

    const auto pRenderer = pClient->GetOverlayRenderHandler();
    if (!pRenderer)
        return 0;

    auto& discord = World::Get().ctx().at<DiscordService>();
    discord.WndProcHandler(hwnd, uMsg, wParam, lParam);

    const bool active = s_pOverlay->GetActive();
    if (active)
    {
        auto& imgui = World::Get().ctx().at<ImguiService>();
        imgui.WndProcHandler(hwnd, uMsg, wParam, lParam);
    }

#if TP_FALLOUT4
    POINTER_FALLOUT4(uint8_t, s_viewportLock, 1549778);
    *s_viewportLock = active ? 1 : 0;

    // The game's window class has no cursor (hCursor 0) and its WM_SETCURSOR only ever applies that, so over the game
    // the cursor has no shape. Whatever opened the overlay (F2 here in ProcessKeyboard, the client's own windows from
    // their services), while it is up the cursor is shown here, on the window's own thread, with the arrow; given back
    // once it's down. The first few messages after it opens log what Windows has for the cursor, the first few left
    // clicks where they go
    static bool s_arrowShown = false;
    static int s_cursorChecks = 0;
    static int s_clickChecks = 0;
    if (active)
    {
        if (!s_arrowShown)
        {
            while (ShowCursor(TRUE) < 0)
                ;
            s_arrowShown = true;
            s_cursorChecks = 3;
            s_clickChecks = 5;
        }
        if (uMsg == WM_SETCURSOR || !GetCursor())
            SetCursor(LoadCursor(nullptr, IDC_ARROW));
        if (s_cursorChecks > 0 && uMsg == WM_INPUT)
        {
            --s_cursorChecks;
            CURSORINFO info{sizeof(CURSORINFO)};
            GetCursorInfo(&info);
            spdlog::info("Overlay cursor: thread {}, showing {}, shape {}, this thread's {}", GetCurrentThreadId(), (info.flags & CURSOR_SHOWING) != 0,
                         static_cast<void*>(info.hCursor), static_cast<void*>(GetCursor()));
        }
    }
    else if (s_arrowShown)
    {
        while (ShowCursor(FALSE) >= 0)
            ;
        s_arrowShown = false;
    }
    else if (s_pOverlay->IsOff())
    {
        // Off for good (its process kept ending): what shows the cursor for a card or F2 gets no overlay to use it on
        CURSORINFO info{sizeof(CURSORINFO)};
        if (GetCursorInfo(&info) && (info.flags & CURSOR_SHOWING))
            ShowCursor(FALSE);
    }

    // Keys and mouse buttons held as the overlay took over: the game still gets their release, or it keeps walking or
    // aiming down the sights until they're pressed again. Only those releases; everything else stays the overlay's
    static std::bitset<256> s_heldForGame;
    static bool s_wasActive = false;
    if (active && !s_wasActive)
        for (int key = 1; key < 255; ++key)
            if (GetAsyncKeyState(key) & 0x8000)
                s_heldForGame.set(key);
    if (!active)
        s_heldForGame.reset();
    // The cursor's clip goes as the overlay opens and comes back as it closes, if the game is still the window in front
    if (active && !s_wasActive && RealClipCursor)
        RealClipCursor(nullptr);
    if (!active && s_wasActive && RealClipCursor && GetForegroundWindow() == hwnd)
    {
        RECT window{};
        if (GetWindowRect(hwnd, &window))
            RealClipCursor(&window);
    }
    s_wasActive = active;
    bool releaseForGame = false;
#endif

    POINT position;

    GetCursorPos(&position);
    ScreenToClient(GetActiveWindow(), &position);

    ProcessMouseMove(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y));

    if (uMsg == WM_INPUT)
    {
        RAWINPUT input;
        UINT size = sizeof(RAWINPUT);

        GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER));

        if (active)
        {
            auto& imgui = World::Get().ctx().at<ImguiService>();
            imgui.RawInputHandler(input);
        }

        if (input.header.dwType == RIM_TYPEKEYBOARD)
        {
            const auto keyboard = input.data.keyboard;
#if TP_FALLOUT4
            if (active && (keyboard.Flags & RI_KEY_BREAK) && keyboard.VKey < 256 && s_heldForGame.test(keyboard.VKey))
            {
                s_heldForGame.reset(keyboard.VKey);
                releaseForGame = true;
            }
#endif

            ProcessKeyboard(keyboard.VKey, keyboard.MakeCode, keyboard.Flags & RI_KEY_BREAK ? KEYEVENT_KEYUP : KEYEVENT_KEYDOWN, keyboard.Flags & RI_KEY_E0, keyboard.Flags & RI_KEY_E1);
        }
        else if (input.header.dwType == RIM_TYPEMOUSE)
        {
            const auto mouse = input.data.mouse;
#if TP_FALLOUT4
            const std::pair<USHORT, int> buttons[] = {{RI_MOUSE_LEFT_BUTTON_UP, VK_LBUTTON}, {RI_MOUSE_RIGHT_BUTTON_UP, VK_RBUTTON}, {RI_MOUSE_MIDDLE_BUTTON_UP, VK_MBUTTON}};
            for (const auto& [flag, key] : buttons)
            {
                if (active && (mouse.usButtonFlags & flag) && s_heldForGame.test(key))
                {
                    s_heldForGame.reset(key);
                    releaseForGame = true;
                }
            }
#endif

            if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
            {
                ProcessMouseWheel(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), (int16_t)mouse.usButtonData);
            }

            if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN)
            {
#if TP_FALLOUT4
                if (active && s_clickChecks > 0)
                {
                    --s_clickChecks;
                    RECT client{};
                    GetClientRect(hwnd, &client);
                    spdlog::info("Overlay click: at {}, {} of the window's {} x {} (active window {}, this one {})", position.x, position.y, client.right,
                                 client.bottom, static_cast<void*>(GetActiveWindow()), static_cast<void*>(hwnd));
                }
#endif
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_LEFT, true);
            }

            if (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP)
            {
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_LEFT, false);
            }

            if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN)
            {
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_RIGHT, true);
            }

            if (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP)
            {
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_RIGHT, false);
            }

            if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN)
            {
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_MIDDLE, true);
            }

            if (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP)
            {
                ProcessMouseButton(static_cast<uint16_t>(position.x), static_cast<uint16_t>(position.y), MBT_MIDDLE, false);
            }
        }
    }
    else if (uMsg == WM_CHAR)
    {
        uint16_t scancode = (lParam >> 16) & 0xFF;
        uint16_t virtualKey = static_cast<uint16_t>(wParam);
        if (!IsWindowUnicode(hwnd))
        {
            wchar_t wch;
            ::MultiByteToWideChar(s_currentACP, MB_PRECOMPOSED, reinterpret_cast<char*>(&virtualKey), 2, &wch, sizeof(wchar_t));
            virtualKey = wch;
        }
        ProcessKeyboard(virtualKey, scancode, KEYEVENT_CHAR, false, false);
    }
    else if (uMsg == WM_INPUTLANGCHANGE)
    {
        s_currentACP = GetRealACP();
        spdlog::info("Input language changed, current ACP: {}", s_currentACP);
    }

#if TP_FALLOUT
    // Fallout specific code to disable input (but for the release of a key or button held as the overlay opened)
    if (active && !releaseForGame && IsOverlayInput(uMsg))
        return 1;
#endif

    return 0;
}

InputService::InputService(OverlayService& aOverlay) noexcept
{
    s_pOverlay = &aOverlay;
    s_currentACP = GetRealACP();
}

InputService::~InputService() noexcept
{
    s_pOverlay = nullptr;
}
