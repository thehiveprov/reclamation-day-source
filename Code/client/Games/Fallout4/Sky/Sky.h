#pragma once

struct Sky
{
    static Sky* Get() noexcept;

    static bool s_shouldUpdateWeather;

    virtual ~Sky();

    // Changes to the weather with a transition (abAccelerate: a short one). abOverride: the game doesn't pick another
    // until the override is released.
    void SetWeather(TESWeather* apWeather, bool abOverride = true, bool abAccelerate = true) noexcept;
    // Changes to the weather at once
    void ForceWeather(TESWeather* apWeather) noexcept;
    // The game picks its own weather again
    void ReleaseWeatherOverride() noexcept;
    void ResetWeather() noexcept;

    TESWeather* GetWeather() const noexcept;
    TESWeather* GetOverrideWeather() const noexcept { return pOverrideWeather; }

    uint8_t unk8[0x48 - 0x8];
    TESWeather* pCurrentWeather; // the one shown, or being changed to
    TESWeather* pLastWeather; // being changed from
    TESWeather* pDefaultWeather;
    TESWeather* pOverrideWeather; // set by ForceWeather/SetWeather with override, until released
    uint8_t unk68[0x480 - 0x68];
};

bool Sky::s_shouldUpdateWeather = true;

static_assert(offsetof(Sky, pCurrentWeather) == 0x48);
static_assert(offsetof(Sky, pOverrideWeather) == 0x60);
static_assert(sizeof(Sky) == 0x480);
