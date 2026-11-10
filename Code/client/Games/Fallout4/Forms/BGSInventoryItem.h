#pragma once

#include <ExtraData/ExtraDataList.h>
#include <Forms/TESBoundObject.h>

struct BGSInventoryItem
{
    struct Stack : BSIntrusiveRefCounted
    {
        // Bits of usFlags. Only the slot bits say the stack is worn: the others are set without it being worn (e.g.
        // "should equip" on a stack the game wanted to equip on a remote character, where equipping is blocked).
        enum Flag : uint16_t
        {
            INV_SLOT_INDEX_1 = 1 << 0,
            INV_SLOT_INDEX_2 = 1 << 1,
            INV_SLOT_INDEX_3 = 1 << 2,
            INV_EQUIP_STATE_LOCKED = 1 << 3,
            INV_SHOULD_EQUIP = 1 << 4,
            INV_TEMPORARY = 1 << 5,
            INV_SLOT_MASK = INV_SLOT_INDEX_1 | INV_SLOT_INDEX_2 | INV_SLOT_INDEX_3,
        };

        virtual ~Stack();

        [[nodiscard]] bool IsEquipped() const noexcept { return (usFlags & INV_SLOT_MASK) != 0; }

        Stack* spNextStack;
        ExtraDataList* spExtra;
        uint32_t uiCount;
        uint16_t usFlags; // a 16-bit field followed by padding

        // TODO(cosideci): impl range based loop
#if 0
        struct Iterator 
        {
            Iterator(Stack* apEntry) : m_pEntry(apEntry) {}
            Iterator operator++() { m_pEntry = m_pEntry->spNextStack; return *this; }
            bool operator!=(const Iterator& acRhs) const { return m_pEntry != acRhs.m_pEntry; }
            T* operator*() const { return m_pEntry->data; }
        private:
            Stack* m_pEntry;
        };

        Iterator begin() 
        {
            return Iterator(&entry);
        }

        Iterator end()
        {
            return Iterator(nullptr);
        }
#endif
    };

    TESBoundObject* pObject;
    Stack* spStackData;
};

static_assert(sizeof(BGSInventoryItem) == 0x10);
