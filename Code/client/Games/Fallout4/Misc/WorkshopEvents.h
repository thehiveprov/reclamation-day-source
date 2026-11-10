#pragma once

struct TESObjectREFR;

// Fallout 4's workshop (settlement build mode) events, sent while the local player builds (CommonLibF4 RE::Workshop).
// The pointers are NiPointers in the game, held for the time of the event.
namespace Workshop
{
struct ItemPlacedEvent
{
    TESObjectREFR* pWorkshop;
    TESObjectREFR* pPlacedItem;
};
static_assert(sizeof(ItemPlacedEvent) == 0x10);

struct ItemMovedEvent
{
    TESObjectREFR* pWorkshop;
    TESObjectREFR* pMovedItem;
};
static_assert(sizeof(ItemMovedEvent) == 0x10);

// Scrapped or stored
struct ItemDestroyedEvent
{
    TESObjectREFR* pWorkshop;
    TESObjectREFR* pDestroyedItem;
};
static_assert(sizeof(ItemDestroyedEvent) == 0x10);

// The local player entering (Start) or leaving build mode at a workbench
struct WorkshopModeEvent
{
    TESObjectREFR* pWorkshop;
    bool Start;
};
static_assert(sizeof(WorkshopModeEvent) == 0x10);
} // namespace Workshop
