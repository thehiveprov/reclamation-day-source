#include <Games/Fallout4/NemesisPipboy.h>

#if TP_FALLOUT4

#include <World.h>
#include <Forms/TESForm.h>
#include <Forms/TESGlobal.h>
#include <Components/TESFullName.h>
#include <Games/Memory.h>
#include <Games/TES.h>
#include <Games/Fallout4/PipboyPages.h>

namespace
{
constexpr char kPlugin[] = "RD_Nemeses.esp";
// Tools/BuildNemesesPlugin.js: the pages and their globals; Code/server/Nemesis/NemesisTypes.h kPipboyPages and the
// separators and markers
constexpr uint32_t kFirstPage = 0x800;
constexpr uint32_t kFirstShown = 0x840;
constexpr size_t kPages = 40;
// RD_NemesisOpenScreen: RDNemesesTab.psc sets it when the first entry (OPEN FULL SCREEN) is pressed
constexpr uint32_t kOpenScreen = 0x882;
// (the log's name for it)
constexpr char kTabName[] = "NEMESIS";
constexpr char kEntrySeparator = '\x1E';
constexpr char kFieldSeparator = '\x1F';
// The hotkey's name, for the pages' {KEY}
std::string s_keyName = "N";

uint32_t PluginForm(uint32_t aLocalId) noexcept
{
    const Mod* pMod = ModManager::Get()->GetByName(kPlugin);
    return pMod && pMod->IsLoaded() ? pMod->GetFormId(aLocalId) : 0;
}

void SetShown(size_t aIndex, float aValue) noexcept
{
    if (auto* pGlobal = Cast<TESGlobal>(TESForm::GetById(PluginForm(kFirstShown + static_cast<uint32_t>(aIndex)))))
        pGlobal->f = aValue;
}
} // namespace

namespace NemesisPipboy
{
void SetPages(const std::string& acPages) noexcept
{
    if (!PluginForm(kFirstPage))
        return;

    std::unordered_map<const void*, std::string> texts;
    size_t index = 0;
    size_t start = 0;
    while (index < kPages)
    {
        const size_t end = acPages.find(kEntrySeparator, start);
        if (end == std::string::npos)
            break;
        const std::string entry = acPages.substr(start, end - start);
        start = end + 1;

        const size_t first = entry.find(kFieldSeparator);
        const size_t last = entry.rfind(kFieldSeparator);
        if (first == std::string::npos || last == first)
            continue;
        TESForm* pPage = TESForm::GetById(PluginForm(kFirstPage + static_cast<uint32_t>(index)));
        if (!pPage)
            break;

        PipboyPages::SetName(pPage, entry.substr(0, first));
        std::string text = entry.substr(first + 1, last - first - 1);
        for (size_t key = text.find("{KEY}"); key != std::string::npos; key = text.find("{KEY}", key))
            text.replace(key, 5, s_keyName);
        if (const void* pDescription = Cast<TESDescription>(pPage))
            texts[pDescription] = PipboyPages::ToHtml(text);
        SetShown(index, entry.substr(last + 1) == "1" ? 2.f : 1.f);
        ++index;
    }
    for (size_t hidden = index; hidden < kPages; ++hidden)
        SetShown(hidden, 0.f);

    PipboyPages::SetTexts(kTabName, std::move(texts));
}

void Clear() noexcept
{
    if (PluginForm(kFirstPage))
        for (size_t i = 0; i < kPages; ++i)
            SetShown(i, 0.f);
    PipboyPages::SetTexts(kTabName, {});
}

void SetKeyName(const std::string& acName) noexcept
{
    s_keyName = acName;
}

bool TakeOpenRequest() noexcept
{
    auto* pGlobal = Cast<TESGlobal>(TESForm::GetById(PluginForm(kOpenScreen)));
    if (!pGlobal || pGlobal->f <= 0.f)
        return false;
    pGlobal->f = 0.f;
    return true;
}
} // namespace NemesisPipboy

#endif
