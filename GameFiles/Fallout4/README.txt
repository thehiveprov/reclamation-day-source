FALLOUT 4: RECLAMATION DAY - BETA (__VERSION__)
=================================================

Co-op multiplayer for Fallout 4. This is a beta: expect bugs, and please send crash reports (see the end).


WHAT YOU NEED FIRST (download these yourself)
---------------------------------------------
1. Fallout 4 on Steam with all six DLCs and the data files of the 2024 update (including the free Creation Club
   items it added), with the game itself downgraded to version 1.10.163. Reclamation Day won't start on any other
   version.
2. F4SE 0.6.23, the Script Extender for 1.10.163: https://f4se.silverlock.org (into the Fallout 4 folder).
3. Address Library for F4SE Plugins, the 1.10.163 file: https://www.nexusmods.com/fallout4/mods/47327
4. Backported Archive2 Support System: https://www.nexusmods.com/fallout4/mods/81859
5. Garden of Eden Papyrus Script Extender, the OG build for 1.10.163: https://www.nexusmods.com/fallout4/mods/74160
6. Full Body First Person: https://www.nexusmods.com/fallout4/mods/109356
7. Uneducated Shooter: https://www.nexusmods.com/fallout4/mods/56789
Mods 6 and 7 have one file for every game version. Adaptive Combat Tactics isn't needed any more: Reclamation Day has
its own combat AI plugin, RD_Combat.esp, based on it (switch Adaptive Combat Tactics' plugins off if you have them;
"Launch Reclamation Day.exe" does that itself when plugins.txt lists them). Actor Widgets isn't needed any more
either: Reclamation Day draws its own bars over characters, based on it. Remove Actor Widgets if you have it (while
it's loaded, ours stay off so nobody gets two bars). Smooth Cell Loading isn't needed any more either: Reclamation Day
smooths cell loading itself, based on it (Mod Configuration Menu, Reclamation Day, Smooth Loading). Remove Smooth Cell
Loading if you have it (while it's installed, ours stays off). Zzyxzz's Smart Selection isn't needed any more either:
Reclamation Day's Better Interact picks what you look roughly at and outlines it, based on it (Mod Configuration Menu,
Reclamation Day, Better Interact). Remove Smart Selection if you have it (while it's installed, ours stays off). VATS
Unchained isn't needed any more either: Reclamation Day has RD VATS 76', based on it (Mod Configuration Menu, Reclamation
Day, VATS 76'). Remove VATS Unchained if you have it (while it's installed, ours stays off).

The mods Reclamation Day builds on (up to 1.6.7-285 these came with it; they're downloads from their authors now):
8. Unofficial Fallout 4 Patch: https://www.nexusmods.com/fallout4/mods/4598
9. Start Me Up Redux: https://www.nexusmods.com/fallout4/mods/56984 , the "Normal - XDI Dialogue" version, with both
    StartMeUp.esp and StartMeUpShaunDefaultAppearance.esp.
10. Actor Velocity Framework: https://www.nexusmods.com/fallout4/mods/56604 (slides and dodges push you with it).
11. Extended Dialogue Interface: https://www.nexusmods.com/fallout4/mods/27216 , version 1.3.6, the one for 1.10.163
    (the dialogue menu the party votes read).
12. KARMA: https://www.nexusmods.com/fallout4/mods/80425
13. Be Exceptional - Skills and Perk Overhaul: https://www.nexusmods.com/fallout4/mods/28222 (without its uninstall
    plugin).
14. LevelUpMenuEx: https://www.nexusmods.com/fallout4/mods/28822 , the file for 1.10.163 (Be Exceptional's level up
    menu).
15. PipboyTabs: https://www.nexusmods.com/fallout4/mods/29568 , the file for 1.10.163 (KARMA's Pip-Boy tab, and
    Reclamation Day's NEMESIS and SOCIAL tabs).
If you played an earlier version, Reclamation Day already put KARMA, Be Exceptional, LevelUpMenuEx, PipboyTabs and
Extended Dialogue Interface in your Data folder, so you have those five.

The graphics, which replace ENB (with an ENB, Upscaling can't upscale):
16. NAC X - Natural and Atmospheric Commonwealth, Legacy Edition: https://www.nexusmods.com/fallout4/mods/46722
17. Vivid Fallout - All in One: https://www.nexusmods.com/fallout4/mods/25714 , the "Best Choice" file.
18. Flutter Flicker Fixer For Foliage: https://www.nexusmods.com/fallout4/mods/79853 , the "Vanilla" file.
19. This Made My Spline Stiff: https://www.nexusmods.com/fallout4/mods/98935
20. Upscaling: https://www.nexusmods.com/fallout4/mods/99130 , the "Pre-NG (version 1.10.163)" file under Update
    files.
21. Motion Vector Fixes: https://www.nexusmods.com/fallout4/mods/98544
22. High FPS Physics Fix: https://www.nexusmods.com/fallout4/mods/44798
23. Mod Configuration Menu: https://www.nexusmods.com/fallout4/mods/21497 , the "Mod Configuration Menu 1.39" file
    (for 1.10.163) under Optional files.
24. Previsibines Repair Pack: https://www.nexusmods.com/fallout4/mods/46403 , the "Full" file for 1.10.163. It rebuilds
    the game's precombined meshes and visibility data, which is most of the frame rate in Boston. Its PRP.esp must load
    last: "Launch Reclamation Day.exe" moves it to the end of the load order at every start.
You don't have to collect these by hand: the launcher's Required mods page lists them all, with a Download button for
each that opens it on Nexus Mods as a mod manager download, and ticks them off as they land. Reclamation Day's main
menu lists the ones it can't find too, with a link, and co-op won't start until they're installed.

Recommended, not required (it only changes your own picture): ReShade 6.8.0 or newer (https://reshade.me). Run its
setup, pick Fallout4.exe and "Microsoft DirectX 10/11/12", then the preset TrueSight-NAC.ini from the NAC-X file of
TrueSight reShade (https://www.nexusmods.com/fallout4/mods/98263), and let it install the effects that preset uses.
NAC X's menu (in your apparel) and the settings of Upscaling and ReShade are your own. The weather is shared: the
first player in a worldspace decides it, so picking a weather in NAC X only changes it for everyone when you're the one
deciding.


INSTALL
-------
1. Install the mods above with your mod manager (or by hand), and enable their plugins in this order:
     unofficial fallout 4 patch.esp, StartMeUpShaunDefaultAppearance.esp, StartMeUp.esp
   then the graphics' six, NAC X's three last:
     Flutter Flicker Fixer For Foliage.esp, Stiff Splines.esp, VividFallout - AiO - BestChoice.esp, NAC.esp,
     NAC-FH.esp, NAC-NW.esp
   then Reclamation Day's own (RDVehicles.esp, RD_Companions.esp, RD_Combat.esp, RD_Perks.esp and the other RD_
   plugins: "Launch Reclamation Day.exe" adds them itself), then KARMA.esm, Be Exceptional.esp and XDI.esm, then
   RD_Landscape.esp and RD_Trees.esp (the nature overhaul: the launcher puts these two and their files in Data and
   adds them itself). Last of all: PPF.esm and PRP.esp (the launcher keeps PRP.esp at the very end). The whole order
   is under LOAD ORDER below.
2. "Reclamation Day", "Game Files" and "Launch Reclamation Day.exe": keep them together, anywhere OUTSIDE the
   Fallout 4 folder.
3. Start the game with "Launch Reclamation Day.exe". For a desktop shortcut: right-click it > Send to > Desktop.
   It opens the launcher: the first time it asks which Fallout 4 to use (it lists the copies it finds) and where Game
   Files should go. Game Files has the parts of Reclamation Day your mod manager should know about (the motorcycle's
   F4SE plugin and models, and the dogs' looks): the launcher puts them into Vortex's or Mod Organizer 2's staging
   folder as one mod, "Reclamation Day - Game Files" (enable it there), or straight into "Fallout 4\Data" when you have
   no mod manager. To install them yourself, pick "I install them myself". Then it shows the required mods above, with
   what's missing (you can skip this step and come back). After that the launcher shows Play, the patch notes of every
   release, and those choices with a button to change them. Its look can be changed under Style: Pip-Boy, Brotherhood of
   Steel, Vault-Tec Bulletin or Minutemen.


LOAD ORDER
----------
Everyone on a server needs the same plugins as the host, in the same order. When yours differ, joining tells you
exactly which ones to add or remove. The load order for this beta:
__LOAD_ORDER__


PLAYING
-------
- Main menu > MULTIPLAYER. HOST GAME makes a world and runs its server on your PC. JOIN connects to a host.
- Hosting over the internet: forward UDP port 10578 to your PC (or use a VPN such as Tailscale or ZeroTier) and
  give the other players your address.
- Co-op characters and saves are kept apart from your single-player ones.
- New: crouch while sprinting to slide. The others see you slide (and stumble after a big fall).
- New: ZK's Fluid Movement. Jump at a ledge to climb it (from waist height to well over your head), at a low step
  to step up, or at a rail or a low wall while running to vault it; keep jump held in the air to grab a ledge. High
  climbs cost AP (without enough, the climb fails). Falling, press crouch just before you land to roll out of it
  (costs a little AP, softens the landing). Your weapon is out of your hands for each move. The others see and
  hear your moves.
- New: Realtime Pip-Boy Map. Press M to raise your Pip-Boy and look at a live map on it on the move (your weapon
  goes out of your hand while it's up): tap M to keep it up (M again lowers it), or hold M to look while it's held.
  Outside it shows the world map, inside the local map, turned the way you face, with your quest targets, nearby
  locations, exits and the other players. Attacking, aiming, sprinting, reloading or opening a menu lowers it; Tab
  still opens the full Pip-Boy. First person only (from third person it switches over and back). To give M back to
  the game's own map, set enabled=false in Reclamation Day\config\PipboyMap.ini.
- New: actor bars. Enemies in combat get a red bar over their head: name, level and health, a star beside
  legendaries and a skull in place of the level of enemies far above yours. Friendly characters (companions,
  settlers, Dogmeat) and the other players (with their player name and level) get one in your HUD color while in a
  fight. Hidden behind walls. Change all of it in the pause menu: Mod Config > Reclamation Day > Actor Bars (always shown, only the
  one you aim at, Action Points, distance, size, ...).
- Weather is shared like in Fallout 76: everyone outside in the same world (the Commonwealth, Far Harbor, ...) has
  the same weather, the one of whoever joined first (the host, in a hosted world).
- VATS doesn't slow time down, like in Fallout 76: RD VATS 76' locks on to the enemy nearest your crosshair and the game
  keeps running at full speed, so everyone's game keeps the same pace. Aim at a body part to pick it; your shots roll its
  hit chance and spend Action Points. Change it in the pause menu: Mod Config > Reclamation Day > VATS 76'.
- New: motorcycles. Build yours at a power armor station with no power armor in or near it (and none on you): its
  menu has the Motorcycle (steel 30, aluminum 10, rubber 10, screws 8, gears 6, springs 4, oil 5, leather 6, copper 4,
  adhesive 4, circuitry 2, glass 2), and once you have one, Bring My Motorcycle Here. One per player.
    B get on (within a few steps of your bike; your weapon is put away first), E get off (climbs off when stopped,
      jumps off while moving), W throttle, S brake and reverse, A and D steer, Left Shift handbrake (slides the
      tail out), Left Alt tuck (a little faster, steers less), hold and let go of Space to hop, W and S lean you
      forward and back in the air. The game's Toggle POV key (or the mouse wheel) switches between your own eyes
      and the camera behind. Crashing into something fast throws you off; you get up by yourself.
  Parked, it stays where you left it; its saddlebags are storage of their own. Deep water ends the ride. Keys and
  handling: Data\F4SE\Plugins\RDVehicles.ini (changes apply while you play). Not tried in co-op yet.
- New: Better Interact. When your crosshair is on nothing, what you're most likely looking at (an item, a container,
  a door, the bike's seat or saddlebags) is picked for you and outlined in your HUD color. Your left hand also goes out
  when you pick something up (what you took in it), press a button or push a door, without ever slowing you down, and
  other players see it. Settings: Mod Config > Reclamation Day > Better Interact.
- New: the Analytical Eye perk. Once your Intelligence reaches 6 (points, bobbleheads and books count, chems don't)
  you get it by yourself, and what Better Interact picks glows the way VATS shows its targets instead of the outline.
- New: Karma (KARMA by bp42s). Your actions earn and lose karma, with titles in a KARMA tab of your Pip-Boy, bounty
  hunters at Very Good or Very Evil karma, karma legendary effects and reactions; settings in Mod Config > KARMA.
  Each player has their own karma.
- New: skills (Be Exceptional by Rcollpc). Each level gives skill points (10 plus your Intelligence) for 16 skills
  (Barter, Lockpick, Rifles, Science, ...), spent in the level up menu (LevelUpMenuEx), which also has Be
  Exceptional's reworked perks in place of the vanilla chart. Each player has their own skills.
- New: dogs to adopt. Seven dogs wait around the Commonwealth, each with its own look: outside the Diamond City
  gate, in Salem by the Museum of Witchcraft, at Bunker Hill, the C.I.T. ruins, the Castle's courtyard,
  Goodneighbor's plaza and outside the Natick police station (Dogmeat is still at Red Rocket). Press E on one nobody
  has adopted to adopt it and give it a name; it's your own Dogmeat (follow, stay, Find stuff, tricks, dog armor)
  and travels beside your human companion. Talking to another dog never commands yours. "Go home" sends it to the
  settlement you pick. One dog per player: adopting another sends the first one home. In co-op the others don't see
  your dog's name or that it's yours yet.
- New: emotes. Hold G for the emote wheel, move the mouse toward one and let go of G to play it (or click it). Each
  is a chat command too: /salute, /bossalute, /point, /pointleft, /pointright, /pointback, /yes, /no, /clap, /cheer,
  /boo, /think, /lookaround, /offer, /victory, and the poses /sit, /kneel, /pray and /surrender, which hold until you
  press a movement key; /emotes lists them. Your weapons are put away meanwhile, and in first person the view goes to
  third person and back. Everyone near you sees them. The key and the wheel's eight: Reclamation Day\config\Emotes.ini.
- New: party votes in conversations. When you talk to someone with party members within 30 m of you, your options go
  to a vote: your pick in the dialogue is your vote, theirs is on a card (1-9 or a click). After 30 seconds, or once
  everyone has voted, the most votes win (a tie: the host's vote, then yours) and your character says it. Alone, you
  pick as always. The votes use Extended Dialogue Interface (XDI by reg2k, a required download): its dialogue menu shows each
  option's full line, and quests made for it can offer more than four options.
- New: a greener Commonwealth (Boston Natural Surroundings by Silverfox600, remade). New grass, ground and plants
  across the Commonwealth (RD_Landscape.esp) and about 22,800 new trees, with forests you see from far away
  (RD_Trees.esp: maples, elms, red pines, firs and aspens). Both are off in Winter of Atom, whose Commonwealth is
  under snow: "Launch Reclamation Day.exe" switches them for the campaign you play.


QUESTS (like Fallout 76)
------------------------
- Your quests are your own: what you do in a quest doesn't change anyone else's.
- When a new quest shows up in your Pip-Boy, a pop-up offers to do it together: "Invite party" (or everyone, if
  you're not in a party). Later, or to pick who: F2 > Player manager > Quests > Invite. They get a pop-up to accept
  or decline. Everyone on it shares its progress from then on; who joins catches up to where the team is.
- Share main quests (F2 > Player manager > Party Menu, the party leader's switch): the whole party shares the main
  quest without invites. Everyone catches up to whoever is furthest, and every step anyone takes counts for all.
  Catching up runs each step's events, rewards included.
- Who already finished the quest (or took another path through it) joins as a helper: they fight alongside you and
  their own progress stays as it is.
- Quest items: everyone has their own copy, except on a quest you do together, where whoever picks it up has it.
- Leave a team in the same place. Teams end when the server stops.


BUILDING (settlements are shared)
--------------------------------
- Claim a settlement to build there: using a workbench nobody has claimed asks "Claim ...?". Whoever claims it first
  leads it. Nobody builds at a settlement nobody claimed. The leader gives the players of their party the right to
  build (place, move) and the right to scrap (scrap, store) there, in the overlay: F2 > Player manager >
  Settlements. Rights stay with the player (by name) while the leader is away, until the leader takes them back.
  Without rights, someone else's workbench says whose settlement it is and closes. The leader can abandon a
  settlement there so someone else can claim it, and the host can release anyone's. Claims are kept with the world
  (settlement-claims.txt, next to settlements.txt). Two players with the same name would share their rights. Claiming
  doesn't change how the settlement runs.
- Your C.A.M.P. (like Fallout 76's): everyone carries a C.A.M.P. (Pip-Boy > Aid, it isn't used up). Using it outside
  sets your C.A.M.P. down in front of you and opens build mode at it: build within about 34 m of it. One per player;
  not inside settlements, near marked locations or near another C.A.M.P. Using it again somewhere else moves your
  C.A.M.P. there with everything you built, turned the way you face (5 to 50 caps, by distance); turrets don't move
  yet. You lead your C.A.M.P. like a settlement (give your party rights in Settlements); the host can remove one.
  C.A.M.P.s stay in the world for everyone (camps.txt, next to settlements.txt).
- Settlements are kept with the world: the server remembers what was built, moved and scrapped (settlements.txt in
  the hosted world's save folder, or data\ next to a dedicated server), so restarting it loses nothing, and whoever
  was away gets the changes when they join. Each player's game remembers which of its objects is which, so nothing
  gets built twice.
- What anyone builds, moves or scraps at a settlement shows up for everyone, wires and power included (wired and
  touching objects connect the same way everywhere, so what's powered is the same for everyone). Turrets too: one
  player's game runs each turret, the others see it fight, and anyone can move or scrap it in build mode.
- The settlement's budget (the size bar in build mode) counts what everyone builds and scraps, not just you
  (except turrets someone else built).
- Settlements load their scenery as separate objects (the game normally merges it into big meshes), so all of it can
  be scrapped. This costs some frame rate in settlements only. Scenery shows a name when outlined, and scrapping it
  gives what it's made of (trees wood, rubble concrete and steel, wrecks steel, rubber and glass, ...), more for bigger
  objects.
- Build tools, in build mode (all keys can be changed in the overlay: F2 > Settings > Build tools; numpad keys need
  Num Lock on):
    F1 place anywhere on/off (overlapping, floating, outside the build area)
    F4 scrap anything on/off (anything in the settlement, not just junk; dead bodies too, their items go to the
       workbench)
    Backspace undo your last change at this settlement: what you built, moved or scrapped (your last 5, one at a
       time; undoing a scrap takes back the materials it gave)
    On the outlined object:
      Insert rotation axis X/Y/Z, , and . rotate, Num - and Num + rotation step, Num 0 level it
      Num 8 2 4 6 7 9 move up, down, left, right, closer, away; Num / and Num * move step;
      Home move along the world's axes or relative to you
      Num 1 and Num 3 smaller/bigger, Page Down and Page Up scale step, Num . normal size
      Num 5 redraw
      = remember it; then on another object: [ ] \ its X, Y, height, ; its angles, ' its scale, / everything


CRASHES AND BUGS
----------------
Main menu > "Export crash reports" saves a zip to your desktop. Send it along with what you were doing.


CREDITS
-------
See "Game Files\CREDITS.txt".
