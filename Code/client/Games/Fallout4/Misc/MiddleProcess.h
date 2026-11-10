#pragma once

struct bhkCharacterController;

// MiddleHighProcessData
struct MiddleProcess
{
    uint8_t unk0[0xC0];
    float direction; // C0
    uint8_t unkC4[0x3E0 - 0xC4];
    // NiPointer; CommonLibF4 (1.10.163) charController at 0x3E0, 0x3D0 is currentPackageSpell
    bhkCharacterController* charController;
};

static_assert(offsetof(MiddleProcess, direction) == 0xC0);
static_assert(offsetof(MiddleProcess, charController) == 0x3E0);
