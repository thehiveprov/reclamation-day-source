#pragma once

#include <Games/Primitives.h>

struct TESObjectREFR;
struct TESForm;

enum class ExtraDataType : uint8_t
{
    None = 0,
    Teleport = 0x2B,  // ExtraTeleport: a load door's DoorTeleportData* at 0x18 (the linked door's handle at 0x08)
    MapMarker = 0x2C, // ExtraMapMarker: MapMarkerData* at 0x18 (TESFullName, then the flags at 0x10)
    LeveledCreature = 0x2D,
    ObjectInstance = 0x35, // BGSObjectInstanceExtra, the object mods (OMODs) attached to a weapon/armor instance
    Faction = 0x5B,
    AliasInstanceArray = 0x88, // the reference/item fills a quest alias (quest items carry this)
    TextDisplayData = 0x99,    // ExtraTextDisplayData: the item's display name (BSFixedString at 0x18; a player's own name
                               // when bit 0 of the flags at 0x10 is set)
    Soul = 0x9C,
    BendableSplineParams = 0xB4, // a wire's shape: slack, thickness (float at 0x1C)
    InstanceData = 0xBA,         // ExtraInstanceData: what an item's mods make of it (its TBO_InstanceData* at 0x20)
    Workshop = 0xBF,            // on a workbench: Workshop::ExtraData, the settlement's power grids
    PowerLinks = 0xD0,           // ExtraPowerLinks: BSTArray<{formID, linkType}> at 0x18; a wire's has its two ends
};

struct BSExtraData
{
    inline static constexpr auto eExtraData = ExtraDataType::None;

    virtual ~BSExtraData() = 0;
    // TODO: this is different for fallout 4 it seems
    virtual ExtraDataType GetType() const noexcept = 0;

    BSExtraData* next{};
    uint16_t usFlags;
    ExtraDataType type;
};

static_assert(offsetof(BSExtraData, type) == 0x12);
static_assert(sizeof(BSExtraData) == 0x18);
