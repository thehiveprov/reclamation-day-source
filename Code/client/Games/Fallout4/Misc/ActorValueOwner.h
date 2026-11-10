#pragma once

#include <Forms/ActorValueInfo.h>

struct ActorValueOwner
{
    enum class ForceMode : uint32_t
    {
        PERMANENT = 0,
        TEMPORARY = 1,
        DAMAGE = 2,
        COUNT = 3,
    };

    virtual ~ActorValueOwner();

    // TODO: refactor these names. Read from the exe (1.10.163) for a reference's owner (TESObjectREFR + 0x58): Papyrus'
    // ObjectReference.GetValue calls slot 1, GetBaseValue 3, ModValue 6, DamageValue 6 with mode DAMAGE (2),
    // RestoreValue 8, SetValue 9 (docs/SYNC_PASS.md, the check after gap 7)
    virtual float GetValue(ActorValueInfo* apInfo) const noexcept;
    virtual float GetMaxValue(ActorValueInfo* apInfo) const noexcept;
    virtual float GetBaseValue(ActorValueInfo* apInfo) const noexcept;
    virtual void sub_04();
    virtual void sub_05();
    virtual void ForceCurrent(ActorValueOwner::ForceMode aMode, ActorValueInfo* apInfo, float aValue) noexcept;
    virtual void sub_07();
    virtual void RestoreValue(ActorValueInfo* apInfo, float aAmount) noexcept;
    virtual void SetValue(ActorValueInfo* apInfo, float aValue) noexcept;
};
