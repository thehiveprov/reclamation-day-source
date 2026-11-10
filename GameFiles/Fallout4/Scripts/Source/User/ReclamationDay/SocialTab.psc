Scriptname ReclamationDay:SocialTab extends Quest
{Reclamation Day's Steam friends (RD_Social.esp's RD_SocialTab quest, start game enabled; built by Tools/BuildSocialPlugin.js).
The Pip-Boy's SOCIAL tab (PipboyTabs, Data\PBT\RD_SocialTab.ini): PipboyTabs tells scripts when an entry is pressed (its
PBT_ItemPressed external event, through F4SE, with the tab's UniqueID and the entry's form id). Pressing one sets
Pressed to its number + 1 (the entries are consecutive messages from FirstPage); Reclamation Day's client acts on it
(invite, join, the social sidebar) and puts it back to 0.}

GlobalVariable Property Pressed Auto Const Mandatory
{RD_SocialPressed}
Message Property FirstPage Auto Const Mandatory
{RD_SocialPage00: the first entry}

; RD_SocialPage00-63 (SocialPipboy.h kPages)
int Property PageCount = 64 AutoReadOnly

Event OnQuestInit()
    Listen()
EndEvent

; External event registrations are made again on every load
Event Actor.OnPlayerLoadGame(Actor akSender)
    Listen()
EndEvent

Function Listen()
    RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
    RegisterForExternalEvent("PBT_ItemPressed", "OnItemPressed")
EndFunction

; (every tab's presses come here: only our entries' form ids are in range)
Function OnItemPressed(string asTab, int aiFormId)
    int index = aiFormId - FirstPage.GetFormID()
    If index >= 0 && index < PageCount
        Pressed.SetValue(index + 1)
    EndIf
EndFunction
