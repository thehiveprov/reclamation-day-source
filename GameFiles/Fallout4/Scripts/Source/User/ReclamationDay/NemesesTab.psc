Scriptname ReclamationDay:NemesesTab extends Quest
{Reclamation Day's nemeses (RD_Nemeses.esp's RD_NemesesTab quest, start game enabled; built by Tools/BuildNemesesPlugin.js).
- The Pip-Boy's NEMESIS tab (PipboyTabs, Data\PBT\RD_NemesesTab.ini): PipboyTabs tells scripts when an entry is pressed
  (its PBT_ItemPressed external event, through F4SE); pressing the first one, OPEN FULL SCREEN, sets OpenScreen to 1.
  Reclamation Day's client sees it, opens the full nemesis screen and puts it back to 0.
- Every player's Interrogate perk (its choice on a captive, ReclamationDay:InterrogatePerk).
- A man's hands, as the game's own holdup (holdupEffectScript) raises and lowers them; Reclamation Day's client calls
  these with his form id when he gives up, is let go, or is left.}

GlobalVariable Property OpenScreen Auto Const Mandatory
{RD_NemesisOpenScreen}
Message Property FirstPage Auto Const Mandatory
{RD_NemesisPage00: the OPEN FULL SCREEN entry}
Keyword Property Captive Auto Const Mandatory
{RD_NemesisCaptive: Interrogate shows on a man with it}
Perk Property InterrogatePerk Auto Const Mandatory
{RD_NemesisInterrogate}
Keyword Property AnimFlavorHandsUp Auto Const Mandatory
{Fallout4.esm's: the anim flavor that raises a man's arms}
ActorValue Property Assistance Auto Const Mandatory

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
    If !Game.GetPlayer().HasPerk(InterrogatePerk)
        Game.GetPlayer().AddPerk(InterrogatePerk)
    EndIf
EndFunction

Function OnItemPressed(string asTab, int aiFormId)
    If aiFormId == FirstPage.GetFormID()
        OpenScreen.SetValue(1.0)
    EndIf
EndFunction

; The game's holdup (Fallout4.esm's holdupquest): its victims' collection alias (id 2) gives them HoldupPackage, which
; keeps a man where he stands while his HoldupAV is 1. holdupEffectScript puts its victims there the same way; without
; it a man's own package walks him off, hands down.
RefCollectionAlias Function HoldupVictims()
    Quest holdup = Game.GetForm(0x000E7725) as Quest
    If holdup
        Return holdup.GetAlias(2) as RefCollectionAlias
    EndIf
    Return None
EndFunction

ActorValue Function HoldupAV()
    Return Game.GetForm(0x000E7726) as ActorValue
EndFunction

Function HoldStill(Actor man, bool abHold)
    RefCollectionAlias victims = HoldupVictims()
    If abHold
        man.SetValue(HoldupAV(), 1)
        If victims
            victims.AddRef(man)
        EndIf
    Else
        man.SetValue(HoldupAV(), 0)
        If victims
            victims.RemoveRef(man)
        EndIf
    EndIf
EndFunction

; He gives up: out of combat, unaggressive, held where he stands, hands up (holdupEffectScript's OnEffectStart,
; without its roll)
Function HandsUp(int aiFormId)
    Actor man = Game.GetForm(aiFormId) as Actor
    If man == None || man.IsDead()
        Return
    EndIf
    man.AddKeyword(Captive)
    HoldStill(man, True)
    man.SetValue(Game.GetAggressionAV(), 0)
    man.SetValue(Assistance, 0)
    man.EnableAI(False)
    man.StopCombat()
    man.EnableAI()
    man.ChangeAnimFlavor(AnimFlavorHandsUp)
    man.EvaluatePackage()
EndFunction

; Held up by the game's own Intimidation perk: his hands are already up, only Interrogate is added
Function MarkCaptive(int aiFormId)
    Actor man = Game.GetForm(aiFormId) as Actor
    If man
        man.AddKeyword(Captive)
    EndIf
EndFunction

; Nobody questioned him: hands down, back as he was
Function HandsDown(int aiFormId)
    Actor man = Game.GetForm(aiFormId) as Actor
    If man == None
        Return
    EndIf
    man.RemoveKeyword(Captive)
    HoldStill(man, False)
    man.ChangeAnimFlavor()
    man.SetValue(Game.GetAggressionAV(), man.GetBaseValue(Game.GetAggressionAV()))
    man.SetValue(Game.GetConfidenceAV(), man.GetBaseValue(Game.GetConfidenceAV()))
    man.SetValue(Assistance, man.GetBaseValue(Assistance))
    man.EvaluatePackage()
EndFunction

; Let go: hands down. He runs (cowardly and unaggressive, so in combat he only flees), or turns on the player again
Function LetGo(int aiFormId, bool abFights)
    Actor man = Game.GetForm(aiFormId) as Actor
    If man == None || man.IsDead()
        Return
    EndIf
    man.RemoveKeyword(Captive)
    HoldStill(man, False)
    man.ChangeAnimFlavor()
    man.SetValue(Assistance, man.GetBaseValue(Assistance))
    If abFights
        man.SetValue(Game.GetAggressionAV(), man.GetBaseValue(Game.GetAggressionAV()))
        man.SetValue(Game.GetConfidenceAV(), man.GetBaseValue(Game.GetConfidenceAV()))
    Else
        man.SetValue(Game.GetAggressionAV(), 0)
        man.SetValue(Game.GetConfidenceAV(), 0)
    EndIf
    man.EvaluatePackage()
    man.StartCombat(Game.GetPlayer())
EndFunction
