#include <HivemindPCH.h>

#include <Forms/TESQuest.h>

#include <Services/PapyrusService.h>

void TESQuest::SetActive(bool toggle)
{
    // bethesda implemented a new event dispatcher
    // whenever a quest is activated, that's why
    // we use the game function instead of reimplementing it.
    using TSetActive = void(TESQuest*, bool);
    POINTER_FALLOUT4(TSetActive, SetActive, 1567563);

    SetActive(this, toggle);
}

void TESQuest::SetStopped()
{
    flags &= 0xFFFE;
    MarkChanged(2);
}

bool TESQuest::EnsureQuestStarted(bool& success, bool force)
{
    TP_THIS_FUNCTION(TSetRunning, bool, TESQuest, bool*, bool);
    POINTER_FALLOUT4(TSetRunning, SetRunning, 966434);
    return SetRunning(this, &success, force);
}

bool TESQuest::SetStage(uint16_t stage)
{
    using TSetStage = bool(TESQuest*, uint16_t);
    POINTER_FALLOUT4(TSetStage, SetStage, 952800);

    return SetStage(this, stage);
}

bool TESQuest::IsStageDone(uint16_t aStageIndex) const noexcept
{
    for (uint32_t i = 0; i < stages.length; ++i)
    {
        const Stage* pStage = stages.data[i];
        if (pStage && pStage->stageIndex == aStageIndex)
            return pStage->IsDone();
    }

    return false;
}

Vector<uint16_t> TESQuest::GetDoneStages() const noexcept
{
    Vector<uint16_t> done;
    for (uint32_t i = 0; i < stages.length; ++i)
    {
        const Stage* pStage = stages.data[i];
        if (pStage && pStage->IsDone())
            done.push_back(pStage->stageIndex);
    }

    std::sort(done.begin(), done.end());
    return done;
}

const char* TESQuest::GetName() const noexcept
{
    const char* pName = fullName.value.AsAscii();
    return pName ? pName : "";
}

void TESQuest::ScriptSetStage(uint16_t stageIndex)
{
    // At it already too: its events would run twice (upstream fix #805)
    if (currentStage == stageIndex || IsStageDone(stageIndex))
        return;

    using Quest = TESQuest;
    PAPYRUS_FUNCTION(void, Quest, SetCurrentStageID, int);
    s_pSetCurrentStageID(this, stageIndex);
}
