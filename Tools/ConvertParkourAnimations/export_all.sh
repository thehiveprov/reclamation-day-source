#!/bin/sh
# Exports every parkour clip, both views: retarget -> finalize -> contact sheet. Prints one line per clip.
# Vault and steps are exported twice: the second time cut EXIT_AFTER seconds after the travel has settled (the vault:
# and the body has landed past the obstacle; a step's feet end at their highest, so only the travel counts), so the game's own transition takes the body back to running or standing from there (one continuous
# motion, no settling into a stand first).
# Usage: export_all.sh <skyparkour archive folder> <fo4 game files folder (Meshes/Actors/Character/...)> <out folder>
set -e
SRC="$1"; FO4="$2"; OUT="$3"
HERE="$(cd "$(dirname "$0")" && pwd)"
PY="/c/Program Files/Blender Foundation/Blender 4.4/4.4/python/bin/python.exe"
NIFLY="$APPDATA/Blender Foundation/Blender/4.4/scripts/addons/io_scene_nifly"
export PYTHONPATH="$NIFLY/hkx;$NIFLY;$HERE"
mkdir -p "$OUT/3p" "$OUT/1p"
EXIT_AFTER=0.15

# Skyrim clip -> ledge height for the sheet (the clip's own end height). LandRoll is the landing roll (the original's
# slide files; it rolls in place, the character's own momentum carries it)
for clip in ClimbLow:113 ClimbMedium:152 ClimbHigh:199 ClimbHighest:249 Grab:74 GrabHigh:113 Fail:0 \
            StepLowLeft:50 StepLowRight:50 StepHighLeft:70 StepHighRight:70 Vault:19 LandRoll:0; do
    name=${clip%%:*}; ledge=${clip##*:}
    case $name in Vault|Step*) trim=0.0667 ;; *) trim=0 ;; esac
    for view in 3p 1p; do
        if [ $view = 3p ]; then
            in="$SRC/meshes/actors/character/animations/SkyParkour/SkyParkour_$name.hkx"
            skel="$FO4/Meshes/Actors/Character/CharacterAssets/skeleton.hkx"; fp=""
        else
            in="$SRC/meshes/actors/character/_1stperson/animations/SkyParkour/SkyParkour_${name}_1.hkx"
            skel="$FO4/Meshes/Actors/Character/_1stPerson/CharacterAssets/skeleton.hkx"; fp="--first-person"
        fi
        if [ $name = LandRoll ]; then
            case $view in 3p) in="$SRC/meshes/actors/character/animations/SkyParkour_Slide/LandRoll.hkx" ;;
                          *) in="$SRC/meshes/actors/character/_1stperson/animations/SkyParkour_Slide/LandRoll.hkx" ;; esac
        fi
        o="$OUT/$view/$name"
        "$PY" "$HERE/retarget.py" "$in" "$o.hkx" "$skel" "$HERE/map_3p.json" $fp --trim $trim
        "$PY" "$HERE/finalize.py" "$o.hkx" "$in" "$skel" "$o.json" --trim $trim $fp > /dev/null
        case $name in Vault|Step*)
            case $name in Vault) keys="i['landing'], i['settled']" ;; *) keys="i['settled'], 0" ;; esac
            end=$("$PY" -c "import json; i = json.load(open(r'$o.json')); print(round(min(i['duration'], max($keys) + $EXIT_AFTER), 4))")
            "$PY" "$HERE/retarget.py" "$in" "$o.hkx" "$skel" "$HERE/map_3p.json" $fp --trim $trim --end $end
            "$PY" "$HERE/finalize.py" "$o.hkx" "$in" "$skel" "$o.json" --trim $trim $fp > /dev/null ;;
        esac
        "$PY" "$HERE/render.py" "$o.hkx" "$o.json" "$in" "$HERE/map_3p.json" "$skel" "$o.png" $fp --ledge $ledge --trim $trim
    done
done
