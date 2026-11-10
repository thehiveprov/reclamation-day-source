#include <Games/Fallout4/DialogueGame.h>

#include <PlayerCharacter.h>
#include <TESObjectREFR.h>
#include <Interface/IMenu.h>
#include <Interface/Scaleform.h>
#include <Interface/UI.h>
#include <Misc/BSFixedString.h>

#include <algorithm>

// Address library ids here are Reclamation Day's (CommonLibF4's + 1). Read from the decrypted 1.10.163 exe:
//   SelectDialogueOption (player, option)    CommonLibF4 696117  0x140EB8C50, XDI's "4C 8B DC 56 48 83 EC 30"
//   MenuTopicManager singleton               CommonLibF4 1391114 0x1458CEB30
//   GetInfoForPlayerDialogueOption           CommonLibF4 781358  0x1402C6170 (action, scene, target, option)
//   GetQuestAliasHandle (quest, out, alias)  CommonLibF4 688914  0x1405DCF60
//   the prompt of a topic info               CommonLibF4 1435199 0x140621F00 (its hash set, else its parent's; "" none)
namespace
{
using TSelectDialogueOption = void(void* apPlayer, uint32_t aOption);
TSelectDialogueOption* RealSelectDialogueOption = nullptr;

bool (*s_pickHandler)(int32_t) = nullptr;
bool s_ourPick = false;

void HookSelectDialogueOption(void* apPlayer, uint32_t aOption)
{
    if (!s_ourPick && s_pickHandler && !s_pickHandler(static_cast<int32_t>(aOption)))
        return;
    RealSelectDialogueOption(apPlayer, aOption);
}

// MenuTopicManager (XDI's layout): awaitingPlayerInput at 0x55
const uint8_t* TopicManager() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_manager, 1391115);
    return *s_manager.Get();
}

// BGSScene (CommonLibF4): actions (a BSTArray<BGSSceneAction*>: its data at 0x90, its size at 0xA0), parentQuest 0xA8,
// currentActivePhase 0xCC. TESObjectREFR::GetCurrentScene is the vtable's 0x5C.
void* CurrentScene(PlayerCharacter* apPlayer) noexcept
{
    using TGetCurrentScene = void*(void*);
    auto** pVTable = *reinterpret_cast<void***>(apPlayer);
    return reinterpret_cast<TGetCurrentScene*>(pVTable[0x5C])(apPlayer);
}

// An actor's speech (1.10.163 exe): TESObjectREFR::IsTalking, the vtable's 0x4E (1499177), is "talking" unless both
// bits 7 and 8 of +0x2D0 are set and the voice timer (+0x38C) is at 0 or below. UpdateInDialogue (0x5E, 841563) clears
// them as a line starts and sets +0x38C and +0x390 to the line's length (its sound's); UpdateSoundCallBack (0x51,
// 632367), the voice sound's end, sets them again; StopCurrentDialogue (0x61, 1485290) sets them, the timer -1 and the
// length 0. The player's end never came after the prequel's start, a pick in the trader's menu or a death's grunt
// (10-29, read live: bit 8 off, the timer at 0.467 for minutes), so no NPC would talk (TESNPC::Activate's 611087 asks
// the player's IsTalking), XDI's menu never showed the choices and a picked line never ended. A load or a respawn
// (the actor reset, 0x180 again) let it go.
constexpr uint32_t kIsTalkingSlot = 0x4E;
constexpr uint32_t kStopCurrentDialogueSlot = 0x61;
constexpr size_t kSpeechFlags = 0x2D0;
constexpr size_t kLineLength = 0x390;
// past the line's own length (its sound's): with 3 s the prequel's blizzard kept the player "talking" nearly all the
// time (a 2.5 s line every 5.5 s, 10-29 02:15-02:16), the gaps too short to start a conversation in
constexpr float kLineGrace = 0.5f;

float s_talkingFor = 0.f;
float s_lineLength = 0.f;

// BGSSceneAction (XDI's layout): GetType the vtable's 8 (3: player dialogue), aliasID 0x08, startPhase 0x0C, endPhase
// 0x0E, status 0x14 (1: running)
constexpr uint32_t kPlayerDialogueAction = 3;

uint8_t* PlayerDialogueAction(void* apScene) noexcept
{
    if (!apScene)
        return nullptr;
    auto* pScene = static_cast<uint8_t*>(apScene);
    auto** ppActions = *reinterpret_cast<uint8_t***>(pScene + 0x90);
    const uint32_t count = *reinterpret_cast<uint32_t*>(pScene + 0xA0);
    const uint32_t phase = *reinterpret_cast<uint32_t*>(pScene + 0xCC);
    for (uint32_t i = 0; ppActions && i < count; ++i)
    {
        uint8_t* pAction = ppActions[i];
        if (!pAction)
            continue;
        using TGetType = uint32_t(void*);
        auto** pVTable = *reinterpret_cast<void***>(pAction);
        if (reinterpret_cast<TGetType*>(pVTable[8])(pAction) != kPlayerDialogueAction)
            continue;
        const uint16_t start = *reinterpret_cast<uint16_t*>(pAction + 0x0C);
        const uint16_t end = *reinterpret_cast<uint16_t*>(pAction + 0x0E);
        if (pAction[0x14] == 1 || (start <= phase && phase <= end))
            return pAction;
    }
    return nullptr;
}

// What the player character says for a topic info: its first response's text (TESTopicInfo: dataInfo, the info it
// shares its lines with, at 0x30; responses at 0x48, a list whose first node holds the text's pool entry, as XDI reads
// it); "" when the game has none loaded
std::string ResponseLine(const uint8_t* apInfo) noexcept
{
    for (int depth = 0; apInfo && depth < 8; ++depth)
    {
        const auto* pShared = *reinterpret_cast<const uint8_t* const*>(apInfo + 0x30);
        if (!pShared)
            break;
        apInfo = pShared;
    }
    const auto* pList = apInfo ? *reinterpret_cast<const uint8_t* const*>(apInfo + 0x48) : nullptr;
    const auto* pText = pList ? *reinterpret_cast<const BSFixedString::Data* const*>(pList) : nullptr;
    const char* pLine = pText && pText->IsAscii() ? pText->AsAscii() : nullptr;
    return pLine ? pLine : "";
}

// The game's four, without XDI: each option's topic info (for the scene's target), its prompt and its line
std::vector<DialogueGame::Option> GameOptions() noexcept
{
    std::vector<DialogueGame::Option> options;
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    void* pScene = pPlayer ? CurrentScene(pPlayer) : nullptr;
    uint8_t* pAction = PlayerDialogueAction(pScene);
    if (!pAction)
        return options;

    using TGetQuestAliasHandle = bool(void* apQuest, uint32_t* apHandle, uint32_t aAlias);
    POINTER_FALLOUT4(TGetQuestAliasHandle, s_getAliasHandle, 688915);
    using TGetInfoForOption = void*(void* apAction, void* apScene, TESObjectREFR* apTarget, int32_t aOption);
    POINTER_FALLOUT4(TGetInfoForOption, s_getInfo, 781359);
    using TGetPrompt = const char*(void* apInfo);
    POINTER_FALLOUT4(TGetPrompt, s_getPrompt, 1435200);

    uint32_t handle = 0;
    void* pQuest = *reinterpret_cast<void**>(static_cast<uint8_t*>(pScene) + 0xA8);
    if (pQuest)
        s_getAliasHandle.Get()(pQuest, &handle, *reinterpret_cast<uint32_t*>(pAction + 0x08));
    TESObjectREFR* pTarget = handle ? TESObjectREFR::GetByHandle(handle) : nullptr;

    // question, positive, negative, neutral (as XDI lists them)
    for (const int32_t id : {3, 0, 1, 2})
    {
        void* pInfo = s_getInfo.Get()(pAction, pScene, pTarget, id);
        if (!pInfo)
            continue;
        const char* pPrompt = s_getPrompt.Get()(pInfo);
        DialogueGame::Option option{};
        option.Id = id;
        option.Prompt = pPrompt ? pPrompt : "";
        option.Line = ResponseLine(static_cast<const uint8_t*>(pInfo));
        options.push_back(std::move(option));
    }
    return options;
}

// XDI's: its DialogueMenu.swf's code object has GetDialogueOptions ([{optionID, prompt, response, enabled,
// challengeLevel...}]) and IsFrameworkActive (a scene that uses XDI's own list: picks are 5 + optionID)
bool XdiOptions(std::vector<DialogueGame::Option>& aOut) noexcept
{
    UI* pUI = UI::Get();
    IMenu* pMenu = pUI ? pUI->FindMenuByName("DialogueMenu") : nullptr;
    auto* pMovie = pMenu ? static_cast<GFx::Movie*>(pMenu->pMovie) : nullptr;
    GFx::Value code;
    if (!pMovie || !pMovie->GetVariable(&code, "root.Menu_mc.BGSCodeObj") || !code.IsObject())
        return false;
    GFx::Value function;
    if (!code.GetMember("GetDialogueOptions", &function) || function.GetType() == GFx::ValueType::kUndefined)
        return false;

    GFx::Value list;
    GFx::Value active;
    if (!code.Invoke("GetDialogueOptions", &list) || list.GetType() != GFx::ValueType::kArray)
        return false;
    code.Invoke("IsFrameworkActive", &active);
    const bool framework = active.GetType() == GFx::ValueType::kBoolean && active.value.boolean;

    const auto text = [](const GFx::Value& acObject, const char* acpName)
    {
        GFx::Value member;
        return acObject.GetMember(acpName, &member) && member.GetString() ? std::string(member.GetString()) : std::string();
    };
    const auto number = [](const GFx::Value& acObject, const char* acpName, double aDefault)
    {
        GFx::Value member;
        return acObject.GetMember(acpName, &member) && member.GetType() != GFx::ValueType::kUndefined ? member.GetNumber() : aDefault;
    };

    for (uint32_t i = 0; i < list.GetArraySize(); ++i)
    {
        GFx::Value entry;
        if (!list.GetElement(i, &entry) || !entry.IsObject())
            continue;
        DialogueGame::Option option{};
        const int32_t id = static_cast<int32_t>(number(entry, "optionID", -1.0));
        if (id < 0)
            continue;
        option.Id = framework ? id + 5 : id;
        option.Prompt = text(entry, "prompt");
        option.Line = text(entry, "response");
        option.Enabled = number(entry, "enabled", 1.0) != 0.0;
        const double challenge = number(entry, "challengeLevel", 0.0);
        option.Challenge = challenge > 0.0 && challenge < 16.0 ? static_cast<uint8_t>(challenge) : 0;
        aOut.push_back(std::move(option));
    }
    return true;
}
} // namespace

namespace DialogueGame
{
bool AwaitingPick() noexcept
{
    const uint8_t* pManager = TopicManager();
    return pManager && pManager[0x55] != 0;
}

uintptr_t CurrentAction() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    return pPlayer ? reinterpret_cast<uintptr_t>(PlayerDialogueAction(CurrentScene(pPlayer))) : 0;
}

std::vector<Option> ReadOptions() noexcept
{
    std::vector<Option> options;
    if (XdiOptions(options))
        return options;
    return GameOptions();
}

bool Pick(int32_t aId) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !AwaitingPick() || !RealSelectDialogueOption)
        return false;
    s_ourPick = true;
    RealSelectDialogueOption(pPlayer, static_cast<uint32_t>(aId));
    s_ourPick = false;
    return true;
}

void SetPickHandler(bool (*apHandler)(int32_t aOption)) noexcept
{
    s_pickHandler = apHandler;
}

void InstallHook() noexcept
{
    if (RealSelectDialogueOption)
        return;
    POINTER_FALLOUT4(TSelectDialogueOption, s_selectDialogueOption, 696118);
    RealSelectDialogueOption = s_selectDialogueOption.Get();
    TP_HOOK_IMMEDIATE(&RealSelectDialogueOption, HookSelectDialogueOption);
    spdlog::info("Dialogue: the pick hook is on (after the script extender's mods found the function)");
}

void UpdatePlayerVoice(float aDelta) noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
        return;
    auto** pVTable = *reinterpret_cast<void***>(pPlayer);
    using TIsTalking = bool(void*);
    if (!reinterpret_cast<TIsTalking*>(pVTable[kIsTalkingSlot])(pPlayer))
    {
        s_talkingFor = 0.f;
        s_lineLength = 0.f;
        return;
    }

    const auto* pBytes = reinterpret_cast<const uint8_t*>(pPlayer);
    s_talkingFor += aDelta;
    s_lineLength = std::max(s_lineLength, *reinterpret_cast<const float*>(pBytes + kLineLength));
    if (s_talkingFor < s_lineLength + kLineGrace)
        return;

    spdlog::warn("Dialogue: the player's line of {:.2f} s still talking {:.1f} s on (speech flags {:#x}): ended as the game ends one", s_lineLength,
                 s_talkingFor, *reinterpret_cast<const uint32_t*>(pBytes + kSpeechFlags));
    using TStopCurrentDialogue = void(void*, bool);
    reinterpret_cast<TStopCurrentDialogue*>(pVTable[kStopCurrentDialogueSlot])(pPlayer, true);
    s_talkingFor = 0.f;
    s_lineLength = 0.f;
}
} // namespace DialogueGame
