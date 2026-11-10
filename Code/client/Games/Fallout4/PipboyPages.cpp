#include <Games/Fallout4/PipboyPages.h>

#if TP_FALLOUT4

#include <Forms/TESForm.h>
#include <Components/TESFullName.h>
#include <Games/Memory.h>

namespace
{
constexpr char kPartSeparator = '\x1D';

// BSStringT<char> (0x10, CommonLibF4): the game's own string, allocated with its plain malloc
struct GameString
{
    char* pData;
    uint16_t Length;
    uint16_t Capacity;
};
static_assert(sizeof(GameString) == 0x10);

struct Tab
{
    // HTML of each of its pages, by the message's TESDescription
    std::unordered_map<const void*, std::string> Texts;
    bool Answered{false};
};

std::mutex s_lock;
std::map<std::string, Tab> s_tabs;
std::atomic<bool> s_any{false};

std::string Escape(const std::string& acText)
{
    std::string out;
    out.reserve(acText.size());
    for (const char c : acText)
    {
        switch (c)
        {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string Upper(std::string aText)
{
    std::transform(aText.begin(), aText.end(), aText.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return aText;
}

void Assign(GameString& aOut, const std::string& acText) noexcept
{
    const size_t length = std::min<size_t>(acText.size(), 0xFFFE);
    auto* pData = static_cast<char*>(Memory::AllocatePlain(length + 1));
    if (!pData)
        return;
    std::memcpy(pData, acText.data(), length);
    pData[length] = '\0';
    if (aOut.pData)
        Memory::FreePlain(aOut.pData);
    aOut.pData = pData;
    aOut.Length = static_cast<uint16_t>(length);
    aOut.Capacity = static_cast<uint16_t>(length + 1);
}

// TESDescription::GetDescription(BSStringT<char>& out, const TESForm* parent) (CommonLibF4 id 523613, checked in the
// exe and in PipboyTabs.dll, which reads its pages' text with it: this = the message cast to TESDescription, parent
// null)
TP_THIS_FUNCTION(TGetDescription, void, void, GameString*, TESForm*);
static TGetDescription* RealGetDescription = nullptr;

void TP_MAKE_THISCALL(HookGetDescription, void, GameString* apOut, TESForm* apParent)
{
    if (apOut && s_any.load(std::memory_order_relaxed))
    {
        std::scoped_lock lock(s_lock);
        for (auto& [name, tab] : s_tabs)
        {
            const auto it = tab.Texts.find(apThis);
            if (it == tab.Texts.end())
                continue;
            Assign(*apOut, it->second);
            if (!std::exchange(tab.Answered, true))
                spdlog::info("Pip-Boy: the {} tab asked for a page's text, given", name);
            return;
        }
    }
    TiltedPhoques::ThisCall(RealGetDescription, apThis, apOut, apParent);
}
} // namespace

static TiltedPhoques::Initializer s_pipboyPagesHooks(
    []()
    {
        POINTER_FALLOUT4(TGetDescription, s_getDescription, 523614);
        RealGetDescription = s_getDescription.Get();
        TP_HOOK(&RealGetDescription, HookGetDescription);
    });

namespace PipboyPages
{
std::string ToHtml(const std::string& acMarked)
{
    std::string html;
    size_t start = 0;
    while (start < acMarked.size())
    {
        size_t end = acMarked.find('\n', start);
        if (end == std::string::npos)
            end = acMarked.size();
        const std::string line = acMarked.substr(start, end - start);
        start = end + 1;

        if (line.empty())
        {
            html += "<br>";
            continue;
        }
        const char marker = line[0];
        const std::string text = line.substr(1);
        const auto part = text.find(kPartSeparator);
        switch (marker)
        {
        case '!': html += "<font size='32' letterSpacing='1'><b>" + Escape(text) + "</b></font><br>"; break;
        case '#': html += "<font size='17' letterSpacing='3' color='#C8C8C8'><b>" + Escape(Upper(text)) + "</b></font><br>"; break;
        case '~': html += "<font size='18' color='#A0A0A0'>" + Escape(text) + "</font><br>"; break;
        case '=':
        {
            // 24 cells, filled as far as the value goes
            const int percent = part == std::string::npos ? 0 : std::clamp(std::atoi(text.substr(0, part).c_str()), 0, 100);
            const int filled = (percent * 24 + 50) / 100;
            html += "<font size='18' letterSpacing='-1'><font color='#FFFFFF'>" + std::string(static_cast<size_t>(filled), '|') + "</font><font color='#505050'>" +
                    std::string(static_cast<size_t>(24 - filled), '|') + "</font></font>  <font size='20'><b>" +
                    Escape(part == std::string::npos ? text : text.substr(part + 1)) + "</b></font><br>";
            break;
        }
        case '^':
            html += "<font size='16' color='#A0A0A0'>" + Escape(part == std::string::npos ? std::string() : text.substr(0, part)) + "</font>  <font size='19'>" +
                    Escape(part == std::string::npos ? text : text.substr(part + 1)) + "</font><br>";
            break;
        default: html += "<font size='20'>" + Escape(text) + "</font><br>"; break;
        }
    }
    return html;
}

void SetTexts(const char* acpTab, std::unordered_map<const void*, std::string> aTexts) noexcept
{
    std::scoped_lock lock(s_lock);
    s_tabs[acpTab].Texts = std::move(aTexts);
    s_any = std::any_of(s_tabs.begin(), s_tabs.end(), [](const auto& acTab) { return !acTab.second.Texts.empty(); });
}

void SetName(TESForm* apForm, const std::string& acName) noexcept
{
    auto* pFullName = Cast<TESFullName>(apForm);
    if (!pFullName)
        return;
    const char* pCurrent = pFullName->value.AsAscii();
    if (pCurrent && acName == pCurrent)
        return;
    // BSStringPool::GetEntry<char> (CommonLibF4 id 507142), as the dogs' names
    using TGetEntry = void(BSFixedString::Data*&, const char*, bool);
    POINTER_FALLOUT4(TGetEntry, s_getEntry, 507143);
    BSFixedString::Data* pEntry = nullptr;
    s_getEntry.Get()(pEntry, acName.c_str(), false);
    if (pEntry)
        pFullName->value.data = pEntry;
}
} // namespace PipboyPages

#endif
