#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

namespace ESLoader
{
struct PluginData;
}

struct ModsComponent
{
    struct Entry
    {
        uint32_t id;
        uint32_t refCount;
    };

    // A plugin's id in this world (the one it got before, else a new one, saved). Ids go with the plugin's file name, so
    // a plugin keeps its id whatever the load order, and when it turns from standard to light or back.
    uint32_t AddStandard(const String& acpFilename) noexcept;
    uint32_t AddLite(const String& acpFilename) noexcept;

    // The ids this world gave its plugins on earlier runs (plugin-ids.txt in the world's folder, RD_WORLD_DATA as
    // settlements.txt): everything the world saved with them (settlements, C.A.M.P.s, claims, nemeses) keeps meaning
    // the same plugins after the load order changes. Once, at start.
    void LoadSavedIds() noexcept;
    // Against the plugins this world has known: those in acFilenames it never had (new) and those it had that aren't
    // there (gone; what was saved from them waits until they're back). Empty when the world has no saved ids yet.
    void CompareLoadOrder(const Vector<String>& acFilenames, Vector<String>& aNew, Vector<String>& aGone) const noexcept;
    [[nodiscard]] bool HasSavedIds() const noexcept { return m_hadSavedIds; }

    void AddServerMod(const ESLoader::PluginData& acData);

    const auto& GetStandardMods() const noexcept { return m_standardMods; }
    const auto& GetLiteMods() const noexcept { return m_liteMods; }
    const auto& GetServerMods() const noexcept { return m_serverMods; }

    bool IsInstalled(const String& acpFileName) const noexcept;

    using TModList = TiltedPhoques::Map<String, Entry>;

private:
    uint32_t Add(TModList& aList, const TModList& acOther, const String& acpFilename) noexcept;
    void SaveIds() const noexcept;

    bool m_hadSavedIds = false;
    uint32_t m_seed = 0;
    // Mappings of ids owned by the server
    TModList m_standardMods;
    TModList m_liteMods;

    // List of mods installed on the server.
    TModList m_serverMods;
};
