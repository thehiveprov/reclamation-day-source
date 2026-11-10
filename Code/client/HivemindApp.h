#pragma once

#include <BranchInfo.h>
#include <d3d11.h>

#if (!IS_MASTER)
#include "CrashHandler.h"
#else
#include <crash_handler/CrashHandler.h>
#endif

struct HivemindApp final : App
{
    HivemindApp();
    virtual ~HivemindApp();

    HivemindApp(HivemindApp&&) = delete;
    HivemindApp(const HivemindApp&) = delete;
    HivemindApp& operator=(HivemindApp&&) = delete;
    HivemindApp& operator=(const HivemindApp&) = delete;

    [[nodiscard]] void* GetMainAddress() const override;

    bool BeginMain() override;
    bool EndMain() override;

    void Update() override;
#if TP_FALLOUT4
    // Called every frame from the game's main loop, menus included (the game skips frames while its window is
    // inactive, unless it is kept running in the background)
    void OnMainLoop() noexcept;
    // The game's frames so far: one per main loop pass (the update event can come several times a frame)
    [[nodiscard]] static uint64_t GetFrameCount() noexcept { return s_frameCount.load(std::memory_order_relaxed); }
#endif

    bool Attach() override;
    bool Detach() override;

    static void InstallHooks2();
    // Keeps the game running while its window is in the background (two instances on one PC, alt-tabbing)
    static void KeepRunningInBackground() noexcept;

protected:
    void InstallHooks();
    void UninstallHooks();

private:
    void ApplyNvidiaFix() noexcept;
#if (!IS_MASTER)
    CrashHandler m_crashHandler;
#else
    //ScopedCrashHandler m_crashHandler;
#endif
    ID3D11Device* m_pDevice = nullptr;
#if TP_FALLOUT4
    static inline std::atomic<uint64_t> s_frameCount{0};
#endif
};
