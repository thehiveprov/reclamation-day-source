#include "SubtitleManager.h"

#include <Events/SubtitleEvent.h>

#include <TESObjectREFR.h>
#include <Actor.h>
#include <Forms/TESNPC.h>
#include <Games/ActorExtension.h>

#include <Forms/TESTopicInfo.h>
#include <Misc/BSFixedString.h>

#if TP_FALLOUT4
#include <WinterOfAtom.h>
#endif

SubtitleManager* SubtitleManager::Get() noexcept
{
    POINTER_SKYRIMSE(SubtitleManager*, s_singleton, 400443);
    POINTER_FALLOUT4(SubtitleManager*, s_singleton, 740512);
    return *s_singleton.Get();
}

#if TP_SKYRIM64
TP_THIS_FUNCTION(TShowSubtitle, void, SubtitleManager, TESObjectREFR* apSpeaker, const char* apSubtitleText, bool aIsInDialogue);
#elif TP_FALLOUT4
TP_THIS_FUNCTION(TShowSubtitle, void, SubtitleManager, TESObjectREFR* apSpeaker, BSFixedString* apSubtitleText, TESTopicInfo* apTopicInfo, bool aIsInDialogue);
#endif
static TShowSubtitle* RealShowSubtitle = nullptr;

void SubtitleManager::ShowSubtitle(TESObjectREFR* apSpeaker, const char* apSubtitleText, TESTopicInfo* apTopicInfo, bool aUnk1) noexcept
{
#if TP_SKYRIM64
    TiltedPhoques::ThisCall(RealShowSubtitle, this, apSpeaker, apSubtitleText, aUnk1);
#elif TP_FALLOUT4
    BSFixedString subtitleText(apSubtitleText);
    TiltedPhoques::ThisCall(RealShowSubtitle, this, apSpeaker, &subtitleText, apTopicInfo, aUnk1);
    subtitleText.Release();
#endif
}

TP_THIS_FUNCTION(THideSubtitle, void*, SubtitleManager, TESObjectREFR* apSpeaker);
static THideSubtitle* RealHideSubtitle = nullptr;

// Fallout 4 id checked: the game calls it with SubtitleManager's singleton (740511)
void* SubtitleManager::HideSubtitle(TESObjectREFR* apSpeaker) noexcept
{
    POINTER_SKYRIMSE(THideSubtitle, s_hideSubtitle, 52627);
    POINTER_FALLOUT4(THideSubtitle, s_hideSubtitle, 755148);
    return TiltedPhoques::ThisCall(s_hideSubtitle, this, apSpeaker);
}

#if TP_SKYRIM64
void TP_MAKE_THISCALL(HookShowSubtitle, SubtitleManager, TESObjectREFR* apSpeaker, const char* apSubtitleText, bool aIsInDialogue)
#elif TP_FALLOUT4
void TP_MAKE_THISCALL(HookShowSubtitle, SubtitleManager, TESObjectREFR* apSpeaker, BSFixedString* apSubtitleText, TESTopicInfo* apTopicInfo, bool aIsInDialogue)
#endif
{
    // spdlog::debug("Subtitle for actor {:X} (bool {}):\n\t{}", apSpeaker ? apSpeaker->formID : 0, aIsInDialogue, apSubtitleText);

    Actor* pActor = Cast<Actor>(apSpeaker);
    // Includes the local player's own lines, so players following a conversation see the choices that were made
    if (apSubtitleText && pActor && pActor->GetExtension()->IsLocal())
#if TP_SKYRIM64
        World::Get().GetRunner().Trigger(SubtitleEvent(apSpeaker->formID, apSubtitleText));
#elif TP_FALLOUT4
        World::Get().GetRunner().Trigger(SubtitleEvent(apSpeaker->formID, apSubtitleText->AsAscii()));
#endif

#if TP_SKYRIM64
    TiltedPhoques::ThisCall(RealShowSubtitle, apThis, apSpeaker, apSubtitleText, aIsInDialogue);
#elif TP_FALLOUT4
    // Winter of Atom's unvoiced lines are subtitled even with the player's subtitles off (it turns them on first)
    WinterOfAtom::OnSubtitle(apTopicInfo);
    // And so is the caravan trader's thanks, on every player's game
    if (const auto* pNpc = pActor && !apTopicInfo ? Cast<TESNPC>(pActor->baseForm) : nullptr)
    {
        const char* pName = pNpc->fullName.value.AsAscii();
        if (pName && !strcmp(pName, "Caravan Trader"))
            WinterOfAtom::KeepSubtitlesOn();
    }
    TiltedPhoques::ThisCall(RealShowSubtitle, apThis, apSpeaker, apSubtitleText, apTopicInfo, aIsInDialogue);
#endif
}

#if TP_FALLOUT4
// The game's quit ends the UI before the world: its shutdown (id 234348) clears SubtitleManager's singleton (id 74102)
// and only then deletes TES, whose unloading references stop any line they're still saying (ExtraSayToTopicInfo, ids
// 1125030 and 140592) and hide its subtitle through the null singleton. That read of null + 8 (id 1373836, 0x1412B4051)
// ended nearly every quit since 10-10 in RD's crash handler, a crash dump each time. No subtitle is left to hide then.
void* TP_MAKE_THISCALL(HookHideSubtitle, SubtitleManager, TESObjectREFR* apSpeaker)
{
    if (!apThis)
        return nullptr;

    return TiltedPhoques::ThisCall(RealHideSubtitle, apThis, apSpeaker);
}
#endif

TiltedPhoques::Initializer s_subtitleHooks(
    []()
    {
        POINTER_SKYRIMSE(TShowSubtitle, s_showSubtitle, 52626);
        POINTER_FALLOUT4(TShowSubtitle, s_showSubtitle, 875509);

        RealShowSubtitle = s_showSubtitle.Get();

        TP_HOOK(&RealShowSubtitle, HookShowSubtitle);

#if TP_FALLOUT4
        POINTER_FALLOUT4(THideSubtitle, s_hideSubtitle, 755148);

        RealHideSubtitle = s_hideSubtitle.Get();

        TP_HOOK(&RealHideSubtitle, HookHideSubtitle);
#endif
    });
