#include "AnimationEventLists.h"

// The lists might not contain all relevant animation events, extend as necessary.

using TiltedPhoques::Set;

namespace
{
String ToLower(std::string_view aText) noexcept
{
    String lower(aText.begin(), aText.end());
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lower;
}

struct InstantPair
{
    std::string_view Event;
    std::string_view Instant; // empty: no instant counterpart exists
};

#if defined(TP_SKYRIM)
constexpr InstantPair kIdleToInstant[] = {
    // Idle animations that have instant counterparts
    {"HorseEnter", "HorseEnterInstant"},
    {"IdleAlchemyEnter", "IdleAlchemyEnterInstant"},
    {"IdleAltarEnter", "IdleAltarEnterInstant"},
    {"IdleBarCounterEnter", "IdleBarCounterEnterInstant"},
    {"IdleBedEnterStart", "IdleBedEnterInstant"},
    {"IdleBedLeftEnterStart", "IdleBedLeftEnterInstant"},
    {"IdleBedRightEnterStart", "IdleBedRightEnterInstant"},
    {"IdleBedRollFrontEnterStart", "IdleBedRollFrontEnterInstant"},
    {"IdleBedRollLeftEnterStart", "IdleBedRollLeftEnterInstant"},
    {"IdleBedRollRightEnterStart", "IdleBedRollRightEnterInstant"},
    {"IdleBlackSmithingEnterStart", "IdleBlackSmithEnterInstant"},
    {"IdleBlessingKneelEnter", "IdleBlessingKneelEnterInstant"},
    {"IdleBoundKneesStart", "IdleBoundKneesEnterInstant"},
    {"IdleCartTravelDriverEnter", "IdleCartTravelDriverEnterInstant"},
    {"IdleCartTravelPlayerEnter", "IdleCartTravelPlayerEnterInstant"},
    {"IdleChairCHILDFrontEnter", "IdleChairCHILDEnterInstant"},
    {"IdleChairCHILDLeftEnter", "IdleChairCHILDEnterInstant"},
    {"IdleChairCHILDRightEnter", "IdleChairCHILDEnterInstant"},
    {"IdleChairEnterStart", "IdleChairEnterInstant"},
    {"IdleChairEnterToSit", "IdleChairEnterInstant"},
    {"IdleChairFrontEnter", "IdleChairEnterInstant"},
    {"IdleChairLeftEnter", "IdleChairEnterInstant"},
    {"IdleChairRightEnter", "IdleChairEnterInstant"},
    {"IdleChildSitOnKnees", "IdleChildSitOnKneesInstant"},
    {"IdleCoffinEnter", "IdleCoffinEnterInstant"},
    {"IdleCookingPotEnter", "IdleCookingPotEnterInstant"},
    {"IdleCookingSpitEnter", "IdleCookingSpitEnterInstant"},
    {"IdleCounterStart", "IdleCounterEnterInstant"},
    {"IdleCowerEnter", "IdleCowerEnterInstant"},
    {"IdleDLC2PickaxeEnter", "IdleDLC2PickaxeEnterInstant"},
    {"IdleEnchantingEnter", "IdleEnchantingEnterInstant"},
    {"IdleExecutionerIdle", "IdleExecutionerIdleEnterInstant"},
    {"IdleGrainMillEnter", "IdleGrainMillEnterInstant"},
    {"IdleHammerTableEnter", "IdleHammerTableEnterInstant"},
    {"IdleHammerWallEnter", "IdleHammerWallEnterInstant"},
    {"IdleHideLEnter", "IdleHideLEnterInstant"},
    {"IdleHideREnter", "IdleHideREnterInstant"},
    {"IdleJarlChairEnter", "IdleJarlChairEnterInstant"},
    {"IdleLayDownEnter", "IdleLayDownEnterInstant"},
    {"IdleLeanTable", "IdleLeanTableEnterInstant"},
    {"IdleLeanTableEnter", "IdleLeanTableEnterInstant"},
    {"IdleLeverPushStart", "IdleLeverPushEnterInstant"},
    {"IdleOvenEnter", "IdleOvenEnterInstant"},
    {"IdlePickaxeEnter", "IdlePickaxeEnterInstant"},
    {"IdlePickaxeFloorEnter", "IdlePickaxeFloorEnterInstant"},
    {"IdlePickaxeTableEnter", "IdlePickaxeTableEnterInstant"},
    {"IdlePrayCrouchedEnter", "IdlePrayCrouchedEnterInstant"},
    {"IdleRailLeanEnter", "IdleRailLeanEnterInstant"},
    {"IdleSearchingChest", "IdleSearchChestEnterInstant"},
    {"IdleSearchingTable", "IdleSearchTableEnterInstant"},
    {"IdleSharpeningWheelStart", "IdleSharpeningWheelEnterInstant"},
    {"IdleSitCrossLeggedEnter", "IdleSitCrossLeggedEnterInstant"},
    {"IdleSitLedgeEnter", "IdleSitLedgeEnterInstant"},
    {"IdleSmelterEnter", "IdleSmelterEnterInstant"},
    {"IdleSnowElfPrinceChairEnter", "IdleSnowElfPrinceChairEnterInstant"},
    {"IdleStoolEnter", "IdleStoolEnterInstant"},
    {"IdleTableEnter", "IdleTableEnterInstant"},
    {"IdleTableEnterPlayer", "IdleTableEnterInstant"},
    {"IdleTanningEnter", "IdleTanningEnterInstant"},
    {"IdleWallLeanStart", "IdleWallLeanEnterInstant"},
    {"IdleWallSoldierEnter", "IdleWallSoldierEnterInstant"},
    {"IdleWoodChopStart", "IdleWoodChopEnterInstant"},
    {"IdleWoodPickUpEnter", "IdleWoodPickUpEnterInstant"},
};

constexpr std::string_view kExitSpecial[] = {
    "IdleForceDefaultState", "IdleReturnToDefault", "ReturnDefaultState", "ReturnToDefault", "ForceFurnExit", "IdleStop",
    "IdleStopInstant", "IdleBedExitToStand", "IdleChairExitToStand", "IdleFurnitureExitSlow", "IdleMesmerizeStop",
    "IdleWoodChopStop", "BlackSmithAnvilExit", "BlacksmithSharpeningWheelExit", "BleedOutEarlyExit", "CartExit",
    "CraftingEnchantingBenchExit", "DragonMountExit", "DragonMountExitOut", "GetUpExit", "HorseExit", "HorseExitOut", "LayExit",
    "NPCDragonMountExitOut", "NPCHorseExit", "NPCHorseExitOut", "StoolBackExit", "TableBackExit", "TrapExitStart",
    "VehicleExit", "WebIdleExit",
    // Not an "Exit" action per se, but makes the actor return to its initial anim state
    "GetUpBegin",
};

constexpr std::string_view kIgnore[] = {
    // Animation events with empty names don't carry anything useful besides various animvars updates and state changes
    // to the currently running animations. Ignored because they spam too much
    "", "TurnLeft", "TurnRight", "turnStop", "CyclicFreeze", "CyclicCrossBlend", "NPC_BumpedFromBack", "NPC_BumpedFromLeft",
    "NPC_BumpedFromRight", "NPC_BumpEnd", "NPC_BumpFromFront", "NPC_TurnLeft180", "NPC_TurnLeft90", "NPC_TurnRight180",
    "NPC_TurnRight90", "NPC_TurnToWalkLeft180", "NPC_TurnToWalkLeft90", "NPC_TurnToWalkRight180", "NPC_TurnToWalkRight90",
    // Jumps
    "JumpDirectionalStart", "JumpFall", "JumpFallDirectional", "JumpFastEnd", "JumpFastLeft", "JumpFastRight", "JumpForwardEnd",
    "JumpForwardStart", "JumpLand", "JumpLandDirectional", "JumpLandEnd", "JumpLandFailSafe", "JumpLandFast", "JumpLandSoft",
    "JumpSoftEnd", "JumpStandingStart",
    // Useless idles, sometimes spam
    "IdlePlayer", "IdleChairVar1", "idleChairVar2", "idleChairArmsCrossedVar1", "idleChairArmsCrossedVar2",
    // Weapon drawing is handled separately in CharacterService
    "WeapEquip", "WeapSoloEquip", "WeapForceEquip", "WeapEquip_Out", "WeapUnEquip",
    // https://github.com/tiltedphoques/TiltedEvolution/issues/810
    "Unequip", "combatStanceStop",
};
#elif defined(TP_FALLOUT)
// From the IDLE records of the masters and the human behavior graphs (RaiderRootBehavior, the furniture behaviors).
// Only root graph states: furniture's instant events (sitStartFromStandInstant) need the furniture subgraph, which only
// the furniture action (with its target) loads, so those replay their normal enter instead.
constexpr InstantPair kIdleToInstant[] = {
    {"g_defaultState", "g_defaultStateInstant"},
    {"g_archetypeBaseStateStart", "g_archetypeBaseStateStartInstant"},
    {"g_archetypeRelaxedStateStart", "g_archetypeRelaxedStateStartInstant"},
    {"g_SwimStart", "g_SwimStartInstant"},
    {"g_SwimStart_PA", "g_SwimStart_PA_Instant"},
    {"g_SwimStart_MT", "g_SwimStart_MT_Instant"},
    {"g_SwimStart_MT_PA", "g_SwimStart_MT_PA_Instant"},
    {"g_EssentialDown_MT", "g_EssentialDownInstant_MT"},
};

constexpr std::string_view kExitSpecial[] = {
    // Back to the root/default state
    "g_defaultState", "g_defaultStateInstant", "defaultState", "returnToDefault", "idleReturnToDefault", "IdleStop",
    "g_instantInitializeGraphToBaseState", "g_InitializeToBaseState", "g_returnToCore",
    // Leaving furniture, idle markers and dynamic idles
    "sitStop", "furnitureExit", "QuickExitToStand", "QuickExitToStandLeft", "QuickExitToStandRight", "ExitFront_ToStand",
    "ExitFront_ToStand_Delayed", "IdleToExit_NonCombat", "IdleToExit_Combat", "exitDynamicAnim", "exitDynamicAnimation",
    "syncIdleStop", "idleReadingStop", "idleStopEating", "pipboyIdleStop", "exitCover",
    // Getting back up, leaving the water
    "GetUpBegin", "GetUpStart", "MobilityDownGetUpStart", "UnconsciousStop", "injuredStop", "SwimStop", "MTSwimStop",
    // ...others are checked in IsExit() by their suffix
};

constexpr std::string_view kIgnore[] = {
    // Animation events with empty names only update animation variables (spam)
    "",
    // Turning on the spot
    "TurnLeft", "TurnRight", "turnStop", "TurnStart", "TurnStart_ToMove", "TurnStart_MoveToMove", "TurnStart_ToMove_OppFoot",
    "TurnStart_MoveToMove_OppFoot", "TurnLeft90", "TurnRight90", "TurnLeft180", "TurnRight180", "Turn180FromStop",
    "TurnToRun90LeftStart", "TurnToRun90RightStart", "cannedTurnLeft", "cannedTurnRight", "TurnClockwise", "TurnCounterClockwise",
    // Jumps and falls
    "JumpStart", "jumpStartFromRun", "jumpFall", "jumpMovingFall", "jumpLand", "jumpLandSoft", "jumpLandToRun", "jumpLandToWalk",
    "jumpRunLand", "jumpImpactLand", "jumpImpactLandToRun", "jumpEnd", "FallStart",
    // Weapon drawing is handled separately in CharacterService
    "WeapEquip", "weapEquip_CreateABot", "weapForceEquip", "weapForceEquip_FromLaying", "weapForceEquip_FromGrazing",
    "g_weapForceEquip", "g_weapForceEquipInstant", "g_weaponForceEquipInstant", "weapUnequip", "Unequip", "WeaponEquip",
    "WeaponUnequip", "weapSneakEquip", "SneakUnequip", "ForceWeaponEquip", "g_ForceEquip", "Sheathe", "Draw", "EquipRotateCW",
    "EquipRotateCCW", "UnequipRotateCW", "UnequipRotateCCW", "weaponInstantDown",
    // Talking gestures
    "dyn_talkGeneric", "dyn_Talk", "dialogueFlavor", "Bark", "Bark1",
};

// One-shots that would look like the actor being hit or attacking again when it shows up for someone
constexpr std::string_view kIgnorePrefixes[] = {
    "attack", "melee", "rangedattack", "throw", "grenadethrow", "minethrow", "reload", "stagger", "recoil", "hitflinch",
    "hitreaction", "hitreact", "headflinch", "dodge", "evade", "pa_paired", "pa_killmove", "paired",
};
#endif

const Map<String, String>& InstantMap() noexcept
{
    static const Map<String, String> s_map = []
    {
        Map<String, String> map;
        for (const auto& [event, instant] : kIdleToInstant)
            map[ToLower(event)] = String(instant);
        return map;
    }();
    return s_map;
}

const Set<String>& ExitSet() noexcept
{
    static const Set<String> s_set = []
    {
        Set<String> set;
        for (const auto name : kExitSpecial)
            set.insert(ToLower(name));
        return set;
    }();
    return s_set;
}

const Set<String>& IgnoreSet() noexcept
{
    static const Set<String> s_set = []
    {
        Set<String> set;
        for (const auto name : kIgnore)
            set.insert(ToLower(name));
        return set;
    }();
    return s_set;
}
} // namespace

namespace AnimationEventLists
{
std::optional<std::string_view> FindInstantCounterpart(std::string_view aEventName) noexcept
{
    const auto& map = InstantMap();
    const auto it = map.find(ToLower(aEventName));
    if (it == map.end() || it->second.empty())
        return std::nullopt;

    return std::string_view(it->second);
}

bool IsExit(std::string_view aEventName) noexcept
{
    const String name = ToLower(aEventName);
    if (ExitSet().contains(name))
        return true;

#if defined(TP_SKYRIM)
    return name.starts_with("idle") && (name.ends_with("exit") || name.ends_with("exitstart"));
#else
    // Leaving furniture or an idle (furnitureExit, AmbushExit, standStartDelayedExit, ExitFront_ToStand...), but not the
    // graph's own sub-state exits (sneakStateExit, attackStateExit) or the end of a stagger/recoil
    if (name.find("state") != String::npos || name.starts_with("stagger") || name.starts_with("recoil"))
        return false;

    return name.ends_with("exit") || name.ends_with("_tostand") || name.find("exittostand") != String::npos;
#endif
}

bool IsIgnored(std::string_view aEventName) noexcept
{
    const String name = ToLower(aEventName);
    if (IgnoreSet().contains(name))
        return true;

#if defined(TP_FALLOUT)
    for (const auto prefix : kIgnorePrefixes)
    {
        if (name.starts_with(prefix))
            return true;
    }
#endif

    return false;
}
} // namespace AnimationEventLists
