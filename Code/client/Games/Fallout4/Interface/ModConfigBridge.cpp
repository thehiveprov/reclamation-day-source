#include <HivemindPCH.h>

#include <Interface/ModConfigBridge.h>

#include <Interface/Scaleform.h>
#include <MiniJson.h>
#include <Services/OverlayService.h>
#include <World.h>

#include <include/cef_parser.h>

#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>

// MCM is mcm.dll (reg2k's f4mcm) plus MCM.swf, which it loads into MainMenu.swf. mcm.dll puts its native object on the
// movie's root as root.mcm (GetModSettingInt, SetKeybind, CallQuestFunction, ...) and keeps the settings (each mod's
// MCM\Config\<mod>\settings.ini under the player's MCM\Settings\<mod>.ini, written back on every Set) and the keybinds
// (MCM\Settings\Keybinds.json, written by OnMCMClose). MCM.swf reads each config.json and drives those natives; this
// does the same for the overlay, following MCM.swf's ActionScript (re\mcm): readValue, OptionsList.onValueChange,
// parseParams, the filterer's groups. Facts and the format in docs/MENUS.md.
namespace
{
constexpr char kNative[] = "root.mcm";
constexpr char kF4se[] = "root.f4se";

// How MCM.swf hands a setting's value to mcm.dll, by its source: the natives check the Scaleform type (an int that
// arrives as a Number is ignored)
enum class Kind
{
    kString,
    kNumber,
    kInt,
    kBool,
};

struct Action
{
    std::string type;
    std::string form;
    std::string scriptName;
    std::string script;
    std::string function;
    CefRefPtr<CefListValue> params;
};

// One entry of a page (config.json's content items)
struct Item
{
    std::string type;
    std::string id;
    std::string text;
    std::string help;
    // "center", and a big font (the mods' titles are HTML with <font size='30'>)
    bool centered{false};
    bool big{false};
    std::string sourceType;
    std::string sourceForm;
    std::string scriptName;
    std::string propertyName;
    std::vector<std::string> options;
    double min{0.0};
    double max{1.0};
    double step{1.0};
    bool allowModifiers{true};
    // groupControl: a switcher that turns its group's bit on; groupCondition: the bits an entry needs to show
    int groupControl{-1};
    std::string groupOperator;
    uint32_t groupFlag{1};
    std::optional<Action> action;
    bool refreshMenu{false};

    // From the game
    double value{0.0};
    std::string valueString;
    int keycode{0};
    int modifiers{0};
};

struct Page
{
    // empty for the mod's own page (config.json's "content")
    std::string name;
    std::vector<Item> items;
};

struct Mod
{
    std::string folder;
    std::string modName;
    std::string displayName;
    // why its pages can't be used (a plugin it needs, a newer MCM)
    std::string error;
    std::vector<Page> pages;
};

struct Conflict
{
    int item;
    int keycode;
    int modifiers;
    std::string with;
};

struct Bridge
{
    GFx::Movie* pMovie{nullptr};
    bool open{false};
    std::vector<Mod> mods;
    int mod{-1};
    int page{-1};
    // a key another mod's keybind has, waiting for the player's yes
    std::optional<Conflict> conflict;
    // frames until the page's values are read again (an action's script runs a moment later)
    int reread{0};
    bool loggedTranslation{false};
    std::unordered_map<std::string, std::string> translations;
    std::string sent;
};
// Never destroyed: at exit the menu's movie may already be gone and releasing its objects would crash
Bridge& s = *new Bridge;

OverlayService& Overlay() noexcept
{
    return World::Get().ctx().at<OverlayService>();
}

GFx::Value Get(const char* acpPath) noexcept
{
    GFx::Value value;
    if (s.pMovie)
        s.pMovie->GetVariable(&value, acpPath);
    return value;
}

// One of mcm.dll's natives
GFx::Value Native(const char* acpName, std::initializer_list<GFx::Value> aArgs = {}) noexcept
{
    GFx::Value native = Get(kNative);
    GFx::Value result;
    if (!native.IsObject() || !native.Invoke(acpName, &result, aArgs.begin(), aArgs.size()))
        spdlog::warn("Mod config: MCM's {} not called", acpName);
    return result;
}

GFx::Value Native(const char* acpName, const std::vector<GFx::Value>& acArgs) noexcept
{
    GFx::Value native = Get(kNative);
    GFx::Value result;
    if (!native.IsObject() || !native.Invoke(acpName, &result, acArgs.data(), acArgs.size()))
        spdlog::warn("Mod config: MCM's {} not called", acpName);
    return result;
}

std::string String(const GFx::Value& acValue) noexcept
{
    const char* pText = acValue.GetString();
    return pText ? pText : "";
}

std::string StringMember(const GFx::Value& acObject, const char* acpName) noexcept
{
    GFx::Value value;
    return acObject.GetMember(acpName, &value) ? String(value) : std::string{};
}

// F4SE's SendExternalEvent on the movie's root: Papyrus scripts registered with RegisterForExternalEvent get it
void SendExternalEvent(std::initializer_list<GFx::Value> aArgs) noexcept
{
    GFx::Value f4se = Get(kF4se);
    if (!f4se.IsObject() || !f4se.Invoke("SendExternalEvent", nullptr, aArgs.begin(), aArgs.size()))
        spdlog::warn("Mod config: F4SE's SendExternalEvent not called");
}

// A "$KEY" text in the game's language. MCM.swf's Translator: a TextField given the key shows its translation (the
// game's translator has every plugin's Interface\Translations\<plugin>_<language>.txt); a key it doesn't know shows
// without its "$"
std::string Translate(const std::string& acText) noexcept
{
    if (!acText.starts_with('$') || !s.pMovie)
        return acText;
    if (const auto it = s.translations.find(acText); it != s.translations.end())
        return it->second;

    std::string result = acText;
    GFx::Value field, text;
    s.pMovie->CreateObject(&field, "flash.text.TextField");
    if (field.IsObject() && field.SetMember("text", GFx::Value(acText.c_str())) && field.GetMember("text", &text) && text.GetString())
        result = text.GetString();
    if (!std::exchange(s.loggedTranslation, true))
        spdlog::info("Mod config: {} translates to \"{}\"", acText, result);
    if (result.starts_with('$'))
        result.erase(0, 1);
    s.translations.emplace(acText, result);
    return result;
}

std::string Lower(std::string aText) noexcept
{
    std::ranges::transform(aText, aText.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return aText;
}

// MCM's texts can be HTML (the mods' titles: <p align='center'><font size='30'>...); the overlay shows text, with
// the line breaks, centring and a big font kept as hints
std::string PlainText(std::string_view aHtml, bool* apCentered = nullptr, bool* apBig = nullptr) noexcept
{
    std::string out;
    for (size_t i = 0; i < aHtml.size(); ++i)
    {
        const char c = aHtml[i];
        if (c == '<')
        {
            const size_t end = aHtml.find('>', i);
            if (end == std::string_view::npos)
                break;
            const std::string tag = Lower(std::string(aHtml.substr(i + 1, end - i - 1)));
            if (tag.starts_with("br") || tag == "/p")
                out += '\n';
            if (apCentered && (tag.find("align='center'") != std::string::npos || tag.find("align=\"center\"") != std::string::npos))
                *apCentered = true;
            if (const size_t size = tag.find("size="); apBig && size != std::string::npos)
            {
                const char* pSize = tag.c_str() + size + 5;
                if (*pSize == '\'' || *pSize == '"')
                    ++pSize;
                *apBig = *apBig || std::atoi(pSize) >= 25;
            }
            i = end;
            continue;
        }
        if (c == '&')
        {
            constexpr std::pair<std::string_view, char> kEntities[] = {{"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&#39;", '\''}, {"&apos;", '\''}, {"&nbsp;", ' '}};
            bool replaced = false;
            for (const auto& [entity, character] : kEntities)
            {
                if (aHtml.substr(i, entity.size()) == entity)
                {
                    out += character;
                    i += entity.size() - 1;
                    replaced = true;
                    break;
                }
            }
            if (replaced)
                continue;
        }
        out += c;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == ' '))
        out.pop_back();
    return out;
}

// A text as the overlay shows it: the tags out, then the key translated
std::string ShownText(const std::string& acText, bool* apCentered = nullptr, bool* apBig = nullptr) noexcept
{
    const std::string plain = acText.find('<') != std::string::npos ? PlainText(acText, apCentered, apBig) : acText;
    return Translate(plain);
}

// config.json's values

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
    case VTYPE_STRING: return std::atof(acValue->GetString().ToString().c_str());
    default: return aDefault;
    }
}

double Num(const CefRefPtr<CefDictionaryValue>& acDict, const char* acpKey, double aDefault) noexcept
{
    return acDict && acDict->HasKey(acpKey) ? Num(acDict->GetValue(acpKey), aDefault) : aDefault;
}

// A groupCondition's bits: a group number, or a list of them (each group n is the bit 2^n)
uint32_t GroupBits(const CefRefPtr<CefValue>& acValue) noexcept
{
    if (!acValue)
        return 0;
    if (acValue->GetType() == VTYPE_LIST)
    {
        uint32_t bits = 0;
        const auto list = acValue->GetList();
        for (size_t i = 0; i < list->GetSize(); ++i)
            bits |= 1u << (static_cast<int>(Num(list->GetValue(i), 0.0)) & 31);
        return bits;
    }
    return 1u << (static_cast<int>(Num(acValue, 0.0)) & 31);
}

Item ParseItem(const CefRefPtr<CefDictionaryValue>& acEntry) noexcept
{
    Item item;
    item.type = Str(acEntry, "type");
    item.id = Str(acEntry, "id");
    const std::string text = Str(acEntry, "text");
    item.text = ShownText(text, &item.centered, &item.big);
    item.centered = item.centered || Str(acEntry, "align") == "center";
    item.help = ShownText(Str(acEntry, "help"));
    item.refreshMenu = acEntry->GetType("refreshMenu") == VTYPE_BOOL && acEntry->GetBool("refreshMenu");

    if (acEntry->GetType("valueOptions") == VTYPE_DICTIONARY)
    {
        const auto options = acEntry->GetDictionary("valueOptions");
        item.sourceType = Str(options, "sourceType");
        item.sourceForm = Str(options, "sourceForm");
        item.scriptName = Str(options, "scriptName");
        item.propertyName = Str(options, "propertyName");
        item.min = Num(options, "min", 0.0);
        item.max = Num(options, "max", 1.0);
        item.step = Num(options, "step", 1.0);
        if (options->GetType("allowModifierKeys") == VTYPE_BOOL)
            item.allowModifiers = options->GetBool("allowModifierKeys");
        if (options->GetType("options") == VTYPE_LIST)
        {
            const auto list = options->GetList("options");
            for (size_t i = 0; i < list->GetSize(); ++i)
                item.options.push_back(list->GetType(i) == VTYPE_STRING ? ShownText(list->GetString(i).ToString()) : fmt::format("{}", Num(list->GetValue(i), 0.0)));
        }
    }

    if (acEntry->HasKey("groupControl"))
        item.groupControl = static_cast<int>(Num(acEntry, "groupControl", -1.0));
    if (acEntry->HasKey("groupCondition"))
    {
        const auto condition = acEntry->GetValue("groupCondition");
        item.groupOperator = "OR";
        item.groupFlag = 0;
        if (condition->GetType() == VTYPE_DICTIONARY)
        {
            const auto dict = condition->GetDictionary();
            for (const char* op : {"OR", "AND", "ONLY"})
            {
                if (dict->HasKey(op))
                {
                    item.groupOperator = op;
                    item.groupFlag = GroupBits(dict->GetValue(op));
                }
            }
        }
        else
            item.groupFlag = GroupBits(condition);
    }

    if (acEntry->GetType("action") == VTYPE_DICTIONARY)
    {
        const auto action = acEntry->GetDictionary("action");
        Action parsed{Str(action, "type"), Str(action, "form"), Str(action, "scriptName"), Str(action, "script"), Str(action, "function"), nullptr};
        if (action->GetType("params") == VTYPE_LIST)
            parsed.params = action->GetList("params");
        item.action = std::move(parsed);
    }
    return item;
}

std::vector<Item> ParseContent(const CefRefPtr<CefListValue>& acContent) noexcept
{
    std::vector<Item> items;
    for (size_t i = 0; acContent && i < acContent->GetSize(); ++i)
    {
        if (acContent->GetType(i) == VTYPE_DICTIONARY)
            items.push_back(ParseItem(acContent->GetDictionary(i)));
    }
    return items;
}

std::string Utf8(const std::filesystem::path& acPath) noexcept
{
    const std::u8string text = acPath.u8string();
    return {text.begin(), text.end()};
}

std::string ReadFile(const std::filesystem::path& acPath) noexcept
{
    std::ifstream file(acPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.starts_with("\xEF\xBB\xBF"))
        text.erase(0, 3);
    return text;
}

// A mod's config.json: its names, what it needs (MCM.swf's checkreqs) and its pages
Mod LoadMod(const std::filesystem::path& acFolder) noexcept
{
    Mod mod;
    mod.folder = Utf8(acFolder.filename());
    mod.modName = mod.folder;
    mod.displayName = mod.folder;

    const CefRefPtr<CefValue> json = CefParseJSON(CefString(ReadFile(acFolder / L"config.json")), JSON_PARSER_ALLOW_TRAILING_COMMAS);
    if (!json || json->GetType() != VTYPE_DICTIONARY)
    {
        mod.error = "Its config.json can't be read.";
        spdlog::warn("Mod config: {}\\config.json can't be read", mod.folder);
        return mod;
    }
    const auto config = json->GetDictionary();
    if (const std::string name = Str(config, "modName"); !name.empty())
        mod.modName = name;
    if (const std::string name = Str(config, "displayName"); !name.empty())
        mod.displayName = ShownText(name);

    const double version = Num(config, "minMcmVersion", 0.0);
    if (version > Native("GetMCMVersionCode").GetNumber())
        mod.error = "It needs a newer Mod Configuration Menu.";
    if (config->GetType("pluginRequirements") == VTYPE_LIST)
    {
        std::string missing;
        const auto plugins = config->GetList("pluginRequirements");
        for (size_t i = 0; i < plugins->GetSize(); ++i)
        {
            const std::string plugin = plugins->GetString(i).ToString();
            if (!plugin.empty() && Native("IsPluginInstalled", {GFx::Value(plugin.c_str())}).GetNumber() == 0.0)
                missing += (missing.empty() ? "" : ", ") + plugin;
        }
        if (!missing.empty())
            mod.error = "It needs " + missing + ".";
    }

    // the mod's own page, unless it's empty and the mod has named pages (Reclamation Day's "content": [])
    const auto content = config->GetType("content") == VTYPE_LIST ? config->GetList("content") : nullptr;
    const bool namedPages = config->GetType("pages") == VTYPE_LIST && config->GetList("pages")->GetSize() > 0;
    if (content && (content->GetSize() > 0 || !namedPages))
        mod.pages.push_back(Page{"", ParseContent(content)});
    if (config->GetType("pages") == VTYPE_LIST)
    {
        const auto pages = config->GetList("pages");
        for (size_t i = 0; i < pages->GetSize(); ++i)
        {
            if (pages->GetType(i) != VTYPE_DICTIONARY)
                continue;
            const auto page = pages->GetDictionary(i);
            const auto content = page->GetType("content") == VTYPE_LIST ? page->GetList("content") : nullptr;
            mod.pages.push_back(Page{ShownText(Str(page, "pageDisplayName")), ParseContent(content)});
        }
    }
    return mod;
}

// Every mod with a Data\MCM\Config\<mod>\config.json (mcm.dll's GetConfigList), in MCM's order: its sOrder setting
// (folder names, comma separated) first, the rest by folder name
void LoadMods() noexcept
{
    s.mods.clear();
    std::vector<std::filesystem::path> folders;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::current_path() / L"Data" / L"MCM" / L"Config", error))
    {
        if (entry.is_directory(error) && std::filesystem::exists(entry.path() / L"config.json", error))
            folders.push_back(entry.path());
    }

    std::vector<std::string> order;
    const std::string orderSetting = String(Native("GetModSettingString", {GFx::Value("MCM"), GFx::Value("sOrder:Main")}));
    for (size_t start = 0; start < orderSetting.size();)
    {
        const size_t comma = std::min(orderSetting.find(',', start), orderSetting.size());
        order.push_back(Lower(orderSetting.substr(start, comma - start)));
        start = comma + 1;
    }
    const auto rank = [&order](const std::filesystem::path& acFolder)
    {
        const std::string name = Lower(Utf8(acFolder.filename()));
        const auto it = std::ranges::find(order, name);
        return std::pair{it == order.end() ? order.size() : static_cast<size_t>(it - order.begin()), name};
    };
    std::ranges::sort(folders, [&rank](const auto& acLeft, const auto& acRight) { return rank(acLeft) < rank(acRight); });

    for (const auto& folder : folders)
        s.mods.push_back(LoadMod(folder));
    spdlog::info("Mod config: {} mods with settings", s.mods.size());
}

Mod* CurrentMod() noexcept
{
    return s.mod >= 0 && s.mod < static_cast<int>(s.mods.size()) ? &s.mods[s.mod] : nullptr;
}

Page* CurrentPage() noexcept
{
    Mod* pMod = CurrentMod();
    return pMod && s.page >= 0 && s.page < static_cast<int>(pMod->pages.size()) ? &pMod->pages[s.page] : nullptr;
}

Kind KindOf(const std::string& acSourceType) noexcept
{
    if (acSourceType == "ModSettingInt" || acSourceType == "PropertyValueInt")
        return Kind::kInt;
    if (acSourceType == "ModSettingBool" || acSourceType == "PropertyValueBool")
        return Kind::kBool;
    if (acSourceType == "ModSettingFloat" || acSourceType == "PropertyValueFloat" || acSourceType == "GlobalValue")
        return Kind::kNumber;
    return Kind::kString;
}

// MCM.swf's readValue: booleans as 0 or 1, numbers as they are, strings in valueString; a hotkey's key from its keybind
void ReadValue(const Mod& acMod, Item& aItem) noexcept
{
    const GFx::Value modName(acMod.modName.c_str()), id(aItem.id.c_str()), form(aItem.sourceForm.c_str());
    const GFx::Value script(aItem.scriptName.c_str()), property(aItem.propertyName.c_str());

    if (aItem.type == "hotkey")
    {
        GFx::Value keybind = Native("GetKeybind", {modName, id});
        GFx::Value keycode, modifiers;
        aItem.keycode = keybind.GetMember("keycode", &keycode) ? static_cast<int>(keycode.GetNumber()) : 0;
        aItem.modifiers = keybind.GetMember("modifiers", &modifiers) ? static_cast<int>(modifiers.GetNumber()) : 0;
        return;
    }

    const std::string& source = aItem.sourceType;
    if (source.empty())
        return;

    GFx::Value result;
    if (source.starts_with("ModSetting"))
        result = Native(("Get" + source).c_str(), {modName, id});
    else if (source == "GlobalValue")
        result = Native("GetGlobalValue", {form});
    else if (source.starts_with("PropertyValue"))
        result = aItem.scriptName.empty() ? Native("GetPropertyValue", {form, property}) : Native("GetPropertyValueEx", {form, script, property});
    else
        return;

    switch (KindOf(source))
    {
    case Kind::kString: aItem.valueString = result.GetType() == GFx::ValueType::kString ? String(result) : fmt::format("{}", result.GetNumber()); break;
    case Kind::kBool: aItem.value = result.GetNumber() != 0.0 ? 1.0 : 0.0; break;
    default: aItem.value = result.GetNumber(); break;
    }
}

void ReadPage() noexcept
{
    Mod* pMod = CurrentMod();
    Page* pPage = CurrentPage();
    if (!pMod || !pPage || !pMod->error.empty())
        return;
    for (auto& item : pPage->items)
        ReadValue(*pMod, item);
}

// The groups' bits: bit 0 always (entries without a group), then each switcher with a groupControl
uint32_t GroupFlags(const Page& acPage) noexcept
{
    uint32_t flags = 1;
    for (const auto& item : acPage.items)
    {
        if (item.groupControl < 0 || item.groupControl > 31)
            continue;
        if (item.value == 1.0)
            flags |= 1u << item.groupControl;
        else if (item.value == 0.0)
            flags &= ~(1u << item.groupControl);
    }
    return flags;
}

// ListFiltererEx.EntryMatchesFilter
bool Shown(const Item& acItem, uint32_t aFlags) noexcept
{
    if (acItem.groupOperator == "AND")
        return (acItem.groupFlag & aFlags) == acItem.groupFlag;
    if (acItem.groupOperator == "ONLY")
        return acItem.groupFlag == (aFlags & ~1u);
    return (acItem.groupFlag & aFlags) != 0;
}

std::string Number(double aValue) noexcept
{
    return std::isfinite(aValue) ? fmt::format("{}", aValue) : "0";
}

std::string BuildState() noexcept
{
    std::string json = R"({"mods":[)";
    for (size_t i = 0; i < s.mods.size(); ++i)
    {
        const Mod& mod = s.mods[i];
        std::string pages;
        for (const auto& page : mod.pages)
            pages += (pages.empty() ? "" : ",") + MiniJson::Quote(page.name);
        json += fmt::format(R"({}{{"name":{},"pages":[{}],"error":{}}})", i ? "," : "", MiniJson::Quote(mod.displayName), pages, MiniJson::Quote(mod.error));
    }
    json += fmt::format(R"(],"mod":{},"page":{},"items":[)", s.mod, s.page);

    if (const Page* pPage = CurrentPage(); pPage && CurrentMod()->error.empty())
    {
        const uint32_t flags = GroupFlags(*pPage);
        bool first = true;
        for (size_t i = 0; i < pPage->items.size(); ++i)
        {
            const Item& item = pPage->items[i];
            if (!Shown(item, flags))
                continue;
            std::string options;
            for (const auto& option : item.options)
                options += (options.empty() ? "" : ",") + MiniJson::Quote(option);
            json += fmt::format(R"({}{{"i":{},"type":{},"text":{},"help":{},"centered":{},"big":{},"value":{},"valueString":{},"options":[{}],"min":{},"max":{},"step":{},"keycode":{},"modifiers":{},"allowModifiers":{}}})",
                                first ? "" : ",", i, MiniJson::Quote(item.type), MiniJson::Quote(item.text), MiniJson::Quote(item.help), item.centered, item.big, Number(item.value),
                                MiniJson::Quote(item.valueString), options, Number(item.min), Number(item.max), Number(item.step), item.keycode, item.modifiers,
                                item.allowModifiers);
            first = false;
        }
    }
    json += "]";

    if (s.conflict)
        json += fmt::format(R"(,"conflict":{{"item":{},"keycode":{},"modifiers":{},"with":{}}})", s.conflict->item, s.conflict->keycode, s.conflict->modifiers,
                            MiniJson::Quote(s.conflict->with));
    return json + "}";
}

// The value MCM.swf gives a setting's source, by the source's kind
GFx::Value TypedValue(Kind aKind, double aValue, const std::string& acText) noexcept
{
    switch (aKind)
    {
    case Kind::kInt: return GFx::Value(static_cast<int32_t>(aValue));
    case Kind::kBool: return GFx::Value(aValue != 0.0);
    case Kind::kNumber: return GFx::Value(aValue);
    default: return GFx::Value(acText.c_str());
    }
}

// OptionsList.onValueChange: the value to its source
void WriteValue(const Mod& acMod, const Item& acItem) noexcept
{
    const std::string& source = acItem.sourceType;
    const Kind kind = KindOf(source);
    const std::string text = kind == Kind::kString ? acItem.valueString : Number(acItem.value);
    const GFx::Value value = TypedValue(kind, acItem.value, text);
    const GFx::Value modName(acMod.modName.c_str()), id(acItem.id.c_str()), form(acItem.sourceForm.c_str());
    const GFx::Value script(acItem.scriptName.c_str()), property(acItem.propertyName.c_str());

    if (source == "GlobalValue")
        Native("SetGlobalValue", {form, GFx::Value(acItem.value)});
    else if (source.starts_with("ModSetting"))
        Native(("Set" + source).c_str(), {modName, id, value});
    else if (source.starts_with("PropertyValue"))
    {
        if (acItem.scriptName.empty())
            Native("SetPropertyValue", {form, property, value});
        else
            Native("SetPropertyValueEx", {form, script, property, value});
    }
}

// An action's parameters, MCM.swf's parseParams: "{value}" is the value as its source's kind; "{i}", "{b}" and "{f}"
// in a text make it an int, a boolean or a number once "{value}" in it is the value; numbers and booleans as they are
std::vector<GFx::Value> ActionParams(const Item& acItem, bool aWithValue, std::deque<std::string>& aTexts) noexcept
{
    std::vector<GFx::Value> params;
    const auto& list = acItem.action->params;
    const Kind kind = KindOf(acItem.sourceType);
    const std::string valueText = kind == Kind::kString ? acItem.valueString : Number(acItem.value);
    const auto replaceValue = [&valueText](std::string aText, std::string_view aMarker)
    {
        if (const size_t at = aText.find(aMarker); at != std::string::npos)
            aText.erase(at, aMarker.size());
        if (const size_t at = aText.find("{value}"); at != std::string::npos)
            aText.replace(at, 7, valueText);
        return aText;
    };

    for (size_t i = 0; list && i < list->GetSize(); ++i)
    {
        const auto param = list->GetValue(i);
        switch (param->GetType())
        {
        case VTYPE_BOOL: params.emplace_back(param->GetBool()); break;
        case VTYPE_INT: params.emplace_back(static_cast<int32_t>(param->GetInt())); break;
        case VTYPE_DOUBLE:
        {
            // ActionScript keeps a whole number as an int
            const double number = param->GetDouble();
            if (number == std::floor(number) && std::abs(number) < 2147483647.0)
                params.emplace_back(static_cast<int32_t>(number));
            else
                params.emplace_back(number);
            break;
        }
        case VTYPE_STRING:
        {
            const std::string text = param->GetString().ToString();
            if (!aWithValue)
                params.emplace_back(aTexts.emplace_back(text).c_str());
            else if (text == "{value}")
                params.push_back(kind == Kind::kString ? GFx::Value(aTexts.emplace_back(valueText).c_str()) : TypedValue(kind, acItem.value, valueText));
            else if (text.find("{i}") != std::string::npos)
                params.emplace_back(static_cast<int32_t>(std::atoi(replaceValue(text, "{i}").c_str())));
            else if (text.find("{b}") != std::string::npos)
                params.emplace_back(std::atoi(replaceValue(text, "{b}").c_str()) != 0);
            else if (text.find("{f}") != std::string::npos)
                params.emplace_back(std::atof(replaceValue(text, "{f}").c_str()));
            else
                params.emplace_back(aTexts.emplace_back(replaceValue(text, "")).c_str());
            break;
        }
        default: break;
        }
    }
    return params;
}

// An entry's action (a switcher's after its value, a button's on its press): a function on a quest's script or a
// global one
void RunAction(const Item& acItem, bool aWithValue) noexcept
{
    if (!acItem.action)
        return;
    const Action& action = *acItem.action;
    std::deque<std::string> texts;
    std::vector<GFx::Value> params = ActionParams(acItem, aWithValue, texts);

    std::vector<GFx::Value> args;
    if (action.type == "CallFunction")
        args = {GFx::Value(action.form.c_str()), GFx::Value(action.scriptName.c_str()), GFx::Value(action.function.c_str())};
    else if (action.type == "CallGlobalFunction")
        args = {GFx::Value(action.script.c_str()), GFx::Value(action.function.c_str())};
    else
    {
        spdlog::warn("Mod config: action {} isn't supported", action.type);
        return;
    }
    args.insert(args.end(), params.begin(), params.end());
    Native(action.type == "CallFunction" ? "CallQuestFunction" : "CallGlobalFunction", args);
    spdlog::info("Mod config: {} {} on {}{}", action.type, action.function, action.form.empty() ? action.script : action.form,
                 action.scriptName.empty() ? "" : " (" + action.scriptName + ")");
    // the script runs a moment later: its changes are read then
    s.reread = 20;
}

// The setting changed: Papyrus scripts listening for OnMCMSettingChange (all mods, or just this one) hear it
void SettingChanged(const Mod& acMod, const Item& acItem) noexcept
{
    if (acItem.id.empty())
        return;
    const std::string perMod = "OnMCMSettingChange|" + acMod.modName;
    SendExternalEvent({GFx::Value("OnMCMSettingChange"), GFx::Value(acMod.modName.c_str()), GFx::Value(acItem.id.c_str())});
    SendExternalEvent({GFx::Value(perMod.c_str()), GFx::Value(acMod.modName.c_str()), GFx::Value(acItem.id.c_str())});
}

Item* PageItem(int aIndex) noexcept
{
    Page* pPage = CurrentPage();
    return pPage && aIndex >= 0 && aIndex < static_cast<int>(pPage->items.size()) ? &pPage->items[aIndex] : nullptr;
}

// A hotkey's new key (0: none), as Option_ButtonMapping does it: cleared, remapped from its old key or set; MCM keeps
// one keybind per key, so a key another mod had is that mod's no more. OnMCMClose writes Keybinds.json.
void AssignKey(const Mod& acMod, Item& aItem, int aKeycode, int aModifiers) noexcept
{
    const GFx::Value modName(acMod.modName.c_str()), id(aItem.id.c_str());
    if (aKeycode == 0)
        Native("ClearKeybind", {modName, id});
    else if (aItem.keycode != 0)
        Native("RemapKeybind", {modName, id, GFx::Value(static_cast<int32_t>(aKeycode)), GFx::Value(static_cast<int32_t>(aModifiers))});
    else
        Native("SetKeybind", {modName, id, GFx::Value(static_cast<int32_t>(aKeycode)), GFx::Value(static_cast<int32_t>(aModifiers))});
    Native("OnMCMClose");
    spdlog::info("Mod config: {} {} key {} (modifiers {})", acMod.modName, aItem.id, aKeycode, aModifiers);
    ReadPage();
}

// Another mod's (or another entry's) keybind on that key, as "its name (its mod)"; empty if the key is free
std::string KeyTakenBy(const Mod& acMod, const Item& acItem, int aKeycode, int aModifiers) noexcept
{
    GFx::Value keybind = Native("GetKeybind", {GFx::Value(static_cast<int32_t>(aKeycode)), GFx::Value(static_cast<int32_t>(aModifiers))});
    const std::string otherId = StringMember(keybind, "keybindID");
    const std::string otherMod = StringMember(keybind, "modName");
    if (otherId.empty() || (otherMod == acMod.modName && otherId == acItem.id))
        return {};
    const std::string name = StringMember(keybind, "keybindName");
    return fmt::format("{} ({})", name.empty() ? otherId : Translate(name), otherMod);
}

void Select(int aMod, int aPage) noexcept
{
    s.conflict.reset();
    s.mod = aMod;
    s.page = aPage;
    Mod* pMod = CurrentMod();
    if (pMod && (s.page < 0 || s.page >= static_cast<int>(pMod->pages.size())))
        s.page = pMod->pages.empty() ? -1 : 0;
    ReadPage();
}

void Open() noexcept
{
    if (s.open || !s.pMovie)
        return;
    if (!Get(kNative).IsObject())
    {
        spdlog::warn("Mod config: MCM isn't in this menu (no root.mcm)");
        return;
    }
    LoadMods();
    s.open = true;
    s.sent.clear();
    Select(s.mods.empty() ? -1 : 0, 0);
    SendExternalEvent({GFx::Value("OnMCMOpen")});
    spdlog::info("Mod config: opened");
}

void Close() noexcept
{
    if (!s.open)
        return;
    s.open = false;
    s.conflict.reset();
    s.sent.clear();
    if (s.pMovie)
    {
        SendExternalEvent({GFx::Value("OnMCMClose")});
        Native("OnMCMClose");
    }
    Overlay().SendEvent("modConfig", "null");
    spdlog::info("Mod config: closed");
}
} // namespace

bool ModConfigBridge::IsOpen() noexcept
{
    return s.open;
}

void ModConfigBridge::OnPageRestarted() noexcept
{
    s.sent.clear();
}

void ModConfigBridge::Run(GFx::Movie* apMovie) noexcept
{
    if (apMovie != s.pMovie)
    {
        // the menu closed: its objects are gone with it (keybinds were written as they changed)
        if (s.open)
        {
            s.pMovie = nullptr;
            Close();
        }
        s.pMovie = apMovie;
        s.translations.clear();
    }
    if (!s.open)
        return;

    if (s.reread > 0 && --s.reread == 0)
        ReadPage();

    std::string json = BuildState();
    if (json != s.sent)
    {
        Overlay().SendEvent("modConfig", json);
        s.sent = std::move(json);
    }
}

void ModConfigBridge::OnAction(const std::string& acAction, const std::string& acPayload) noexcept
{
    if (!s.pMovie)
        return;
    if (acAction == "open")
    {
        Open();
        return;
    }
    if (!s.open)
        return;

    // "a:b:c" fields; a text's value is everything after the entry's number
    const auto colon = acPayload.find(':');
    const int first = std::atoi(acPayload.c_str());
    const std::string rest = colon == std::string::npos ? std::string{} : acPayload.substr(colon + 1);

    Mod* pMod = CurrentMod();
    if (acAction == "close")
        Close();
    else if (acAction == "page")
        Select(first, std::atoi(rest.c_str()));
    else if (Item* pItem = PageItem(first); pItem && pMod && pMod->error.empty())
    {
        if (acAction == "set" || acAction == "text")
        {
            if (acAction == "set")
                pItem->value = std::atof(rest.c_str());
            else
                pItem->valueString = rest;
            WriteValue(*pMod, *pItem);
            RunAction(*pItem, true);
            SettingChanged(*pMod, *pItem);
            ReadPage();
        }
        else if (acAction == "press")
            RunAction(*pItem, false);
        else if (acAction == "key" || acAction == "forceKey")
        {
            // "item:keycode:modifiers"
            const auto second = rest.find(':');
            const int keycode = std::atoi(rest.c_str());
            const int modifiers = pItem->allowModifiers && second != std::string::npos ? std::atoi(rest.c_str() + second + 1) : 0;
            const std::string takenBy = acAction == "key" && keycode != 0 ? KeyTakenBy(*pMod, *pItem, keycode, modifiers) : std::string{};
            s.conflict.reset();
            if (!takenBy.empty())
                s.conflict = Conflict{first, keycode, modifiers, takenBy};
            else
                AssignKey(*pMod, *pItem, keycode, modifiers);
        }
        else
            spdlog::warn("Mod config: unknown action {} {}", acAction, acPayload);
    }
    else if (acAction == "cancelKey")
        s.conflict.reset();
    else
        spdlog::warn("Mod config: {} {} not done", acAction, acPayload);
}
