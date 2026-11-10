Scriptname ReclamationDay:Factions Hidden
{Reclamation Day's faction reactivity (the client's FactionService.cpp, docs/FACTIONS.md): rules the game lacks, done
as the game does its own. A Minuteman who joined the Gunners or the raiders is abandoned by the Minutemen the way
BoSKickOutScript and RRKickOut put a player out of the Brotherhood and the Railroad: out of the faction, the faction
the player's enemy (SetPlayerEnemy, and both ways with PlayerFaction), then MQ00Script.MQFactionKickoutPlayer (2, the
Minutemen), which tells the faction scripts (MinutemenCentralScript's MQFactionKickOut). MQ00Script is called by name,
so this compiles without its source.
Faction quests together: StartShared starts a party member's radiant quest here with the game's own Quest.Start, its
aliases set to that member's fills by the client beforehand.}

Faction Function Fallout4Faction(int aiLocalID) global
	return Game.GetFormFromFile(aiLocalID, "Fallout4.esm") as Faction
EndFunction

Function AbandonMinutemen() global
	Actor player = Game.GetPlayer()
	Faction minutemen = Fallout4Faction(0x068043) ; MinutemenFaction
	Faction playerFaction = Fallout4Faction(0x01C21C) ; PlayerFaction
	if !player || !minutemen || !playerFaction
		Debug.Trace("[RD] Factions: the Minutemen or PlayerFaction not found, nobody abandoned", 1)
		return
	endif

	player.RemoveFromFaction(minutemen)
	minutemen.SetPlayerEnemy(true)
	minutemen.SetEnemy(playerFaction, false, false)

	Quest mq00 = Game.GetFormFromFile(0x0C9B0B, "Fallout4.esm") as Quest ; MQ00
	ScriptObject kickOut = None
	if mq00
		kickOut = mq00.CastAs("MQ00Script")
	endif
	if !kickOut
		Debug.Trace("[RD] Factions: MQ00Script not found, the Minutemen's scripts weren't told", 1)
		return
	endif
	Var[] args = new Var[1]
	args[0] = 2
	kickOut.CallFunction("MQFactionKickoutPlayer", args)
EndFunction

Function StartShared(int aiQuestFormID) global
	Quest shared = Game.GetForm(aiQuestFormID) as Quest
	if !shared
		Debug.Trace("[RD] Factions: shared quest " + aiQuestFormID + " not found", 1)
		return
	endif
	if shared.IsRunning()
		return
	endif
	if !shared.Start()
		Debug.Trace("[RD] Factions: shared quest " + aiQuestFormID + " didn't start here", 1)
	endif
EndFunction
