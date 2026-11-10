#pragma once

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <string>
#include <vector>

// plugins.txt's text, one plugin per line, the active ones starting with '*'. Plain text in, plain text out (no game
// or file access), so the launcher's load order changes can be tested on their own.
namespace PluginsTxt
{
// acText with acPlugins switched on (aActive) or off. Lines it already has keep their place and only gain or lose the
// '*'; switching on, the plugins it doesn't list yet go at its end, in acPlugins' order. Names compare without case.
// aChanged gets the plugins that changed; when it stays empty, the returned text is acText unchanged.
inline std::string SetActive(const std::string& acText, const std::vector<std::string>& acPlugins, bool aActive, std::vector<std::string>& aChanged)
{
    const auto lower = [](std::string aText)
    {
        std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return aText;
    };
    const auto trim = [](const std::string& acLine)
    {
        const auto first = acLine.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return std::string{};
        return acLine.substr(first, acLine.find_last_not_of(" \t\r\n") - first + 1);
    };

    const size_t changedBefore = aChanged.size();
    std::set<std::string> wanted;
    for (const std::string& plugin : acPlugins)
        wanted.insert(lower(plugin));

    std::set<std::string> listed;
    std::string text;
    std::istringstream lines(acText);
    for (std::string rawLine; std::getline(lines, rawLine);)
    {
        std::string line = trim(rawLine);
        const bool active = !line.empty() && line[0] == '*';
        if (active)
            line.erase(0, 1);
        const std::string name = lower(line);
        listed.insert(name);

        if (!line.empty() && line[0] != '#' && wanted.contains(name) && active != aActive)
        {
            text += (aActive ? "*" : "") + line + "\r\n";
            aChanged.push_back(line);
        }
        else
            text += rawLine + "\n";
    }

    if (aActive)
    {
        for (const std::string& plugin : acPlugins)
        {
            if (!listed.insert(lower(plugin)).second)
                continue;
            text += "*" + plugin + "\r\n";
            aChanged.push_back(plugin);
        }
    }

    return aChanged.size() == changedBefore ? acText : text;
}

// acText with acPlugins (those it lists, active or not) moved to its end, in acPlugins' order: for plugins that must
// load after everything else (the Previsibines Repair Pack rebuilds the precombined meshes and previs of the cells it
// covers, and a plugin after it editing one of those cells undoes them). aMoved gets the plugins moved; when it stays
// empty (they're already the last lines, in that order, or not listed), the returned text is acText unchanged.
inline std::string MoveToEnd(const std::string& acText, const std::vector<std::string>& acPlugins, std::vector<std::string>& aMoved)
{
    const auto lower = [](std::string aText)
    {
        std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return aText;
    };
    const auto trim = [](const std::string& acLine)
    {
        const auto first = acLine.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return std::string{};
        return acLine.substr(first, acLine.find_last_not_of(" \t\r\n") - first + 1);
    };
    const auto nameOf = [&](const std::string& acLine)
    {
        const std::string line = trim(acLine);
        return lower(!line.empty() && line[0] == '*' ? line.substr(1) : line);
    };

    std::vector<std::string> kept;
    // each wanted plugin's line as listed (its '*' kept), by its place in acPlugins
    std::vector<std::string> moved(acPlugins.size());
    // the plugin lines in order, to see whether the wanted ones already end the file
    std::vector<std::string> pluginOrder;
    std::istringstream lines(acText);
    for (std::string rawLine; std::getline(lines, rawLine);)
    {
        const std::string name = nameOf(rawLine);
        const std::string line = trim(rawLine);
        if (!name.empty() && line[0] != '#')
            pluginOrder.push_back(name);

        bool wanted = false;
        for (size_t i = 0; i < acPlugins.size(); ++i)
        {
            if (!name.empty() && name == lower(acPlugins[i]))
            {
                if (moved[i].empty())
                    moved[i] = line;
                wanted = true;
                break;
            }
        }
        if (!wanted)
            kept.push_back(rawLine);
    }

    std::vector<std::string> wantedOrder;
    for (size_t i = 0; i < acPlugins.size(); ++i)
    {
        if (!moved[i].empty())
            wantedOrder.push_back(lower(acPlugins[i]));
    }
    if (wantedOrder.empty())
        return acText;
    if (pluginOrder.size() >= wantedOrder.size() && std::equal(wantedOrder.rbegin(), wantedOrder.rend(), pluginOrder.rbegin()))
        return acText;

    // blank lines at the end go, so the moved plugins are the last lines
    while (!kept.empty() && trim(kept.back()).empty())
        kept.pop_back();
    std::string text;
    for (const std::string& line : kept)
        text += line + "\n";
    for (size_t i = 0; i < acPlugins.size(); ++i)
    {
        if (moved[i].empty())
            continue;
        text += moved[i] + "\r\n";
        aMoved.push_back(acPlugins[i]);
    }
    return text;
}
} // namespace PluginsTxt
