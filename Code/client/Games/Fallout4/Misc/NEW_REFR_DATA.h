#pragma once

struct TESObjectCELL;
struct TESWorldSpace;
struct TESObjectREFR;
struct TESForm;
struct BGSObjectInstanceExtra;

struct NEW_REFR_DATA
{
    NEW_REFR_DATA();

    void** vtbl;                // 0
    NiPoint3 postion;           // 8
    NiPoint3 rotation;          // 14
    TESForm* baseForm;          // 20
    TESObjectCELL* parentCell;  // 28
    TESWorldSpace* worldspace;  // 30
    TESObjectREFR* refrToPlace; // 38
    uint32_t unk40[4];          // 40
    uint32_t unk50[4];          // 50
    // the mods the reference is made with: TESDataHandler::CreateReferenceAtLocation (CommonLibF4 500304) adds it to the
    // new reference's extra data (787248, which then owns it) before its 3D is built (read 2026-10-25)
    BGSObjectInstanceExtra* modExtra; // 60
    uint32_t unk68;             // 68 set to 0x1000000
    uint16_t randomFlag;        // 6C  set to 1
};

static_assert(sizeof(NEW_REFR_DATA) == 0x70);
static_assert(offsetof(NEW_REFR_DATA, rotation) == 0x14);
static_assert(offsetof(NEW_REFR_DATA, randomFlag) == 0x6C);
static_assert(offsetof(NEW_REFR_DATA, modExtra) == 0x60);
