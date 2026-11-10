#include <HivemindPCH.h>

#include <Interface/MenuSounds.h>

#include <mutex>

namespace MenuSounds
{
namespace
{
// The sound descriptors (SNDR) of Fallout4.esm the game's menus play, by editor id
constexpr const char* kMenuSounds[]{
    "UIMenuOK", "UIMenuCancel", "UIMenuFocus", "UIMenuPrevNext", "UIMenuPopUpGeneric", "UIMenuInactive", "UIMenuQuantity",
};

std::mutex s_lock;
std::vector<const char*> s_queued;

// The game's data ready flag (id 1092360, the bool F4SE 0.6.23's GameDataReady hook keeps): false while the game
// loads its plugins behind the main menu. The sound descriptors exist before then, but their links to other forms are
// still form ids: playing one crashed the game twice (seen in game, a menu sound in the first 15 s).
bool IsGameDataReady() noexcept
{
    POINTER_FALLOUT4(bool, s_isGameDataReady, 1092361);
    return *s_isGameDataReady.Get();
}

// InventoryUserUIUtils::PlayMenuSound (CommonLibF4 1227993): looks the sound descriptor up by editor id and plays it
void PlayMenuSound(const char* apSound) noexcept
{
    using TPlayMenuSound = void(const char*);
    POINTER_FALLOUT4(TPlayMenuSound, s_playMenuSound, 1227994);
    s_playMenuSound.Get()(apSound);
}
} // namespace

void Queue(const std::string& acSound) noexcept
{
    for (const char* pSound : kMenuSounds)
    {
        if (acSound == pSound)
        {
            std::scoped_lock lock(s_lock);
            // A burst (hovering across a list) plays once
            if (s_queued.size() < 4)
                s_queued.push_back(pSound);
            return;
        }
    }
}

void PlayQueued() noexcept
{
    std::vector<const char*> sounds;
    {
        std::scoped_lock lock(s_lock);
        sounds.swap(s_queued);
    }

    // Clicks while the game is still loading go silent
    if (sounds.empty() || !IsGameDataReady())
        return;

    // The same sound twice in one frame is one sound
    std::sort(sounds.begin(), sounds.end());
    sounds.erase(std::unique(sounds.begin(), sounds.end()), sounds.end());
    for (const char* pSound : sounds)
        PlayMenuSound(pSound);
}
} // namespace MenuSounds
