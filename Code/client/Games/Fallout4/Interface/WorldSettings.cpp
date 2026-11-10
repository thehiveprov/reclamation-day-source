#include <HivemindPCH.h>

#include <Interface/WorldSettings.h>

#include <Interface/MainMenuIntegration.h>
#include <Interface/Updater.h>
#include <Games/Fallout4/PapyrusCall.h>
#include <Games/TES.h>
#include <Forms/TESForm.h>
#include <Forms/TESGlobal.h>
#include <MiniJson.h>
#include <Services/OverlayService.h>
#include <Services/TransportService.h>
#include <World.h>

#include <include/cef_parser.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

namespace
{
// A function run after an entry changes or on its press: a quest script's, or a script's global one
struct Call
{
    std::string quest; // "Plugin|local id"; empty: a global function
    std::string script;
    std::string function;
    CefRefPtr<CefListValue> params;
};

// A slider that never writes its global (a value the mod keeps in step with its own state): each step of a change calls
// the quest function with the up or the down argument
struct Stepper
{
    std::string quest;
    std::string script;
    std::string function;
    int up{2};
    int down{1};
};

// One entry of a page, as WorldSettings.json has it
struct Item
{
    std::string type;
    std::string text;
    std::string help;
    // who sees it: "host", "guest", "2287", "prequel", joined by '+'
    std::string show;
    // its source: a global ("Plugin|local id") or one of the page's own values
    std::string global;
    std::string own;
    std::vector<std::string> options;
    // a dropdown's options are these values of its source
    std::vector<double> values;
    double min{0.0};
    double max{1.0};
    double step{1.0};
    std::optional<Stepper> stepper;
    std::optional<Call> call;
    // own values put back to their defaults on its press
    std::vector<std::string> resetOwn;

    // the shown value (a dropdown's option), and a stepper's new value until its global has it
    double value{0.0};
    std::optional<double> target;
};

struct Page
{
    std::string name;
    std::vector<Item> items;
};

struct State
{
    bool open{false};
    std::vector<Page> pages;
    int page{0};
    std::string error;
    std::map<std::string, double> own;
    std::map<std::string, double> ownDefaults;
    std::string sent;
    int frames{0};
};
State s;

OverlayService& Overlay() noexcept
{
    return World::Get().ctx().at<OverlayService>();
}

std::filesystem::path PagePath() noexcept
{
    return std::filesystem::current_path() / L"Data" / L"ReclamationDay" / L"WorldSettings.json";
}

std::filesystem::path OwnPath() noexcept
{
    return TiltedPhoques::GetPath() / "config" / "WorldSettings.ini";
}

std::string Str(const CefRefPtr<CefDictionaryValue>& acDict, const char* acpKey) noexcept
{
    return acDict && acDict->GetType(acpKey) == VTYPE_STRING ? acDict->GetString(acpKey).ToString() : std::string{};
}

double Num(const CefRefPtr<CefValue>& acValue, double aDefault) noexcept
{
    if (!acValue)
        return aDefault;
    switch (acValue->GetType())
    {
    case VTYPE_INT: return acValue->GetInt();
    case VTYPE_DOUBLE: return acValue->GetDouble();
    case VTYPE_BOOL: return acValue->GetBool() ? 1.0 : 0.0;
    default: return aDefault;
    }
}

double Num(const CefRefPtr<CefDictionaryValue>& acDict, const char* acpKey, double aDefault) noexcept
{
    return acDict && acDict->HasKey(acpKey) ? Num(acDict->GetValue(acpKey), aDefault) : aDefault;
}

std::vector<std::string> Strings(const CefRefPtr<CefDictionaryValue>& acDict, const char* acpKey) noexcept
{
    std::vector<std::string> result;
    const auto list = acDict->GetType(acpKey) == VTYPE_LIST ? acDict->GetList(acpKey) : nullptr;
    for (size_t i = 0; list && i < list->GetSize(); ++i)
        result.push_back(list->GetType(i) == VTYPE_STRING ? list->GetString(i).ToString() : std::string{});
    return result;
}

std::optional<Call> ParseCall(const CefRefPtr<CefDictionaryValue>& acDict) noexcept
{
    if (!acDict)
        return std::nullopt;
    Call call{Str(acDict, "quest"), Str(acDict, "script"), Str(acDict, "function"), nullptr};
    // A copy: CEF's GetList refers into the parsed page, which is gone (and the list empty) once the page is read
    if (acDict->GetType("params") == VTYPE_LIST)
        call.params = acDict->GetList("params")->Copy();
    return call;
}

Item ParseItem(const CefRefPtr<CefDictionaryValue>& acEntry) noexcept
{
    Item item;
    item.type = Str(acEntry, "type");
    item.text = Str(acEntry, "text");
    item.help = Str(acEntry, "help");
    item.show = Str(acEntry, "show");
    item.global = Str(acEntry, "global");
    item.own = Str(acEntry, "own");
    item.options = Strings(acEntry, "options");
    item.resetOwn = Strings(acEntry, "resetOwn");
    if (acEntry->GetType("values") == VTYPE_LIST)
    {
        const auto list = acEntry->GetList("values");
        for (size_t i = 0; i < list->GetSize(); ++i)
            item.values.push_back(Num(list->GetValue(i), 0.0));
    }
    item.min = Num(acEntry, "min", 0.0);
    item.max = Num(acEntry, "max", 1.0);
    item.step = Num(acEntry, "step", 1.0);
    if (acEntry->GetType("stepper") == VTYPE_DICTIONARY)
    {
        const auto stepper = acEntry->GetDictionary("stepper");
        item.stepper = Stepper{Str(stepper, "quest"), Str(stepper, "script"), Str(stepper, "function"), static_cast<int>(Num(stepper, "up", 2.0)),
                               static_cast<int>(Num(stepper, "down", 1.0))};
    }
    if (acEntry->GetType("call") == VTYPE_DICTIONARY)
        item.call = ParseCall(acEntry->GetDictionary("call"));
    return item;
}

// A form by "Plugin|local id" (hex), none when its plugin isn't loaded
TESForm* FormOf(const std::string& acRef) noexcept
{
    const size_t bar = acRef.find('|');
    const ModManager* pManager = ModManager::Get();
    if (bar == std::string::npos || !pManager)
        return nullptr;
    const Mod* pMod = pManager->GetByName(acRef.substr(0, bar).c_str());
    if (!pMod || !pMod->IsLoaded())
        return nullptr;
    return TESForm::GetById(pMod->GetFormId(static_cast<uint32_t>(std::strtoul(acRef.c_str() + bar + 1, nullptr, 16))));
}

TESGlobal* GlobalOf(const Item& acItem) noexcept
{
    return acItem.global.empty() ? nullptr : Cast<TESGlobal>(FormOf(acItem.global));
}

// The page's own values: config\WorldSettings.ini, "name=value" lines over the page's defaults
void LoadOwn() noexcept
{
    s.own = s.ownDefaults;
    std::ifstream file(OwnPath());
    for (std::string line; std::getline(file, line);)
    {
        const size_t equals = line.find('=');
        if (equals != std::string::npos && s.ownDefaults.contains(line.substr(0, equals)))
            s.own[line.substr(0, equals)] = std::atof(line.c_str() + equals + 1);
    }
}

void SaveOwn() noexcept
{
    std::error_code ec;
    std::filesystem::create_directories(OwnPath().parent_path(), ec);
    std::ofstream file(OwnPath(), std::ios::trunc);
    for (const auto& [name, value] : s.own)
        file << name << '=' << value << '\n';
}

bool Load() noexcept
{
    s.pages.clear();
    s.ownDefaults.clear();
    s.error.clear();

    std::ifstream file(PagePath(), std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    const auto root = CefParseJSON(text.str(), JSON_PARSER_ALLOW_TRAILING_COMMAS);
    const auto dict = root && root->GetType() == VTYPE_DICTIONARY ? root->GetDictionary() : nullptr;
    if (!dict)
    {
        s.error = "The world settings page is missing (Data\\ReclamationDay\\WorldSettings.json).";
        spdlog::error("World settings: {} isn't there or isn't JSON", PagePath().string());
        return false;
    }

    if (const std::string required = Str(dict, "requires"); !required.empty())
    {
        const ModManager* pManager = ModManager::Get();
        const Mod* pMod = pManager ? pManager->GetByName(required.c_str()) : nullptr;
        if (!pMod || !pMod->IsLoaded())
            s.error = fmt::format("{} isn't loaded: NAC X's settings need it.", required);
    }

    if (dict->GetType("own") == VTYPE_DICTIONARY)
    {
        const auto own = dict->GetDictionary("own");
        CefDictionaryValue::KeyList keys;
        own->GetKeys(keys);
        for (const auto& key : keys)
            s.ownDefaults[key.ToString()] = Num(own->GetValue(key), 0.0);
    }
    LoadOwn();

    const auto pages = dict->GetType("pages") == VTYPE_LIST ? dict->GetList("pages") : nullptr;
    for (size_t i = 0; pages && i < pages->GetSize(); ++i)
    {
        if (pages->GetType(i) != VTYPE_DICTIONARY)
            continue;
        const auto page = pages->GetDictionary(i);
        Page parsed{Str(page, "name"), {}};
        const auto items = page->GetType("items") == VTYPE_LIST ? page->GetList("items") : nullptr;
        for (size_t j = 0; items && j < items->GetSize(); ++j)
        {
            if (items->GetType(j) == VTYPE_DICTIONARY)
                parsed.items.push_back(ParseItem(items->GetDictionary(j)));
        }
        s.pages.push_back(std::move(parsed));
    }
    return true;
}

// "show": a guest on someone else's server (the host keeps the world's weather and time), the Winter of Atom prequel
bool Shown(const Item& acItem) noexcept
{
    if (acItem.show.empty())
        return true;
    const bool guest = World::Get().GetTransport().IsConnected() && !IsHostingWorld();
    const bool prequel = Updater::GetCampaign() == "winter-of-atom";
    for (size_t start = 0; start <= acItem.show.size();)
    {
        const size_t plus = std::min(acItem.show.find('+', start), acItem.show.size());
        const std::string condition = acItem.show.substr(start, plus - start);
        if ((condition == "host" && guest) || (condition == "guest" && !guest) || (condition == "2287" && prequel) || (condition == "prequel" && !prequel))
            return false;
        start = plus + 1;
    }
    return true;
}

// The source's value, as the entry shows it: a dropdown's option whose value the source has (the nearest); a stepper's
// new value until its global has caught up
void ReadValue(Item& aItem) noexcept
{
    double value = 0.0;
    if (const TESGlobal* pGlobal = GlobalOf(aItem))
        value = pGlobal->f;
    else if (!aItem.own.empty())
        value = s.own[aItem.own];
    else
        return;

    if (!aItem.values.empty())
    {
        size_t nearest = 0;
        for (size_t i = 1; i < aItem.values.size(); ++i)
        {
            if (std::abs(aItem.values[i] - value) < std::abs(aItem.values[nearest] - value))
                nearest = i;
        }
        value = static_cast<double>(nearest);
    }
    if (aItem.target)
    {
        if (std::abs(value - *aItem.target) < 0.001)
            aItem.target.reset();
        else
            value = *aItem.target;
    }
    aItem.value = value;
}

Page* CurrentPage() noexcept
{
    return s.page >= 0 && s.page < static_cast<int>(s.pages.size()) ? &s.pages[s.page] : nullptr;
}

void ReadPage() noexcept
{
    if (Page* pPage = CurrentPage())
    {
        for (auto& item : pPage->items)
            ReadValue(item);
    }
}

// What the entry's value is for its source: a dropdown's option as its value
double SourceValue(const Item& acItem, double aShown) noexcept
{
    if (acItem.values.empty())
        return aShown;
    const size_t option = static_cast<size_t>(std::max(0.0, aShown));
    return acItem.values[std::min(option, acItem.values.size() - 1)];
}

// A call's arguments: booleans; numbers (an int when whole); "{value}" the entry's value for its source, "{name}" one of
// the page's own values (ints)
std::vector<PapyrusCall::Argument> Arguments(const Item& acItem, const Call& acCall) noexcept
{
    std::vector<PapyrusCall::Argument> arguments;
    for (size_t i = 0; acCall.params && i < acCall.params->GetSize(); ++i)
    {
        const auto param = acCall.params->GetValue(i);
        switch (param->GetType())
        {
        case VTYPE_BOOL: arguments.emplace_back(param->GetBool()); break;
        case VTYPE_INT: arguments.emplace_back(static_cast<int32_t>(param->GetInt())); break;
        case VTYPE_DOUBLE:
        {
            const double number = param->GetDouble();
            if (number == std::floor(number))
                arguments.emplace_back(static_cast<int32_t>(number));
            else
                arguments.emplace_back(static_cast<float>(number));
            break;
        }
        case VTYPE_STRING:
        {
            const std::string text = param->GetString().ToString();
            const std::string name = text.size() > 2 && text.front() == '{' && text.back() == '}' ? text.substr(1, text.size() - 2) : std::string{};
            if (name == "value")
                arguments.emplace_back(static_cast<int32_t>(std::lround(SourceValue(acItem, acItem.value))));
            else if (s.own.contains(name))
                arguments.emplace_back(static_cast<int32_t>(std::lround(s.own[name])));
            else
                spdlog::warn("World settings: \"{}\" isn't a value this page has", text);
            break;
        }
        default: break;
        }
    }
    return arguments;
}

void RunCall(const Item& acItem) noexcept
{
    if (!acItem.call)
        return;
    const Call& call = *acItem.call;
    const auto arguments = Arguments(acItem, call);
    bool sent = false;
    if (call.quest.empty())
        sent = PapyrusCall::Static(call.script.c_str(), call.function.c_str(), arguments);
    else
        sent = PapyrusCall::OnQuest(FormOf(call.quest), call.script.c_str(), call.function.c_str(), arguments);
    spdlog::info("World settings: {} {}.{} ({} arguments)", sent ? "called" : "couldn't call", call.script, call.function, arguments.size());
    s.frames = 0; // read again soon: the script changes values
}

void SetEntry(Item& aItem, double aShown) noexcept
{
    const double before = aItem.value;
    aItem.value = aShown;
    const double value = SourceValue(aItem, aShown);

    if (aItem.stepper)
    {
        const Stepper& stepper = *aItem.stepper;
        const int steps = static_cast<int>(std::lround((aShown - before) / std::max(aItem.step, 0.000001)));
        TESForm* pQuest = FormOf(stepper.quest);
        for (int i = 0; i < std::abs(steps); ++i)
            PapyrusCall::OnQuest(pQuest, stepper.script.c_str(), stepper.function.c_str(), {static_cast<int32_t>(steps > 0 ? stepper.up : stepper.down)});
        aItem.target = aShown;
        spdlog::info("World settings: {} {} step{} of {}", std::abs(steps), steps > 0 ? "up" : "down", std::abs(steps) == 1 ? "" : "s", stepper.function);
    }
    else if (TESGlobal* pGlobal = GlobalOf(aItem))
    {
        pGlobal->f = static_cast<float>(value);
        spdlog::info("World settings: {} = {} ({})", aItem.global, value, aItem.text);
    }
    else if (!aItem.own.empty())
    {
        s.own[aItem.own] = value;
        SaveOwn();
    }
    RunCall(aItem);
}

void PressEntry(const Item& acItem) noexcept
{
    if (!acItem.resetOwn.empty())
    {
        for (const auto& name : acItem.resetOwn)
        {
            if (s.ownDefaults.contains(name))
                s.own[name] = s.ownDefaults[name];
        }
        SaveOwn();
    }
    RunCall(acItem);
}

std::string Number(double aValue) noexcept
{
    return std::isfinite(aValue) ? fmt::format("{}", aValue) : "0";
}

// The open page as the overlay draws it: the page names, then the shown entries with their index in the page
std::string BuildState() noexcept
{
    std::string pages;
    for (const auto& page : s.pages)
        pages += (pages.empty() ? "" : ",") + MiniJson::Quote(page.name);

    std::string items;
    if (const Page* pPage = CurrentPage(); pPage && s.error.empty())
    {
        for (size_t i = 0; i < pPage->items.size(); ++i)
        {
            const Item& item = pPage->items[i];
            if (!Shown(item) || (!item.global.empty() && !GlobalOf(item)))
                continue;
            std::string options;
            for (const auto& option : item.options)
                options += (options.empty() ? "" : ",") + MiniJson::Quote(option);
            items += fmt::format(R"({}{{"i":{},"type":{},"text":{},"help":{},"value":{},"options":[{}],"min":{},"max":{},"step":{}}})", items.empty() ? "" : ",", i,
                                 MiniJson::Quote(item.type), MiniJson::Quote(item.text), MiniJson::Quote(item.help), Number(item.value), options, Number(item.min),
                                 Number(item.max), Number(item.step));
        }
    }
    return fmt::format(R"({{"pages":[{}],"page":{},"error":{},"items":[{}]}})", pages, s.page, MiniJson::Quote(s.error), items);
}

void Send() noexcept
{
    std::string json = BuildState();
    if (json != s.sent)
    {
        Overlay().SendEvent("worldSettings", json);
        s.sent = std::move(json);
    }
}

Item* PageItem(int aIndex) noexcept
{
    Page* pPage = CurrentPage();
    return pPage && aIndex >= 0 && aIndex < static_cast<int>(pPage->items.size()) ? &pPage->items[aIndex] : nullptr;
}
} // namespace

namespace WorldSettings
{
void Run() noexcept
{
    if (!s.open)
        return;
    if (++s.frames >= 20)
    {
        s.frames = 0;
        ReadPage();
    }
    Send();
}

void OnAction(const std::string& acAction, const std::string& acPayload) noexcept
{
    if (acAction == "open")
    {
        Load();
        s.open = true;
        s.sent.clear();
        s.page = std::clamp(s.page, 0, std::max(0, static_cast<int>(s.pages.size()) - 1));
        ReadPage();
        Send();
        spdlog::info("World settings: opened ({} pages)", s.pages.size());
        return;
    }
    if (!s.open)
        return;
    if (acAction == "close")
    {
        s.open = false;
        s.sent.clear();
        Overlay().SendEvent("worldSettings", "null");
        return;
    }
    if (acAction == "page")
    {
        s.page = std::clamp(std::atoi(acPayload.c_str()), 0, std::max(0, static_cast<int>(s.pages.size()) - 1));
        ReadPage();
        Send();
        return;
    }

    // "entry" or "entry:value"
    const size_t colon = acPayload.find(':');
    Item* pItem = s.error.empty() ? PageItem(std::atoi(acPayload.c_str())) : nullptr;
    if (!pItem || !Shown(*pItem))
        return;
    if (acAction == "set" && colon != std::string::npos)
        SetEntry(*pItem, std::atof(acPayload.c_str() + colon + 1));
    else if (acAction == "press")
        PressEntry(*pItem);
    else
        spdlog::warn("World settings: {} {} not done", acAction, acPayload);
    Send();
}
} // namespace WorldSettings
