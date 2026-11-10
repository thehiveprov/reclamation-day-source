#pragma once

#include "ExtraData.h"

#include <span>

struct TESForm;

// Holds the object mods (OMODs: scopes, suppressors, receivers, armor linings, ...) attached to one item instance.
// Layout from CommonLibF4 (1.10.163).
struct BGSObjectInstanceExtra : BSExtraData
{
    inline static constexpr auto eExtraData = ExtraDataType::ObjectInstance;

    struct ObjectIndexData
    {
        uint32_t objectID; // form id of the BGSMod::Attachment::Mod
        uint8_t index;     // attach index
        uint8_t rank;
        uint8_t disabled;
        uint8_t pad7;
    };
    static_assert(sizeof(ObjectIndexData) == 0x8);

    // BSTDataBuffer<1>: the payload, followed by block descriptors at buffer + size
    struct DataBuffer
    {
        uint8_t* buffer;
        uint32_t size;
    };

    // Allocates an empty instance extra with the game's vtable, ready for AddMod().
    static BGSObjectInstanceExtra* New() noexcept;

    [[nodiscard]] std::span<const ObjectIndexData> GetIndexData() const noexcept;

    void AddMod(TESForm* apMod, uint8_t aAttachIndex, uint8_t aRank) noexcept;

    const DataBuffer* values;
    uint16_t itemIndex;
};

static_assert(offsetof(BGSObjectInstanceExtra, values) == 0x18);
static_assert(offsetof(BGSObjectInstanceExtra, itemIndex) == 0x20);
static_assert(sizeof(BGSObjectInstanceExtra) == 0x28);
