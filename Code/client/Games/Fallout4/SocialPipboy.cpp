#include <Games/Fallout4/SocialPipboy.h>

#if TP_FALLOUT4

#include <Forms/TESForm.h>
#include <Forms/TESGlobal.h>
#include <Games/TES.h>
#include <Games/Fallout4/PipboyPages.h>

namespace
{
constexpr char kPlugin[] = "RD_Social.esp";
// Tools/BuildSocialPlugin.js: the entries, their globals, the pressed one's global
constexpr uint32_t kFirstPage = 0x800;
constexpr uint32_t kFirstShown = 0x840;
constexpr uint32_t kPressed = 0x882;
// (the log's name for it)
constexpr char kTabName[] = "SOCIAL";

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

namespace SocialPipboy
{
bool SetPages(const std::vector<Page>& acPages) noexcept
{
    if (!PluginForm(kFirstPage))
        return false;

    std::unordered_map<const void*, std::string> texts;
    const size_t count = std::min(acPages.size(), kPages);
    for (size_t i = 0; i < count; ++i)
    {
        TESForm* pPage = TESForm::GetById(PluginForm(kFirstPage + static_cast<uint32_t>(i)));
        if (!pPage)
            return false;
        PipboyPages::SetName(pPage, acPages[i].Name);
        if (const void* pDescription = Cast<TESDescription>(pPage))
            texts[pDescription] = PipboyPages::ToHtml(acPages[i].Text);
        SetShown(i, acPages[i].Dot ? 2.f : 1.f);
    }
    for (size_t hidden = count; hidden < kPages; ++hidden)
        SetShown(hidden, 0.f);

    PipboyPages::SetTexts(kTabName, std::move(texts));
    return true;
}

void Clear() noexcept
{
    if (PluginForm(kFirstPage))
        for (size_t i = 0; i < kPages; ++i)
            SetShown(i, 0.f);
    PipboyPages::SetTexts(kTabName, {});
}

int TakePressed() noexcept
{
    auto* pGlobal = Cast<TESGlobal>(TESForm::GetById(PluginForm(kPressed)));
    if (!pGlobal || pGlobal->f <= 0.f)
        return -1;
    const int index = static_cast<int>(pGlobal->f + 0.5f) - 1;
    pGlobal->f = 0.f;
    return index >= 0 && index < static_cast<int>(kPages) ? index : -1;
}
} // namespace SocialPipboy

#endif
