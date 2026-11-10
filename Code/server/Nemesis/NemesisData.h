#pragma once

#include "NemesisTypes.h"

// Nemeses: the gangs of each campaign, the districts of the Commonwealth, the name banks. Data in code, like the
// rest of the server's rules; the client has the gangs' forms (NemesisGame.cpp) under the same keys.
namespace Nemesis
{
enum CampaignMask : uint8_t
{
    k2287 = 1 << 0,
    kWinterOfAtom = 1 << 1,
    kBothCampaigns = k2287 | kWinterOfAtom,
};

enum GangFlags : uint32_t
{
    kProvokedOnly = 1 << 0, // only men wronged by a player hunt; no seeded orders
    kLawful = 1 << 1,       // takes grudges against evil players (KARMA), comes for them
    kHunters = 1 << 2,      // bounty hunters: hired by others and KARMA's hunts; hunts good (or evil) players
    kHuntsGood = 1 << 3,    // kHunters: their marks are good players (else evil ones)
    kNoWars = 1 << 4,       // doesn't go to war over ground
    kLoners = 1 << 5,       // bands without a hierarchy (scavengers)
    kMutantNames = 1 << 6,
    kAtomNames = 1 << 7,
    kRaiders = 1 << 8,      // raider gangs: hate good players more, respect evil ones
};

struct Tier
{
    const char* Label;  // plural, shown on the hierarchy ("CAPTAINS")
    const char* Single; // "Captain"
    int MinRank;
    int Seats;
};

struct GangDef
{
    const char* Id;
    const char* Name;   // "the Gunners"
    const char* Short;  // "Gunners"
    uint8_t Campaigns;
    const char* Plugin; // required plugin ("" none)
    const char* World;  // worldspace key it lives in
    const char* LeaderName;  // a lore leader (protected), "" for a generated one
    const char* LeaderTitle; // "Major"
    std::array<Tier, 3> Tiers;
    int StartMen;
    int StartPurse;
    int Alignment; // KARMA -2..2
    const char* LookPool; // looks in RD_Nemesis.esp, "" for the gang's spawn form
    int FemalePercent;
    int GhoulPercent;
    uint32_t Flags;
    float AmbushWeight;
    float NightWeight;
    std::vector<std::pair<const char*, float>> Ground;  // district, share at start
    std::vector<std::pair<const char*, int>> Relations; // other gang, -100..100
    std::vector<std::pair<const char*, const char*>> Camps; // "plugin:localid", name
    std::vector<const char*> Trades;
};

struct District
{
    const char* Key;
    const char* Name;
    const char* World;
    float X;
    float Y;
};

struct Cemetery
{
    const char* Location; // "plugin:localid"
    const char* Name;
    const char* District;
};

// Worldspace keys as games send them ("plugin:localid", hex)
constexpr const char* kCommonwealth = "Fallout4.esm:3C";
constexpr const char* kFarHarbor = "DLCCoast.esm:B0F";
constexpr const char* kNukaWorld = "DLCNukaWorld.esm:290F";

const std::vector<GangDef>& Gangs() noexcept;
const GangDef* FindGang(const std::string& acId) noexcept;
bool InCampaign(const GangDef& acGang, const std::string& acCampaign) noexcept;
// The rank range of a gang's tier: tier 0 from its MinRank up to 4 (5 is the leader), the others up to the one above
std::pair<int, int> TierRange(const GangDef& acGang, size_t aTier) noexcept;

const std::vector<District>& Districts() noexcept;
const District* FindDistrict(const std::string& acKey) noexcept;
// The district a spot is in (the nearest centre of its worldspace), nullptr outside the known worlds
const District* DistrictAt(const std::string& acWorld, float aX, float aY) noexcept;

const std::vector<Cemetery>& Cemeteries() noexcept;

// Name banks
const std::vector<const char*>& MaleNames() noexcept;
const std::vector<const char*>& FemaleNames() noexcept;
const std::vector<const char*>& Surnames() noexcept;
const std::vector<const char*>& MutantNames() noexcept;
const std::vector<const char*>& AtomNames() noexcept;
} // namespace Nemesis
