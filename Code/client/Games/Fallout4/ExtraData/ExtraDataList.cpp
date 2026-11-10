#include "ExtraDataList.h"

#include <Forms/TESForm.h>

namespace
{
// The list's lock is the game's BSReadWriteLock: readers count up in uiLock, a writer sets 0x80000001 and its thread.
// Taken with the game's own functions and given back as the game does (CommonLibF4's unlock_read/unlock_write).
// It was taken as a BSRecursiveLock (count 0 -> 1, given back with a 1 -> 0 exchange), which the game reads as one
// reader: a game job thread could read the list meanwhile (1 -> 2), the exchange then failed, the count stayed at 1
// with nobody holding it, and the next write to that actor's list waited forever with the main loop waiting on that
// job (the freezes near Diamond City, 2026-10-17)
struct ReadLock
{
    explicit ReadLock(BSReadWriteLock& aLock) noexcept
        : m_lock(aLock)
    {
        // BSReadWriteLock::lock_read (CommonLibF4 id 1573164): only waits while a writer on another thread holds it
        TP_THIS_FUNCTION(TLockRead, void, BSReadWriteLock);
        POINTER_FALLOUT4(TLockRead, s_lockRead, 1573165);

        TiltedPhoques::ThisCall(s_lockRead, &m_lock);
    }

    ~ReadLock() { InterlockedDecrement(reinterpret_cast<volatile LONG*>(&m_lock.uiLock)); }

private:
    BSReadWriteLock& m_lock;
};

struct WriteLock
{
    explicit WriteLock(BSReadWriteLock& aLock) noexcept
        : m_lock(aLock)
    {
        // BSReadWriteLock::lock_write (CommonLibF4 id 336186): waits until no reader or other writer holds it
        TP_THIS_FUNCTION(TLockWrite, void, BSReadWriteLock);
        POINTER_FALLOUT4(TLockWrite, s_lockWrite, 336187);

        TiltedPhoques::ThisCall(s_lockWrite, &m_lock);
    }

    ~WriteLock()
    {
        if (m_lock.uiLock == 0x80000001)
        {
            m_lock.uiWriterThread = 0;
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&m_lock.uiLock), 0);
        }
        else
            InterlockedDecrement(reinterpret_cast<volatile LONG*>(&m_lock.uiLock));
    }

private:
    BSReadWriteLock& m_lock;
};
} // namespace

ExtraDataList* ExtraDataList::New() noexcept
{
    ExtraDataList* pExtraDataList = Memory::Allocate<ExtraDataList>();
    // Raw game heap memory: zero it so the intrusive ref count starts at 0 and nothing is left uninitialized.
    std::memset(static_cast<void*>(pExtraDataList), 0, sizeof(ExtraDataList));

    pExtraDataList->ExtraData.pHead = nullptr;
    // The game appends through the tail pointer, which must point at the last entry's next field (head when empty).
    pExtraDataList->ExtraData.ppTail = &pExtraDataList->ExtraData.pHead;
    pExtraDataList->ExtraData.pFlags = static_cast<uint8_t*>(Memory::Allocate(0x1B));
    memset(pExtraDataList->ExtraData.pFlags, 0, 0x1B);

    pExtraDataList->ExtraRWLock.uiLock = pExtraDataList->ExtraRWLock.uiWriterThread = 0;

    return pExtraDataList;
}

void ExtraDataList::CreateInstanceData(TESBoundObject* apObject) noexcept
{
    // ExtraDataList::CreateInstanceData (CommonLibF4 id 1280130). Puts an ExtraInstanceData in the list, and names
    // the item after its mods like the game does for the items it makes.
    TP_THIS_FUNCTION(TCreateInstanceData, void*, ExtraDataList, TESBoundObject* apObject, bool aGenerateName);
    POINTER_FALLOUT4(TCreateInstanceData, s_createInstanceData, 1280131);

    TiltedPhoques::ThisCall(s_createInstanceData, this, apObject, true);
}

String ExtraDataList::GetCustomName() const noexcept
{
    // ExtraTextDisplayData (CommonLibF4): the display name at 0x18. A player's own name has bit 0 of the extra data's
    // flags (0x10) set (ExtraDataList::SetOverrideName sets it, id 222303); the names the game makes from an item's
    // mods ("Long ... Pistol", SetDisplayNameFromInstanceData, id 457340) don't, and are made again by every copy
    const auto* pExtra = reinterpret_cast<const uint8_t*>(GetByType(ExtraDataType::TextDisplayData));
    if (!pExtra || (*reinterpret_cast<const uint16_t*>(pExtra + 0x10) & 1) == 0)
        return {};
    const auto* pName = reinterpret_cast<const BSFixedString*>(pExtra + 0x18);
    const char* pText = pName->data ? pName->AsAscii() : nullptr;
    return pText ? String(pText) : String{};
}

void ExtraDataList::SetCustomName(const char* acpName) noexcept
{
    // ExtraDataList::SetOverrideName (CommonLibF4 id 222303), as the game names an item renamed at a workbench
    TP_THIS_FUNCTION(TSetOverrideName, void, ExtraDataList, const char* acpName);
    POINTER_FALLOUT4(TSetOverrideName, s_setOverrideName, 222304);

    TiltedPhoques::ThisCall(s_setOverrideName, this, acpName);
}

Vector<uint32_t> ExtraDataList::GetAliasQuests() const noexcept
{
    Vector<uint32_t> quests;

    // ExtraAliasInstanceArray (alandtse/CommonLibF4): BSTArray<BGSRefAliasInstanceData> at 0x18 (data, capacity,
    // size at 0x28), entries of 0x18 bytes starting with their TESQuest*
    const auto* pExtra = reinterpret_cast<const uint8_t*>(GetByType(ExtraDataType::AliasInstanceArray));
    if (!pExtra)
        return quests;

    ReadLock _(ExtraRWLock);

    const auto* pEntries = *reinterpret_cast<const uint8_t* const*>(pExtra + 0x18);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(pExtra + 0x28);
    for (uint32_t i = 0; pEntries && i < count && i < 64; ++i)
    {
        const auto* pQuest = *reinterpret_cast<const TESForm* const*>(pEntries + i * 0x18);
        if (pQuest && std::find(quests.begin(), quests.end(), pQuest->formID) == quests.end())
            quests.push_back(pQuest->formID);
    }

    return quests;
}

bool ExtraDataList::Contains(ExtraDataType aType) const
{
    ReadLock _(ExtraRWLock);

    if (ExtraData.pFlags)
    {
        const uint32_t value = static_cast<uint32_t>(aType);
        const uint32_t index = value >> 3;

        if (index >= 0x1B)
            return false;

        const uint8_t element = ExtraData.pFlags[index];

        return (element >> (value % 8)) & 1;
    }

    return false;
}

BSExtraData* ExtraDataList::GetByType(ExtraDataType aType) const
{
    ReadLock _(ExtraRWLock);

    if (!Contains(aType))
        return nullptr;

    BSExtraData* pEntry = ExtraData.pHead;
    while (pEntry != nullptr && pEntry->type != aType)
    {
        pEntry = pEntry->next;
    }

    return pEntry;
}

bool ExtraDataList::Add(ExtraDataType aType, BSExtraData* apNewData)
{
    if (Contains(aType))
        return false;

    WriteLock _(ExtraRWLock);

    BSExtraData* pNext = ExtraData.pHead;
    ExtraData.pHead = apNewData;
    apNewData->next = pNext;

    // Prepending to an empty list makes the new entry the last one too, keep the tail pointing at its next field.
    if (!pNext)
        ExtraData.ppTail = &apNewData->next;

    SetType(aType, false);

    return true;
}

uint32_t ExtraDataList::GetCount() const
{
    uint32_t count = 0;

    BSExtraData* pNext = ExtraData.pHead;
    while (pNext)
    {
        count++;
        pNext = pNext->next;
    }

    return count;
}

void ExtraDataList::SetType(ExtraDataType aType, bool aClear)
{
    uint32_t index = static_cast<uint8_t>(aType) >> 3;
    uint8_t bitmask = 1 << (static_cast<uint8_t>(aType) % 8);
    uint8_t& flag = ExtraData.pFlags[index];
    if (aClear)
        flag &= ~bitmask;
    else
        flag |= bitmask;
}
