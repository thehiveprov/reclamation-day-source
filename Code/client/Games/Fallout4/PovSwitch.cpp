#include <HivemindPCH.h>

#include <PovSwitch.h>

#include <PlayerCharacter.h>
#include <Interface/UI.h>
#include <Misc/BSFixedString.h>

namespace PovSwitch
{
namespace
{
// RD ids: CommonLibF4's + 1
constexpr uint32_t kUserPovSwitch = 0x20; // USER_EVENT_FLAG kPOVSwitch
constexpr uint32_t kOtherCamSwitch = 8;   // what InputEnableLayer.EnableCamSwitch (native 571960) turns on and off

// BSInputEnableManager (id 781703), read from the exe: 0x118 / 0x11C the user and other events every layer has on,
// 0x128 the layers' lock (BSSpinLock::lock 1425657), 0x130 the layers' own events (8 bytes a layer: user, other; a bit
// off = that layer holds the event off; a free layer is all on), 0x140 how many, 0x160 their debug names
// (BSFixedString, how many at 0x170)
uint8_t* InputEnableManager() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_manager, 781704);
    return *s_manager.Get();
}

bool IsUserEventOn(uint32_t aFlags) noexcept
{
    using TIsOn = bool(void*, uint32_t);
    POINTER_FALLOUT4(TIsOn, s_isOn, 243831);
    uint8_t* pManager = InputEnableManager();
    return !pManager || s_isOn.Get()(pManager, aFlags);
}

bool IsOtherEventOn(uint32_t aFlags) noexcept
{
    using TIsOn = bool(void*, uint32_t);
    POINTER_FALLOUT4(TIsOn, s_isOn, 1027894);
    uint8_t* pManager = InputEnableManager();
    return !pManager || s_isOn.Get()(pManager, aFlags);
}

// The layers that hold the POV user event or the camera switch off, by name
std::string LayersHoldingItOff() noexcept
{
    uint8_t* pManager = InputEnableManager();
    if (!pManager)
        return {};
    auto& lock = *reinterpret_cast<BSRecursiveLock*>(pManager + 0x128);
    BSScopedLock<BSRecursiveLock> guard(lock);
    const auto* pEvents = *reinterpret_cast<const uint32_t* const*>(pManager + 0x130);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(pManager + 0x140);
    const auto* pNames = *reinterpret_cast<const BSFixedString* const*>(pManager + 0x160);
    const uint32_t nameCount = *reinterpret_cast<const uint32_t*>(pManager + 0x170);
    std::string out;
    for (uint32_t i = 0; pEvents && i < count && i < 256; ++i)
    {
        const uint32_t user = pEvents[i * 2], other = pEvents[i * 2 + 1];
        if ((user & kUserPovSwitch) && (other & kOtherCamSwitch))
            continue;
        const char* cpName = pNames && i < nameCount ? pNames[i].AsAscii() : nullptr;
        out += fmt::format("{}\"{}\" ({}{})", out.empty() ? "" : ", ", cpName ? cpName : "?", (user & kUserPovSwitch) ? "" : "POV",
                           (other & kOtherCamSwitch) ? "" : (user & kUserPovSwitch) ? "camera switch" : " + camera switch");
    }
    return out;
}

// the graph variables read here (made on first use: the game's string pool comes up after our module)
BSFixedString& BlockPovSwitch() noexcept
{
    static BSFixedString s_name("bBlockPOVSwitch");
    return s_name;
}

bool GraphBool(PlayerCharacter* apPlayer, BSFixedString& aName) noexcept
{
    bool value = false;
    return apPlayer->animationGraphHolder.GetVariableBool(&aName, &value) && value;
}

// UI::menuMode (CommonLibF4, 0x1E0): how many open menus take the game's controls
uint32_t MenuModeCount() noexcept
{
    UI* pUI = UI::Get();
    return pUI ? *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(pUI) + 0x1E0) : 0;
}

// PlayerCamera (id 1171980): its current state's id (CommonLibF4 CameraState: 0 first person, 8 third person)
uint32_t CameraStateId() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_camera, 1171981);
    const uint8_t* pCamera = *s_camera.Get();
    const uint8_t* pState = pCamera ? *reinterpret_cast<uint8_t* const*>(pCamera + 0x28) : nullptr;
    return pState ? *reinterpret_cast<const uint32_t*>(pState + 0x20) : 0xFFFFFFFF;
}
} // namespace

void OnPressed(bool aMapUp) noexcept
{
    if (aMapUp)
    {
        spdlog::info("Third person key: the Pip-Boy map is up, it lowers the map instead");
        return;
    }
    using TCanSwitch = bool();
    POINTER_FALLOUT4(TCanSwitch, s_canSwitch, 1498650);
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;
    // every press is logged (10-29: after the prequel's opening the view never switches, yet no refusal was logged)
    if (s_canSwitch.Get()())
    {
        spdlog::info("Third person key: the game allows the switch (camera state {}, {} menus taking the controls)", CameraStateId(),
                     MenuModeCount());
        return;
    }

    const bool userOn = IsUserEventOn(kUserPovSwitch), otherOn = IsOtherEventOn(kOtherCamSwitch);
    const uint32_t gunState = pPlayer->actorState.GetGunState();
    static BSFixedString s_pipboyUp("pipboyUp"), s_isSynced("bIsSynced"), s_animationDriven("bAnimationDriven");
    const bool blocked = GraphBool(pPlayer, BlockPovSwitch());
    const uint32_t menus = MenuModeCount();
    const uint32_t camera = CameraStateId();
    const bool pipboyUp = GraphBool(pPlayer, s_pipboyUp), synced = GraphBool(pPlayer, s_isSynced), driven = GraphBool(pPlayer, s_animationDriven);
    spdlog::info("Third person key: the game won't switch the view (camera state {}): POV event {}, camera switch {}{}, gun state {}, "
                 "bBlockPOVSwitch {}, {} menus taking the controls; pipboyUp {}, bIsSynced {}, bAnimationDriven {}",
                 camera, userOn ? "on" : "OFF", otherOn ? "on" : "OFF", userOn && otherOn ? "" : " (held off by " + LayersHoldingItOff() + ")",
                 gunState, blocked ? "ON" : "off", menus, pipboyUp, synced, driven);

    // The graph keeps it on with nothing on that blocks the view's switch (no Pip-Boy, furniture or paired animation,
    // the plain first or third person camera): left over, from a save or an idle that never let it go
    const bool onlyTheGraph = userOn && otherOn && gunState != 6 && gunState != 8 && menus == 0;
    if (blocked && onlyTheGraph && !pipboyUp && !synced && !driven && (camera == 0 || camera == 8))
    {
        pPlayer->animationGraphHolder.SetVariableBool(&BlockPovSwitch(), false);
        spdlog::info("Third person key: turned off a bBlockPOVSwitch left on; the switch {}", s_canSwitch.Get()() ? "goes through" : "is still held off");
    }
}
} // namespace PovSwitch
