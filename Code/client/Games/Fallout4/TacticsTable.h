// Written by Tools/BuildCombatPlugin.js with RD_Combat.esp: change the table there and run it again, never this file.
// The enemy tactics profiles (docs/ENEMY_TACTICS.md) and where their combat styles start in RD_Combat.esp. A profile's
// styles are its variants in order, each with its moods in order: FirstStyle + variant * MoodCount + mood.
#pragma once

#include <cstdint>

namespace Tactics
{
enum class Kind : uint8_t
{
    kPerson, // variants ranged, melee, heavy; moods steady, press, shaken, flank
    kMutant, // variants ranged, melee (both from their vanilla style); moods steady, rage
    kSynth,  // variants ranged, melee; moods steady, flank
    kRobot,  // its own race's style; moods steady, flank
};

struct Profile
{
    const char* Name;
    Kind Type;
    float Aggression;
    float Discipline;
    float Cunning;
    float Fearlessness;
    uint8_t VariantCount;
    uint8_t MoodCount;
    uint32_t FirstStyle; // local id in RD_Combat.esp
};

inline constexpr Profile kProfiles[] = {
    {"Generic", Kind::kPerson, 0.50f, 0.55f, 0.35f, 0.30f, 3, 4, 0xA00},
    {"Raiders", Kind::kPerson, 0.65f, 0.30f, 0.25f, 0.20f, 3, 4, 0xA0C},
    {"Gunners", Kind::kPerson, 0.55f, 0.85f, 0.70f, 0.60f, 3, 4, 0xA18},
    {"Operators", Kind::kPerson, 0.60f, 0.80f, 0.75f, 0.55f, 3, 4, 0xA24},
    {"Disciples", Kind::kPerson, 0.75f, 0.40f, 0.80f, 0.40f, 3, 4, 0xA30},
    {"Pack", Kind::kPerson, 0.90f, 0.30f, 0.30f, 0.60f, 3, 4, 0xA3C},
    {"Triggermen", Kind::kPerson, 0.70f, 0.45f, 0.30f, 0.35f, 3, 4, 0xA48},
    {"Bos", Kind::kPerson, 0.65f, 0.90f, 0.55f, 0.80f, 3, 4, 0xA54},
    {"Atom", Kind::kPerson, 0.85f, 0.35f, 0.20f, 1.00f, 3, 4, 0xA60},
    {"Trappers", Kind::kPerson, 0.45f, 0.70f, 0.90f, 0.45f, 3, 4, 0xA6C},
    {"RustDevils", Kind::kPerson, 0.60f, 0.65f, 0.60f, 0.45f, 3, 4, 0xA78},
    {"Forged", Kind::kPerson, 0.85f, 0.30f, 0.20f, 0.50f, 3, 4, 0xA84},
    {"Minutemen", Kind::kPerson, 0.40f, 0.75f, 0.40f, 0.50f, 3, 4, 0xA90},
    {"Railroad", Kind::kPerson, 0.50f, 0.70f, 0.75f, 0.50f, 3, 4, 0xA9C},
    {"Courser", Kind::kSynth, 0.75f, 0.90f, 0.90f, 1.00f, 2, 2, 0xAA8},
    {"Mutant", Kind::kMutant, 0.75f, 0.45f, 0.45f, 1.00f, 2, 2, 0xAAC},
    {"Synth", Kind::kSynth, 0.55f, 0.95f, 0.90f, 1.00f, 2, 2, 0xAB0},
    {"Assaultron", Kind::kRobot, 0.85f, 0.70f, 0.95f, 1.00f, 1, 2, 0xAB4},
    {"Protectron", Kind::kRobot, 0.50f, 0.90f, 0.40f, 1.00f, 1, 2, 0xAB6},
    {"SentryBot", Kind::kRobot, 0.80f, 0.80f, 0.50f, 1.00f, 1, 2, 0xAB8},
    {"Gutsy", Kind::kRobot, 0.65f, 0.90f, 0.85f, 1.00f, 1, 2, 0xABA},
    {"Handy", Kind::kRobot, 0.60f, 0.70f, 0.60f, 1.00f, 1, 2, 0xABC},
    {"Eyebot", Kind::kRobot, 0.40f, 0.80f, 0.70f, 1.00f, 1, 2, 0xABE},
    {"Robobrain", Kind::kRobot, 0.55f, 0.95f, 0.90f, 1.00f, 1, 2, 0xAC0},
    {"Bot", Kind::kRobot, 0.65f, 0.85f, 0.80f, 1.00f, 1, 2, 0xAC2},
};
} // namespace Tactics
