#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

// The mods Reclamation Day needs, none of which it includes (each is a separate download from its author). The main
// menu checks them (MainMenuIntegration: co-op is only played with all of them), and the launcher's window lists them
// with a Download button that opens the file on Nexus Mods as a mod manager download (Vortex or Mod Organizer 2 takes
// it from there). ReShade with TrueSight is recommended, not checked: it only changes the picture, and the check
// (ReShade's module + TrueSight-NAC.ini next to it) refused installs that had it set up another way.
namespace Requirements
{
enum class Kind
{
    kDll,      // an F4SE plugin (F4SE has loaded them all by the time the main menu shows)
    kPlugin,   // a plugin, switched on
    kF4seFile, // a file in Data\F4SE\Plugins that isn't a module (Address Library's data, an F4SE plugin's settings)
};

struct Requirement
{
    std::initializer_list<const char*> files; // all of these
    Kind kind;
    const char* pName;
    uint32_t nexusMod;  // Fallout 4's on Nexus Mods
    uint32_t nexusFile; // the one file to take (0: whichever is the mod's main file, pWhich says)
    const char* pWhich; // which file, for the launcher's list
};

// File ids from Nexus's own lists (api.nexusmods.com/v2/graphql, modFiles), 2026-11-01: the versions for Fallout 4
// 1.10.163, or the ones Reclamation Day was tested with
inline const Requirement kAll[]{
    {{"f4se_1_10_163.dll"}, Kind::kDll, "F4SE (Fallout 4 Script Extender)", 42147, 253313, "0.6.23, the one for 1.10.163"},
    {{"version-1-10-163-0.bin"}, Kind::kF4seFile, "Address Library for F4SE Plugins", 47327, 191018, "the file for 1.10.163"},
    {{"BackportedBA2Support.dll"}, Kind::kDll, "Backported Archive2 Support System", 81859, 313321, "1.0"},
    {{"GardenOfEdenPapyrusExtender.dll"}, Kind::kDll, "Garden of Eden Papyrus Script Extender", 74160, 366646, "Legacy OG, the one for 1.10.163"},
    {{"FullBodyFirstPerson.dll"}, Kind::kDll, "Full Body First Person", 109356, 0, "the main file"},
    {{"UneducatedShooter.dll"}, Kind::kDll, "Uneducated Shooter", 56789, 0, "the main file"},
    {{"ActorVelocityFramework.dll"}, Kind::kDll, "Actor Velocity Framework", 56604, 225388, "OG (Stable) 1.4"},
    {{"Unofficial Fallout 4 Patch.esp"}, Kind::kPlugin, "Unofficial Fallout 4 Patch", 4598, 407774, "2.2.2a"},
    {{"StartMeUp.esp", "StartMeUpShaunDefaultAppearance.esp"}, Kind::kPlugin, "Start Me Up Redux", 56984, 405245, "Normal - XDI Dialogue, 9.2"},
    // Karma, skills and their Pip-Boy pages, and the dialogue menu the party votes read (bundled up to 1.6.7-285; a
    // player who had them keeps the copies the launcher put in Data)
    {{"KARMA.esm"}, Kind::kPlugin, "KARMA", 80425, 416526, "3.4.1"},
    {{"Be Exceptional.esp"}, Kind::kPlugin, "Be Exceptional - Skills and Perk Overhaul", 28222, 124760, "Skills and Perks 0.97d"},
    {{"PipboyTabs.dll"}, Kind::kDll, "PipboyTabs", 29568, 172069, "the file for 1.10.163"},
    {{"PRKF.dll"}, Kind::kDll, "LevelUpMenuEx", 28822, 172070, "the file for 1.10.163"},
    {{"XDI.esm"}, Kind::kPlugin, "Extended Dialogue Interface", 27216, 172181, "1.3.6, the one for 1.10.163"},
    // The graphics, the same for everyone (the host's server checks the plugins too)
    {{"Flutter Flicker Fixer For Foliage.esp"}, Kind::kPlugin, "Flutter Flicker Fixer For Foliage", 79853, 393204, "Vanilla"},
    {{"Stiff Splines.esp"}, Kind::kPlugin, "This Made My Spline Stiff", 98935, 376653, "1.0"},
    {{"VividFallout - AiO - BestChoice.esp"}, Kind::kPlugin, "Vivid Fallout - All in One", 25714, 392180, "Best Choice"},
    {{"NAC.esp", "NAC-FH.esp", "NAC-NW.esp"}, Kind::kPlugin, "NAC X - Natural and Atmospheric Commonwealth", 46722, 188430, "Legacy edition"},
    {{"Upscaling.dll"}, Kind::kDll, "Upscaling", 99130, 0, "the Pre-NG (version 1.10.163) file under Update files"},
    {{"MotionVectorFixes.dll"}, Kind::kDll, "Motion Vector Fixes", 98544, 0, "the file for 1.10.163"},
    // The frame rate in the city: rebuilt precombined meshes and previs (the launcher keeps PRP.esp last, Updater)
    {{"PPF.esm", "PRP.esp"}, Kind::kPlugin, "Previsibines Repair Pack", 46403, 395773, "Full (1.10.163)"},
    {{"HighFPSPhysicsFix.dll"}, Kind::kDll, "High FPS Physics Fix", 44798, 0, "the main file"},
    {{"mcm.dll"}, Kind::kDll, "Mod Configuration Menu", 21497, 172180, "1.39, the one for 1.10.163"},
};

// The file on Nexus Mods as a mod manager download (Vortex or Mod Organizer 2 gets it once the player presses Download
// there), or the mod's files to pick from
inline std::string DownloadUrl(const Requirement& acRequirement)
{
    std::string url = "https://www.nexusmods.com/fallout4/mods/" + std::to_string(acRequirement.nexusMod) + "?tab=files";
    if (acRequirement.nexusFile)
        url += "&file_id=" + std::to_string(acRequirement.nexusFile) + "&nmm=1";
    return url;
}

// Where a requirement's file is when it's installed: F4SE itself next to the game, F4SE's plugins and their files in
// Data\F4SE\Plugins, plugins in Data (relative to the game's folder)
inline std::string InstalledPath(const Requirement& acRequirement, const char* apFile)
{
    if (acRequirement.kind == Kind::kPlugin)
        return std::string("Data\\") + apFile;
    if (std::string(apFile) == "f4se_1_10_163.dll")
        return apFile;
    return std::string("Data\\F4SE\\Plugins\\") + apFile;
}
} // namespace Requirements
