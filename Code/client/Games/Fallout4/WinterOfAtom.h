#pragma once

struct TESGlobal;
struct TESObjectREFR;
struct TESTopicInfo;

// Winter of Atom, the prequel campaign: its plugins and the records the client reads or writes. Records are found by
// plugin and local ID (they are defined in the RDWinterOfAtom project, tools/esp/areas/start.js), never by load order.
namespace WinterOfAtom
{
// installed with the campaign, loaded in both campaigns
inline constexpr char kMainPlugin[] = "RD_WinterOfAtom.esp";
// only in the prequel campaign's load order (Data\RDWoA\PrequelPlugins.txt)
inline constexpr char kPrequelPlugin[] = "RD_WoA_Winter.esp";

// globals in kMainPlugin
inline constexpr uint32_t kStartDone = 0xA06;        // 1 once a new character's start (origin, face, kit...) is over
inline constexpr uint32_t kTrainJobReady = 0xA07;    // 1 once the prologue (RDWoA_MQ00) is over
inline constexpr uint32_t kCinematicRequest = 0xA08; // the number of a cinematic for CinematicService to play
inline constexpr uint32_t kCinematicDone = 0xA09;    // the number of the cinematic that just ended
inline constexpr uint32_t kPipboysOnTable = 0xA0B;   // the Pip-Boys the prologue trader's table shows (WinterOfAtomService)

// The game runs the prequel campaign: its prequel-only plugin is loaded
bool IsPrequelLoaded() noexcept;

// A global of kMainPlugin, null when it isn't loaded
TESGlobal* GetGlobal(uint32_t aLocalId) noexcept;

// One of Winter of Atom's story actors: a reference whose base is a kMainPlugin NPC (the prologue's trader and gate
// crowd, which its scripts place). Kept when Reclamation Day connects to a server, unlike other temporary actors.
bool IsStoryActor(const TESObjectREFR* apRef) noexcept;

// Winter of Atom's own lines have no voices yet (silent voice files keep each one up for its time), so they are always
// subtitled, whatever the player's subtitle settings: the game's ShowSubtitle (hooked in SubtitleManager.cpp) hands
// each line's topic info here before it decides. Main thread.
void OnSubtitle(const TESTopicInfo* apInfo) noexcept;
// The player's own subtitle settings back once no such line is up (WinterOfAtomService, once a second)
void UpdateSubtitles() noexcept;
// A silent line is up, whatever the player's subtitle settings: as OnSubtitle for anyone's lines (the caravan trader's)
void KeepSubtitlesOn() noexcept;
} // namespace WinterOfAtom
