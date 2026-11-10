Scriptname ReclamationDay:InterrogatePerk extends Perk Hidden Const
{Reclamation Day's nemeses: the fragment of RD_Nemeses.esp's RD_NemesisInterrogate perk, every player's (the
RD_NemesesTab quest adds it). As Fallout4.esm's Intimidation perk adds "Intimidate", it adds "Interrogate" to the
crosshair's prompt on a man who gave up, down the sights or not; choosing it sets InterrogateNow. Reclamation Day's
client sees it, opens his questioning card and puts it back to 0.}

GlobalVariable Property InterrogateNow Auto Const Mandatory
{RD_NemesisInterrogateNow}

Function Fragment_Entry_00(ObjectReference akTargetRef, Actor akActor)
    InterrogateNow.SetValue(1.0)
EndFunction
