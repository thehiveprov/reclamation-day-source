#include "MenuTopicManager.h"

#include <Events/PlayerDialogueEvent.h>

TP_THIS_FUNCTION(TPlayDialogueOption, bool, MenuTopicManager, int32_t aIndex);
static TPlayDialogueOption* RealPlayDialogueOption = nullptr;

// Skyrim only: on Fallout 4 one player at a time talks to an NPC (the dialogue lock, ObjectService); showing the
// others the choices is the planned dialogue work (XDI)
MenuTopicManager* MenuTopicManager::Get() noexcept
{
#if TP_SKYRIM64
    POINTER_SKYRIMSE(MenuTopicManager*, s_singleton, 401099);
    return *s_singleton.Get();
#else
    return nullptr;
#endif
}

bool TP_MAKE_THISCALL(HookPlayDialogueOption, MenuTopicManager, int32_t aIndex)
{
    if (apThis->pOptions)
    {
        int i = 0;
        const char* pText = nullptr;
        for (auto option : *apThis->pOptions)
        {
            if (i == aIndex)
            {
                pText = option->text;
                break;
            }
            i++;
        }

        if (pText != nullptr)
            World::Get().GetRunner().Trigger(PlayerDialogueEvent(pText));
    }

    return TiltedPhoques::ThisCall(RealPlayDialogueOption, apThis, aIndex);
}

TiltedPhoques::Initializer s_menuTopicHooks(
    []()
    {
// Skyrim only (see MenuTopicManager::Get)
#if TP_SKYRIM64
        POINTER_SKYRIMSE(TPlayDialogueOption, s_playDialogueOption, 35269);

        RealPlayDialogueOption = s_playDialogueOption.Get();

        TP_HOOK(&RealPlayDialogueOption, HookPlayDialogueOption);
#endif
    });
