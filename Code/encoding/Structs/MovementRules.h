#pragma once

using TiltedPhoques::Buffer;

// The host's movement and parkour rules, which apply to every player on a hosted world (MovementService, ParkourService;
// RD's MCM page Movement). How a player's own game feels (the slide's camera shake, the ledge markers) stays theirs.
struct MovementRules
{
    bool operator==(const MovementRules& acRhs) const noexcept;
    bool operator!=(const MovementRules& acRhs) const noexcept { return !(*this == acRhs); }

    void Serialize(TiltedPhoques::Buffer::Writer& aWriter) const noexcept;
    void Deserialize(TiltedPhoques::Buffer::Reader& aReader) noexcept;

    bool Sliding{true};
    float SlideApCost{0.20f}; // share of max AP
    bool CheaperSprintOutOfCombat{true};
    float SprintOutOfCombatCost{0.5f}; // share of the normal AP drain, 0 = free
    bool SwimmingApDrain{true};
    bool SneakingApDrain{true};
    bool LandingStumble{true};
    bool Parkour{true};
    float ParkourApCost{0.15f}; // share of max AP a climb as high as the head takes
    bool LandingRoll{true};
    bool NpcParkour{true}; // human NPCs climb and vault what's in their way
    bool NpcTactics{true}; // enemies fight like what they are, and change as the fight turns (TacticsService)
    bool NpcSlides{true};  // people in a fight slide into cover and at their target
};
