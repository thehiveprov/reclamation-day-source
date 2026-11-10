#pragma once

#include <Forms/TESForm.h>
#include <Misc/BSFixedString.h>

// Layout from CommonLibF4 (1.10.163)
struct TESIdleForm : TESForm
{
    static constexpr FormType Type = FormType::Idle;

    uint8_t conditions[0x8]; // TESCondition
    // IDLE_DATA
    int8_t loopMin;
    int8_t loopMax;
    int8_t idleFlags;
    uint8_t pad2B;
    uint16_t replayDelay;
    uint8_t pad2E[0x2];
    void* childIdles;
    TESIdleForm* parentIdle;
    TESIdleForm* prevIdle;
    BSFixedString behaviorGraphName;
    BSFixedString animEventName;
    // the animation a dynamic idle (event "dyn_Activation") plays, relative to Meshes
    BSFixedString animFileName;
    uint8_t formEditorID[0x10]; // BSStringT<char>
};

static_assert(offsetof(TESIdleForm, loopMin) == 0x28);
static_assert(offsetof(TESIdleForm, childIdles) == 0x30);
static_assert(offsetof(TESIdleForm, behaviorGraphName) == 0x48);
static_assert(offsetof(TESIdleForm, animEventName) == 0x50);
static_assert(offsetof(TESIdleForm, animFileName) == 0x58);
static_assert(sizeof(TESIdleForm) == 0x70);
