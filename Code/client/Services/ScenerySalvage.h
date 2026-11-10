#pragma once

struct TESForm;
struct TESObjectREFR;

/**
 * @brief Scrap anything (Fallout 4): what the world's scenery is called and what scrapping it gives. The game has
 * neither for most of it (Scrap Everything adds both with its plugin: names, and scrap recipes by kind of object):
 * both come from the object's mesh here. Its name from the mesh's file name (Landscape\Plants\VineHanging01.nif:
 * "Vine Hanging"), what it gives from the kind of thing the mesh's path says it is, more for larger objects: mixes of what
 * it's made of (vines fertilizer and fiber as cloth, trees wood and fertilizer, rubble concrete and rebar as steel, wrecks
 * steel, rubber, glass, aluminum and plastic, ...), our own rather than Scrap Everything's single materials.
 */
namespace ScenerySalvage
{
// A name for an object the game has none for, null if its mesh doesn't give one (statics, movable statics, static
// collections and trees). Stays valid.
[[nodiscard]] const char* GetLabel(const TESForm* apBase) noexcept;

// What scrapping a placed object gives: scrap items (the c_..._scrap misc items workbenches keep) and how many
[[nodiscard]] Vector<std::pair<uint32_t, uint32_t>> GetSalvage(const TESObjectREFR* apRef) noexcept;
} // namespace ScenerySalvage
