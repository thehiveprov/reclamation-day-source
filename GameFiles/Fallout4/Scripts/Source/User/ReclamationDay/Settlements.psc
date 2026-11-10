Scriptname ReclamationDay:Settlements Hidden
{Reclamation Day's host-run settlement day (the client's WorkshopDays.cpp): while a host is in the world, only the
host's game runs the settlements' daily update, and the other games hold it with the game's own deferral. Every
workbench's daily timer (WorkshopScript.OnTimerGameTime) waits 0.2 to 1 game hour and looks again while
WorkshopParent.DailyUpdateInProgress is set, so the day never runs there: no settlers, production or attack rolls of
their own. The client sets it again every half second (a build-mode update, DailyUpdate(false), clears it at its end)
and clears it once when the hold ends (offline, or the host left). The property is set by name, so this compiles
without the workshop scripts' sources. docs/SYNC_PASS.md, gap 7}

; WorkshopParent (Fallout4.esm 0002058E), its WorkshopParentScript
ScriptObject Function WorkshopParent() global
	Quest parent = Game.GetFormFromFile(0x0002058E, "Fallout4.esm") as Quest
	if !parent
		return None
	endif
	return parent.CastAs("WorkshopParentScript")
EndFunction

Function Hold(bool abHold) global
	ScriptObject parent = WorkshopParent()
	if !parent
		Debug.Trace("[RD] Settlements: WorkshopParentScript not found, the day isn't held", 1)
		return
	endif
	parent.SetPropertyValue("DailyUpdateInProgress", abHold)
EndFunction
