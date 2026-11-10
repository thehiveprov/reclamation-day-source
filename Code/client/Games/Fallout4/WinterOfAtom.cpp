#include <HivemindPCH.h>

#include <WinterOfAtom.h>

#include <Games/TES.h>
#include <Forms/TESForm.h>
#include <Forms/TESGlobal.h>
#include <TESObjectREFR.h>
#include <Forms/TESTopicInfo.h>

#include <chrono>

namespace WinterOfAtom
{
namespace
{
const Mod* FindLoadedPlugin(const char* acpName) noexcept
{
    const ModManager* pManager = ModManager::Get();
    const Mod* pMod = pManager ? pManager->GetByName(acpName) : nullptr;
    return pMod && pMod->IsLoaded() ? pMod : nullptr;
}

// One of kMainPlugin's own records (not an override of another plugin's)
bool IsOwnForm(uint32_t aFormId) noexcept
{
    const Mod* pMod = FindLoadedPlugin(kMainPlugin);
    return pMod && pMod->GetFormId(aFormId & (pMod->IsLite() ? 0xFFF : 0xFFFFFF)) == aFormId;
}

// The game's subtitle settings (read from the 1.10.163 exe: ShowSubtitle, id 875508, shows a line in a conversation
// only with bDialogueSubtitles on, else only with bGeneralSubtitles on, and the subtitle's update, 102120, checks them
// again every frame). Setting objects, their value at +8 (RD ids: CommonLibF4's + 1). Written here directly: the
// game's setter (900117) also saves the player's preferences, which these stay out of.
uint8_t& SubtitleSetting(bool aDialogue) noexcept
{
    POINTER_FALLOUT4(uint8_t, s_dialogue, 641583); // bDialogueSubtitles:Interface
    POINTER_FALLOUT4(uint8_t, s_general, 1565751); // bGeneralSubtitles:Interface
    return (aDialogue ? s_dialogue.Get() : s_general.Get())[8];
}

// A conversation with the player goes on: MenuTopicManager (its singleton, id 1391114) + 0x4C, as ShowSubtitle reads it
bool InConversation() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_manager, 1391115);
    const uint8_t* pManager = *s_manager.Get();
    return pManager && pManager[0x4C] != 0;
}

// While our lines are up the settings are on, the player's own kept to put back: after the conversation, and a while
// after the last line (an ambient line stays up as long as its voice file)
constexpr auto kLinger = std::chrono::seconds(10);
bool s_forced = false;
uint8_t s_playerDialogue = 0;
uint8_t s_playerGeneral = 0;
std::chrono::steady_clock::time_point s_lastLine{};
} // namespace

bool IsPrequelLoaded() noexcept
{
    return FindLoadedPlugin(kPrequelPlugin) != nullptr;
}

TESGlobal* GetGlobal(uint32_t aLocalId) noexcept
{
    const Mod* pMod = FindLoadedPlugin(kMainPlugin);
    return pMod ? Cast<TESGlobal>(TESForm::GetById(pMod->GetFormId(aLocalId))) : nullptr;
}

bool IsStoryActor(const TESObjectREFR* apRef) noexcept
{
    return apRef && apRef->baseForm && IsOwnForm(apRef->baseForm->formID);
}

void OnSubtitle(const TESTopicInfo* apInfo) noexcept
{
    if (!apInfo || !IsOwnForm(apInfo->formID))
        return;

    s_lastLine = std::chrono::steady_clock::now();
    if (s_forced)
        return;
    s_playerDialogue = SubtitleSetting(true);
    s_playerGeneral = SubtitleSetting(false);
    SubtitleSetting(true) = 1;
    SubtitleSetting(false) = 1;
    s_forced = true;
    spdlog::info("Winter of Atom: subtitles on for its unvoiced lines (the player's: dialogue {}, general {})", s_playerDialogue, s_playerGeneral);
}

void KeepSubtitlesOn() noexcept
{
    s_lastLine = std::chrono::steady_clock::now();
    if (s_forced)
        return;
    s_playerDialogue = SubtitleSetting(true);
    s_playerGeneral = SubtitleSetting(false);
    SubtitleSetting(true) = 1;
    SubtitleSetting(false) = 1;
    s_forced = true;
}

void UpdateSubtitles() noexcept
{
    if (!s_forced || InConversation() || std::chrono::steady_clock::now() - s_lastLine < kLinger)
        return;

    SubtitleSetting(true) = s_playerDialogue;
    SubtitleSetting(false) = s_playerGeneral;
    s_forced = false;
    spdlog::info("Winter of Atom: subtitles back to the player's settings");
}
} // namespace WinterOfAtom
