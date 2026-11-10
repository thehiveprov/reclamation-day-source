#include <HivemindPCH.h>

#include <StartMeUp.h>

#include <Games/TES.h>
#include <Forms/TESForm.h>
#include <Misc/BSFixedString.h>

#include <ShlObj.h>
#include <fstream>

namespace StartMeUp
{
namespace
{
constexpr char kPlugin[] = "StartMeUp.esp";

// Its first prompt, SMU_Door: "You begin to notice that this isn't reality..." (the same id in 7.4 and 9.2)
constexpr uint32_t kDoorMessageId = 0x000F9A;

constexpr uint8_t kMessageFormType = 0x6C; // MESG

const Mod* FindLoadedPlugin() noexcept
{
    const ModManager* pManager = ModManager::Get();
    const Mod* pMod = pManager ? pManager->GetByName(kPlugin) : nullptr;
    return pMod && pMod->GetId() != 0xFF ? pMod : nullptr;
}

// plugins.txt: one plugin per line, active ones start with '*'
bool IsActiveInPluginsFile() noexcept
{
    PWSTR pLocalAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pLocalAppData)))
        return false;
    const std::filesystem::path path = std::filesystem::path(pLocalAppData) / L"Fallout4" / L"plugins.txt";
    CoTaskMemFree(pLocalAppData);

    std::ifstream file(path);
    for (std::string line; std::getline(file, line);)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() > 1 && line[0] == '*' && _stricmp(line.c_str() + 1, kPlugin) == 0)
            return true;
    }
    return false;
}
} // namespace

bool IsInstalled(bool aGameDataLoaded) noexcept
{
    return aGameDataLoaded ? FindLoadedPlugin() != nullptr : IsActiveInPluginsFile();
}

bool BlockPrewarMemory() noexcept
{
    // Not loaded yet (its load index is set once the load order is compiled), or not installed: tried again later
    const Mod* pMod = FindLoadedPlugin();
    if (!pMod)
    {
        static bool s_logged = false;
        const ModManager* pManager = ModManager::Get();
        if (const Mod* pFile = pManager ? pManager->GetByName(kPlugin) : nullptr; pFile && !std::exchange(s_logged, true))
            spdlog::info("Start Me Up: {} is there but not loaded yet (load index {:X})", kPlugin, pFile->GetId());
        return false;
    }

    TESForm* pForm = TESForm::GetById(pMod->GetFormId(kDoorMessageId));
    if (!pForm || static_cast<uint8_t>(pForm->formType) != kMessageFormType)
    {
        spdlog::warn("Start Me Up: its first prompt ({:X}) isn't there, can't remove the pre-war memory choice", kDoorMessageId);
        return true;
    }

    // BGSMessage::buttonList, a BSSimpleList<MESSAGEBOX_BUTTON*> at 0x58 (its first node inline); a button starts with
    // its text (BGSLocalizedString), alandtse/CommonLibF4
    struct Node
    {
        void* pButton;
        Node* pNext;
    };

    Node* pPrevious = nullptr;
    for (auto* pNode = reinterpret_cast<Node*>(reinterpret_cast<uint8_t*>(pForm) + 0x58); pNode && pNode->pButton; pPrevious = pNode, pNode = pNode->pNext)
    {
        const auto* pText = reinterpret_cast<const BSFixedString*>(pNode->pButton);
        const char* text = pText->data && pText->IsAscii() ? pText->AsAscii() : nullptr;
        if (!text || !std::strstr(text, "stay here"))
            continue;

        // The script gets the index of the chosen button: only the last one can go without renumbering the others
        if (!pPrevious || pNode->pNext)
        {
            spdlog::warn("Start Me Up: the pre-war memory choice isn't the last one of its prompt, left as is");
            return true;
        }

        pPrevious->pNext = nullptr;
        spdlog::info("Start Me Up: removed the \"{}\" choice, co-op characters start past the prologue", text);
        return true;
    }

    spdlog::warn("Start Me Up: the pre-war memory choice wasn't found in its first prompt");
    return true;
}
} // namespace StartMeUp
