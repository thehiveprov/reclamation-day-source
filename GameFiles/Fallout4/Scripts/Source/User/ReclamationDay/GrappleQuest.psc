Scriptname ReclamationDay:GrappleQuest extends Quest
{Reclamation Day's grappling gun: RD_GrapplingGun.esp's RD_GrappleQuest, "The Wreck of the Germination" (Tools/
GrappleQuest.js, docs/GRAPPLE.md). A wanderer from Beatsville at the burning barrel by Diamond City's gate sends the
player to the wreck of the USS Germination off Long Wharf, where a crate on its deck holds a grappling gun, Dr. Yarrow's
last log and a lab coat. Taking the gun shows the way back; back at the wanderer, their line sets stage 30 and they hand
over the gun's plans: RD_GrappleBlueprint becomes 1 and the gun's, the spikes' and the chambers' recipes show. In Winter
of Atom (its prequel plugin loaded) the quest stops at once and the wreck and the wanderer stay disabled: there the
Germination still floats, and A New Eden gives the blueprint.}

string Plugin = "RD_GrapplingGun.esp"
string PrequelPlugin = "RD_WoA_Winter.esp"

; RD_GrapplingGun.esp, Tools/GrappleQuest.js
int ID_Blueprint = 0x805
int ID_BlueprintMessage = 0x817
int ID_EnableMarker = 0x860
int ID_Gun = 0x866

int StageTaken = 10
int StageFound = 20
int StageDone = 30
int ObjectiveWreck = 10
int ObjectiveReturn = 20

Event OnQuestInit()
    if Game.IsPluginInstalled(PrequelPlugin)
        Stop()
        return
    endif
    ; the wanderer, the wreck and the crate with what's on it hang from one disabled marker
    ObjectReference marker = Game.GetFormFromFile(ID_EnableMarker, Plugin) as ObjectReference
    if marker
        marker.Enable()
    endif
    ObjectReference gun = Game.GetFormFromFile(ID_Gun, Plugin) as ObjectReference
    if gun
        RegisterForRemoteEvent(gun, "OnContainerChanged")
    endif
EndEvent

; The gun off the crate and into the player's inventory, with or without the wanderer's job
Event ObjectReference.OnContainerChanged(ObjectReference akSender, ObjectReference akNewContainer, ObjectReference akOldContainer)
    if akNewContainer == Game.GetPlayer() && !GetStageDone(StageFound)
        SetStage(StageFound)
    endif
EndEvent

Event OnStageSet(int auiStageID, int auiItemID)
    if auiStageID == StageTaken
        if !GetStageDone(StageFound)
            SetObjectiveDisplayed(ObjectiveWreck)
        endif
    elseif auiStageID == StageFound
        if IsObjectiveDisplayed(ObjectiveWreck)
            SetObjectiveCompleted(ObjectiveWreck)
        endif
        SetObjectiveDisplayed(ObjectiveReturn)
    elseif auiStageID == StageDone
        SetObjectiveCompleted(ObjectiveReturn)
        GlobalVariable blueprint = Game.GetFormFromFile(ID_Blueprint, Plugin) as GlobalVariable
        if blueprint
            blueprint.SetValue(1.0)
        endif
        Message learned = Game.GetFormFromFile(ID_BlueprintMessage, Plugin) as Message
        if learned
            learned.Show()
        endif
    endif
EndEvent
