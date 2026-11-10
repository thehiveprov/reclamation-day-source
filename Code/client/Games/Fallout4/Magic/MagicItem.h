#pragma once

#include <Forms/TESBoundObject.h>
#include <Components/TESFullName.h>
#include <Components/BGSKeywordForm.h>
#include <Magic/EffectItem.h>
#include <Magic/EffectSetting.h>

struct MagicItem : TESBoundObject, TESFullName, BGSKeywordForm
{
    // The spell's effect item for a magic effect (form id), null if it has none
    EffectItem* GetEffect(const uint32_t aEffectId) noexcept
    {
        for (uint32_t i = 0; i < listOfEffects.length; ++i)
        {
            EffectItem* pEffect = listOfEffects[i];
            if (pEffect && pEffect->pEffectSetting && pEffect->pEffectSetting->formID == aEffectId)
                return pEffect;
        }
        return nullptr;
    }

    GameArray<EffectItem*> listOfEffects;
    int32_t iHostileCount;
    EffectSetting* pAVEffectSetting;
    uint32_t uiPreloadCount;
    void* spPreloadedItem;
};
