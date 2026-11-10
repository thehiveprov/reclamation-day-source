#!/bin/sh
# Converts every emote's third person clip into its first person clip and draws its contact sheet.
# The first person clip keeps the third person clip's path under ReclamationDay\Emotes (the game finds it from there).
# The Bird and Hands Up!, ours, are made first in third person (the_bird.py, hands_up.py) and go the same way.
# Usage: export_all.sh <fo4 game files folder (Meshes/Actors/Character/...)> <out Data folder> <sheets folder>
set -e
FO4="$1"; OUT="$2"; SHEETS="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
PARKOUR="$HERE/../ConvertParkourAnimations"
PY="/c/Program Files/Blender Foundation/Blender 4.4/4.4/python/bin/python.exe"
NIFLY="$APPDATA/Blender Foundation/Blender/4.4/scripts/addons/io_scene_nifly"
export PYTHONPATH="$NIFLY/hkx;$NIFLY;$(cygpath -w "$PARKOUR");$(cygpath -w "$HERE")"
SKEL3="$FO4/Meshes/Actors/Character/CharacterAssets/skeleton.hkx"
SKEL1="$FO4/Meshes/Actors/Character/_1stPerson/CharacterAssets/skeleton.hkx"
mkdir -p "$SHEETS"
for clip in MT/Neutral/PoseA_Salute1 MT/Neutral/PoseA_BOSSalute1 MT/Neutral/PoseA_PointForward1 MT/Neutral/PoseA_PointLeft1 \
            MT/Neutral/PoseA_PointRight1 MT/Neutral/PoseA_PointBack1 MT/Player/PoseA_Agree1 MT/Player/Female/PoseA_Agree1 \
            MT/Player/PoseA_Disagree1 MT/Player/Female/PoseA_Disagree1 Furniture/Clap/clap_standing \
            Quest/Cheering/Stand/Cheering1 Quest/Cheering/Stand/Female/Cheering1 Quest/Cheering/Stand/Booing1 \
            Quest/Cheering/Stand/Female/Booing1 MT/Neutral/PoseA_IdleThink1 DynamicAnims/IdleLookUpAround MT/Neutral/PoseA_Give1 \
            Quest/Fighter/Winner \
            Furniture/FloorSitRadioListen/Neutral/EnterFromStand Furniture/FloorSitRadioListen/Neutral/PoseA_Idle1 \
            Furniture/FloorSitRadioListen/Neutral/ExitToStand Furniture/KneelSit/EnterFromStand Furniture/KneelSit/PoseA_Idle1 \
            Furniture/KneelSit/ExitToStand Furniture/KneelPraying/EnterFromStand Furniture/KneelPraying/PoseA_Idle1 \
            Furniture/KneelPraying/ExitToStand Furniture/HeldHostageKneelHandsUp/EnterFromStand \
            Furniture/HeldHostageKneelHandsUp/PoseA_Idle1 Furniture/HeldHostageKneelHandsUp/ExitToStand; do
    in="$FO4/Meshes/Actors/Character/Animations/$clip.hkx"
    out="$OUT/Meshes/Actors/Character/_1stPerson/Animations/ReclamationDay/Emotes/$clip.hkx"
    "$PY" "$HERE/first_person.py" "$in" "$SKEL3" "$SKEL1" "$out"
    "$PY" "$HERE/sheet.py" "$out" "$in" "$SKEL3" "$SKEL1" "$SHEETS/$(echo "$clip" | tr / _).png" --every 9
done
# Ours: The Bird (the_bird.py, from the point forward), then its first person clip
bird3="$OUT/Meshes/Actors/Character/Animations/ReclamationDay/Emotes/TheBird.hkx"
bird1="$OUT/Meshes/Actors/Character/_1stPerson/Animations/ReclamationDay/Emotes/ReclamationDay/Emotes/TheBird.hkx"
"$PY" "$HERE/the_bird.py" "$FO4/Meshes/Actors/Character/Animations/MT/Neutral/PoseA_PointForward1.hkx" "$SKEL3" "$bird3"
"$PY" "$HERE/first_person.py" "$bird3" "$SKEL3" "$SKEL1" "$bird1"
"$PY" "$HERE/sheet.py" "$bird1" "$bird3" "$SKEL3" "$SKEL1" "$SHEETS/TheBird.png" --every 9
# Ours: Hands Up! (hands_up.py, from the point forward), then its first person clip
hands3="$OUT/Meshes/Actors/Character/Animations/ReclamationDay/Emotes/HandsUp.hkx"
hands1="$OUT/Meshes/Actors/Character/_1stPerson/Animations/ReclamationDay/Emotes/ReclamationDay/Emotes/HandsUp.hkx"
"$PY" "$HERE/hands_up.py" "$FO4/Meshes/Actors/Character/Animations/MT/Neutral/PoseA_PointForward1.hkx" "$SKEL3" "$hands3"
"$PY" "$HERE/first_person.py" "$hands3" "$SKEL3" "$SKEL1" "$hands1"
"$PY" "$HERE/sheet.py" "$hands1" "$hands3" "$SKEL3" "$SKEL1" "$SHEETS/HandsUp.png" --every 12
