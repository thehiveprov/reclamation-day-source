#pragma once

#include "ExtraData.h"

struct TESBoundObject;

struct BaseExtraList
{
    BSExtraData* pHead;
    BSExtraData** ppTail;
    uint8_t* pFlags; // pFlags is 0x1B large
};

struct ExtraDataList : BSIntrusiveRefCounted
{
    static ExtraDataList* New() noexcept;

    bool Contains(ExtraDataType aType) const;
    void Set(ExtraDataType aType, bool aSet);

    bool Add(ExtraDataType aType, BSExtraData* apNewData);
    bool Remove(ExtraDataType aType, BSExtraData* apNewData);

    uint32_t GetCount() const;

    void SetType(ExtraDataType aType, bool aClear);
    BSExtraData* GetByType(ExtraDataType type) const;

    // The quests (form ids) whose aliases the reference or item stack with this list fills: quest items have some
    Vector<uint32_t> GetAliasQuests() const noexcept;

    // The name a player gave the item (renamed at a workbench), empty for the game's own name; and giving it one
    String GetCustomName() const noexcept;
    void SetCustomName(const char* acpName) noexcept;

    // Works out what the mods in this list (its BGSObjectInstanceExtra) make of apObject and keeps the result in the
    // list, where the game reads it when the item is put on. The game only does this for the mods it picks itself.
    void CreateInstanceData(TESBoundObject* apObject) noexcept;

    BaseExtraList ExtraData;
    // Taken only through ExtraDataList.cpp's ReadLock/WriteLock, the game's own way: the game's job threads read the
    // same lists at the same time
    mutable BSReadWriteLock ExtraRWLock;
};
