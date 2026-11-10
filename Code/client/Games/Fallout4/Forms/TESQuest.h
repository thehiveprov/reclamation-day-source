#pragma once

#include <Games/Primitives.h>
#include <Forms/BGSStoryManagerTree.h>
#include <Misc/BSString.h>
#include <Misc/BSFixedString.h>
#include <Components/TESFullName.h>

// A quest alias (BGSRefAlias for a reference): its name at 0x08, its ID in the quest at 0x18
struct BGSBaseAlias
{
    virtual ~BGSBaseAlias();

    BSFixedString name;   // 0x08
    char pad10[0x8];      // 0x10
    uint32_t id;          // 0x18
};
static_assert(offsetof(BGSBaseAlias, name) == 0x8);
static_assert(offsetof(BGSBaseAlias, id) == 0x18);

struct TESQuest : BGSStoryManagerTreeForm
{
    enum Flags : uint16_t
    {
        Disabled,
        Enabled = 1 << 0, // running
        Completed = 1 << 1,
        Failed = 1 << 6,
        StageWait = 1 << 7,
        Unk = 2 << 10, // this is very likely a combination
        Unk2 = 8 << 10,
    };

    enum class Type : uint8_t
    {
        None = 0x0,
        QTYPE_MAINQUEST = 0x1,
        QTYPE_BROTHERHOOD = 0x2,
        QTYPE_INSTITUTE = 0x3,
        QTYPE_MINUTEMEN = 0x4,
        QTYPE_RAILROAD = 0x5,
        Miscellaneous = 0x6,
        QTYPE_SIDEQUESTS = 0x7,
        QTYPE_DLC01 = 0x8,
        QTYPE_DLC02 = 0x9,
        QTYPE_DLC03 = 0xA,
        QTYPE_COUNT = 0xB,
    };

    struct Objective
    {
        BSFixedString nameRef;
        TESQuest* parent;
        char pad10[0x8];  // 0x0010
        uint16_t flags;   // 0x0018
        char pad1A[0x2];  // 0x001A
        uint16_t stageId; // 0x001C
    };

    // TESQuestStage, laid out as the engine's IsStageDone (0x1405d7370, behind Quest.IsStageDone) reads it
    struct Stage
    {
        char pad0[0x18];
        uint16_t stageIndex; // 0x18
        uint8_t flags;       // 0x1A

        inline bool IsDone() const { return flags & 1; }
    };

    TESFullName fullName;             // 0x0028
    char pad38[0x38];                 // 0x0038
    GameArray<BGSBaseAlias*> aliases; // 0x0070, by index in the quest (alandtse/CommonLibF4)
    char pad88[0x6C];                 // 0x0088
    uint16_t flags;                   // 0x00F4
    uint8_t priority;                 // 0x00F6
    Type type;                        // 0x00F7
    int32_t scopedStatus;             // 0x00F8
    char padFC[0x4];                  // 0x00FC
    GameArray<Stage*> stages;         // 0x0100
    GameArray<Objective*> objectives; // 0x0118
    char pad130[0x184];              // 0x0130
    uint16_t currentStage;           // 0x02B4
    uint16_t pad2B6;                 // 0x02B6
    BSString idName;                 // 0x02B8
    char pad2C4[0x2C - 8];

    void SetActive(bool toggle);
    bool SetStage(uint16_t stage);
    void SetStopped();
    void ScriptSetStage(uint16_t stage);

    inline bool IsActive() const { return ((flags >> 11) & 1); }
    inline bool IsCompleted() const { return ((flags >> 1) & 1); }
    inline bool IsStopped() const { return !(flags & Flags::Enabled) && flags >= 0; }
    inline bool IsRunning() const { return flags & Flags::Enabled; }

    [[nodiscard]] bool IsStageDone(uint16_t aStageIndex) const noexcept;
    // The stages done so far, ascending
    [[nodiscard]] Vector<uint16_t> GetDoneStages() const noexcept;
    // Its name in the Pip-Boy, empty if it has none
    [[nodiscard]] const char* GetName() const noexcept;

    bool EnsureQuestStarted(bool& succeded, bool force);
};

static_assert(sizeof(TESQuest) == 752);
static_assert(offsetof(TESQuest, fullName) == 0x28);
static_assert(offsetof(TESQuest, stages) == 0x100);
static_assert(offsetof(TESQuest, objectives) == 0x118);
static_assert(offsetof(TESQuest, aliases) == 0x70);
static_assert(offsetof(TESQuest, currentStage) == 0x2B4);
static_assert(offsetof(TESQuest, flags) == 0xF4);
static_assert(offsetof(TESQuest, idName) == 0x2B8);
