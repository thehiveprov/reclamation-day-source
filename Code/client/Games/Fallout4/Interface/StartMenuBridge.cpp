#include <HivemindPCH.h>

#include <Interface/StartMenuBridge.h>

#include <Interface/IMenu.h>
#include <Interface/ModConfigBridge.h>
#include <Interface/Scaleform.h>
#include <Interface/UI.h>
#include <MiniJson.h>
#include <Services/OverlayService.h>
#include <World.h>

#include <array>

// MainMenu.swf (MCM's copy in Data\Interface is the one the game loads; the pause menu is the same movie in its
// PauseMode) is a state machine on root.Menu_mc: currentState names the screen, each screen a list (BSScrollingList:
// entryList, selectedIndex, the itemPress / selectionChange events) and its texts, and ProcessUserEvent(name, down) is
// its input ("Accept", "Cancel", "XButton", "YButton", "Up", "Down", ...). The overlay draws the screens; the movie,
// hidden, keeps running them. Read from the movie's ActionScript (re\startmenu, docs/MENUS.md).
namespace
{
constexpr char kItemPress[] = "BSScrollingList::itemPress";
constexpr char kSelectionChange[] = "BSScrollingList::selectionChange";

constexpr char kMenu[] = "root.Menu_mc";
constexpr char kState[] = "root.Menu_mc.currentState";
constexpr char kMainList[] = "root.Menu_mc.MainPanel_mc.List_mc";
constexpr char kCategoryList[] = "root.Menu_mc.SettingsPanel_mc.SettingsList_mc";
constexpr char kOptionList[] = "root.Menu_mc.OptionsPanel_mc.Fader_mc.List_mc";
constexpr char kGuideText[] = "root.Menu_mc.OptionsPanel_mc.GuideText_tf";
constexpr char kControlList[] = "root.Menu_mc.ControlsPanel_mc.Fader_mc.List_mc";
constexpr char kRemapPrompt[] = "root.Menu_mc.RemapPrompt_mc";
constexpr char kRemapText[] = "root.Menu_mc.RemapPrompt_mc.textField";
constexpr char kHelpList[] = "root.Menu_mc.HelpPanel_mc.HelpList_mc";
constexpr char kHelpTitle[] = "root.Menu_mc.HelpTitleText";
constexpr char kHelpText[] = "root.Menu_mc.HelpText";
constexpr char kSaveList[] = "root.Menu_mc.SaveLoadHolder_mc.Panel_mc.List_mc";
constexpr char kSavePanel[] = "root.Menu_mc.SaveLoadHolder_mc.Panel_mc";
constexpr char kCharacterList[] = "root.Menu_mc.CharacterSelectList_mc";
constexpr char kConfirmText[] = "root.Menu_mc.ConfirmPanel_mc.Text_mc.TextPulse_mc.textField";

// The screens the overlay draws; the others (credits, add-ons, creations, Bethesda.net, the login) are the game's own
constexpr std::array kCovered{"MAIN_STATE",
                              "CONTINUE_CONFIRM_STATE",
                              "NEW_CONFIRM_STATE",
                              "SAVE_LOAD_STATE",
                              "SAVE_LOAD_CONFIRM_STATE",
                              "QUIT_CONFIRM_STATE",
                              "DELETE_SAVE_CONFIRM_STATE",
                              "SETTINGS_CATEGORY_STATE",
                              "HELP_STATE",
                              "OPTIONS_LISTS_STATE",
                              "DEFAULT_SETTINGS_CONFIRM_STATE",
                              "INPUT_MAPPING_STATE",
                              "CHARACTER_SELECT_STATE"};

// The lists the overlay can act on, by the name it uses
struct NamedList
{
    const char* pName;
    const char* pPath;
};
constexpr std::array kLists{NamedList{"main", kMainList},     NamedList{"category", kCategoryList}, NamedList{"option", kOptionList},
                            NamedList{"control", kControlList}, NamedList{"help", kHelpList},         NamedList{"save", kSaveList},
                            NamedList{"character", kCharacterList}};

struct Bridge
{
    IMenu* pMenu{nullptr};
    GFx::Movie* pMovie{nullptr};
    bool pause{false};
    bool open{false};
    // the game waits for the key to map an action to: the overlay lets input through to it meanwhile
    bool remapping{false};
    // a category clicked on another category's page: pressed once the game is back on its category screen
    int pendingCategory{-1};
    int pendingFrames{0};
    // SAVE pressed on a category's page: the game saves as it leaves its category screen, once it's back there
    bool pendingSave{false};
    int pendingSaveFrames{0};
    // a back press held while the game writes the settings (logged once a save)
    bool heldBack{false};
    // What MainMenu keeps in private vars (bSettingsChanged, bSavingSettings: ABC namespace kind 0x05, which can't be read
    // by name, so they always read false and SAVE never showed), kept here: something changed since the last save (an
    // option of ours the game took, a key mapped, the defaults), and the game writing the settings (its back on the
    // category screen puts "$Saving..." on RemapPrompt_mc, public, until onSettingsSaved blanks it)
    bool changed{false};
    bool saving{false};
    int savingFrames{0};
    int leftFrames{0};
    // the prompt's last word while mapping a key ("That button is reserved." can stay on it): not a save
    std::string remapText;
    std::string sent;
    // a first look at what the lists hold, logged once each (field names the overlay doesn't know yet)
    bool probedSaves{false};
    bool probedOptions{false};
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

// The game's texts are translation keys ("$SETTINGS") or plain text; the overlay gets them without the "$"
std::string Text(const GFx::Value& acValue) noexcept
{
    const char* pText = acValue.GetString();
    std::string text = pText ? pText : "";
    if (text.starts_with('$'))
        text.erase(0, 1);
    return text;
}

std::string TextMember(const GFx::Value& acObject, const char* acpName) noexcept
{
    GFx::Value value;
    return acObject.GetMember(acpName, &value) ? Text(value) : std::string{};
}

double NumberMember(const GFx::Value& acObject, const char* acpName, double aDefault = 0.0) noexcept
{
    GFx::Value value;
    if (!acObject.GetMember(acpName, &value))
        return aDefault;
    const auto type = value.GetType();
    return type == GFx::ValueType::kNumber || type == GFx::ValueType::kInt || type == GFx::ValueType::kUInt || type == GFx::ValueType::kBoolean ? value.GetNumber()
                                                                                                                                                   : aDefault;
}

std::string FieldText(const char* acpPath) noexcept
{
    GFx::Value field = Get(acpPath);
    GFx::Value text;
    return field.GetMember("text", &text) ? Text(text) : std::string{};
}

std::string Number(double aValue) noexcept
{
    return std::isfinite(aValue) ? fmt::format("{:.6g}", aValue) : "0";
}

// The game waits for the key to map: MainMenu.onInputMappingPress turns the controls list's disableSelection on and
// onFinishRemapMode off again, for a key it takes or one it refuses (Escape: "That button is reserved.", the prompt
// staying up with that a moment longer)
bool IsRemapping() noexcept
{
    return NumberMember(Get(kControlList), "disableSelection") != 0.0;
}

// A list's entries as JSON, each from its entry object, with the list's selection
template <class TEntry> std::string ListJson(const char* acpPath, const TEntry& acEntry) noexcept
{
    GFx::Value list = Get(acpPath);
    GFx::Value entries;
    if (!list.GetMember("entryList", &entries))
        return R"({"entries":[],"selected":-1})";

    std::string json = R"({"entries":[)";
    for (uint32_t i = 0; i < entries.GetArraySize(); ++i)
    {
        GFx::Value entry;
        if (!entries.GetMember(std::to_string(i).c_str(), &entry))
            continue;
        json += (i ? "," : "") + acEntry(entry);
    }
    return json + fmt::format(R"(],"selected":{}}})", static_cast<int>(NumberMember(list, "selectedIndex", -1)));
}

std::string MainEntry(const GFx::Value& acEntry) noexcept
{
    return fmt::format(R"({{"text":{},"index":{},"disabled":{}}})", MiniJson::Quote(TextMember(acEntry, "text")), static_cast<int>(NumberMember(acEntry, "index", -1)),
                       NumberMember(acEntry, "disabled") != 0.0 ? "true" : "false");
}

std::string PlainEntry(const GFx::Value& acEntry) noexcept
{
    return fmt::format(R"({{"text":{}}})", MiniJson::Quote(TextMember(acEntry, "text")));
}

// An option: its text, its kind (0 a slider, 1 a stepper over its options, 2 a checkbox), its value and ID
std::string OptionEntry(const GFx::Value& acEntry) noexcept
{
    std::string options;
    GFx::Value list;
    if (acEntry.GetMember("options", &list))
    {
        for (uint32_t i = 0; i < list.GetArraySize(); ++i)
        {
            GFx::Value option;
            if (list.GetMember(std::to_string(i).c_str(), &option))
                options += (options.empty() ? "" : ",") + MiniJson::Quote(Text(option));
        }
    }
    return fmt::format(R"({{"text":{},"type":{},"value":{},"id":{},"options":[{}],"min":{},"max":{},"step":{}}})", MiniJson::Quote(TextMember(acEntry, "text")),
                       static_cast<int>(NumberMember(acEntry, "movieType")), Number(NumberMember(acEntry, "value")), static_cast<int>(NumberMember(acEntry, "ID", -1)),
                       options, Number(NumberMember(acEntry, "minValue", 0.0)), Number(NumberMember(acEntry, "maxValue", 1.0)),
                       Number(NumberMember(acEntry, "stepSize", 0.05)));
}

std::string ControlEntry(const GFx::Value& acEntry) noexcept
{
    return fmt::format(R"({{"text":{},"key":{},"button":{}}})", MiniJson::Quote(TextMember(acEntry, "text")), MiniJson::Quote(TextMember(acEntry, "PCKeyName")),
                       MiniJson::Quote(TextMember(acEntry, "buttonName")));
}

std::string SaveEntry(const GFx::Value& acEntry) noexcept
{
    return fmt::format(R"({{"text":{},"playTime":{}}})", MiniJson::Quote(TextMember(acEntry, "text")), MiniJson::Quote(TextMember(acEntry, "playTime")));
}

// The names an entry's fields may have, logged once for the first entry (what's there decides what the overlay shows)
void ProbeEntry(const char* acpWhat, const char* acpPath) noexcept
{
    GFx::Value list = Get(acpPath), entries, entry;
    if (!list.GetMember("entryList", &entries) || !entries.GetMember("0", &entry))
        return;
    std::string found;
    for (const char* name : {"text", "playTime", "fileName", "filename", "saveName", "name", "playerName", "level", "location", "locationName", "saveTime", "timeStamp",
                             "screenshot", "index", "ID", "movieType", "value", "minValue", "maxValue", "stepSize", "options", "PCKeyName", "buttonName"})
    {
        GFx::Value value;
        if (entry.GetMember(name, &value) && value.GetType() != GFx::ValueType::kUndefined)
            found += fmt::format(" {}={}", name, value.GetString() ? value.GetString() : Number(value.GetNumber()));
    }
    spdlog::info("Start menu: a {} entry has{}", acpWhat, found);
}

// The settings screens: MainMenu's bSettingsChanged (an option changed since the last save) and bSavingSettings (its
// "Saving..." until the game's onSettingsSaved). The game saves only as the settings are left from their category screen
// (onCancelPress there: "$Saving...", SavingSettingsTimer, then BGSCodeObj.SaveSettings)
bool IsSettingsState(const std::string& acState) noexcept
{
    return acState == "SETTINGS_CATEGORY_STATE" || acState == "OPTIONS_LISTS_STATE" || acState == "DEFAULT_SETTINGS_CONFIRM_STATE" || acState == "INPUT_MAPPING_STATE";
}

// MainMenu's own two are private (see Bridge::changed): the bridge's record of them
bool SettingsChanged() noexcept
{
    return s.changed;
}

bool SavingSettings() noexcept
{
    return s.saving;
}

// Up to 10 s for the game to write its settings (it takes well under a second): then back isn't held any longer
constexpr int kSavingGiveUpFrames = 600;
// Out of the settings this long with nothing written (a key the game refused): nothing to save
constexpr int kLeftSettingsFrames = 30;

// The game writing its settings, read from its prompt each frame
void TrackSaving(const std::string& acState) noexcept
{
    const std::string prompt = FieldText(kRemapText);
    const bool says = prompt.find_first_not_of(" \t\r\n") != std::string::npos;
    if (acState == "INPUT_MAPPING_STATE")
    {
        if (says)
            s.remapText = prompt;
        return;
    }
    if (!s.saving && s.changed && says && prompt != s.remapText)
    {
        s.saving = true;
        s.savingFrames = 0;
        spdlog::info("Start menu: the game is saving the settings ({})", prompt);
    }
    else if (s.saving && (!says || ++s.savingFrames > kSavingGiveUpFrames))
    {
        if (says)
            spdlog::warn("Start menu: the game's settings save didn't finish in 10 s (the prompt still says {})", prompt);
        else
            spdlog::info("Start menu: the game saved the settings");
        s.saving = false;
        s.changed = false;
    }
    if (s.changed && !s.saving && !IsSettingsState(acState))
    {
        if (++s.leftFrames > kLeftSettingsFrames)
            s.changed = false;
    }
    else
        s.leftFrames = 0;
}

std::string BuildState(const std::string& acState) noexcept
{
    std::string json = fmt::format(R"({{"menu":"{}","state":{})", s.pause ? "pause" : "main", MiniJson::Quote(acState));
    if (IsSettingsState(acState) || SavingSettings())
        json += fmt::format(R"(,"settingsChanged":{},"savingSettings":{})", SettingsChanged() ? "true" : "false", SavingSettings() ? "true" : "false");

    if (acState == "MAIN_STATE")
        json += R"(,"main":)" + ListJson(kMainList, MainEntry);
    else if (acState == "SETTINGS_CATEGORY_STATE")
        json += R"(,"categories":)" + ListJson(kCategoryList, PlainEntry);
    else if (acState == "OPTIONS_LISTS_STATE" || acState == "DEFAULT_SETTINGS_CONFIRM_STATE")
    {
        json += R"(,"categories":)" + ListJson(kCategoryList, PlainEntry);
        json += R"(,"options":)" + ListJson(kOptionList, OptionEntry);
        json += R"(,"guide":)" + MiniJson::Quote(FieldText(kGuideText));
        if (!std::exchange(s.probedOptions, true))
            ProbeEntry("settings option", kOptionList);
    }
    else if (acState == "INPUT_MAPPING_STATE")
    {
        const bool remapping = IsRemapping();
        const bool prompt = NumberMember(Get(kRemapPrompt), "visible") != 0.0;
        json += R"(,"categories":)" + ListJson(kCategoryList, PlainEntry);
        json += R"(,"controls":)" + ListJson(kControlList, ControlEntry);
        // after the key: the game's word on it for a moment ("That button is reserved.")
        json += fmt::format(R"(,"remapping":{},"remapMessage":{})", remapping ? "true" : "false",
                            MiniJson::Quote(prompt && !remapping ? FieldText(kRemapText) : std::string{}));
    }
    else if (acState == "HELP_STATE")
    {
        json += R"(,"help":)" + ListJson(kHelpList, PlainEntry);
        json += R"(,"helpTitle":)" + MiniJson::Quote(FieldText(kHelpTitle));
        json += R"(,"helpText":)" + MiniJson::Quote(FieldText(kHelpText));
    }
    else if (acState == "SAVE_LOAD_STATE" || acState == "SAVE_LOAD_CONFIRM_STATE" || acState == "DELETE_SAVE_CONFIRM_STATE")
    {
        GFx::Value panel = Get(kSavePanel);
        json += R"(,"saves":)" + ListJson(kSaveList, SaveEntry);
        json += fmt::format(R"(,"saving":{},"saveInfo":{{"player":{},"time":{},"location":{},"level":{}}})", NumberMember(panel, "bSaving") != 0.0 ? "true" : "false",
                            MiniJson::Quote(FieldText("root.Menu_mc.SaveLoadHolder_mc.Panel_mc.PlayerInfo_tf")),
                            MiniJson::Quote(FieldText("root.Menu_mc.SaveLoadHolder_mc.Panel_mc.SaveTime_tf")),
                            MiniJson::Quote(FieldText("root.Menu_mc.SaveLoadHolder_mc.Panel_mc.LocationText_tf")),
                            MiniJson::Quote(FieldText("root.Menu_mc.SaveLoadHolder_mc.Panel_mc.LevelText_tf")));
        if (!std::exchange(s.probedSaves, true))
            ProbeEntry("save", kSaveList);
    }
    else if (acState == "CHARACTER_SELECT_STATE")
        json += R"(,"characters":)" + ListJson(kCharacterList, PlainEntry);

    if (acState.ends_with("_CONFIRM_STATE"))
    {
        json += R"(,"confirm":)" + MiniJson::Quote(FieldText(kConfirmText));
        // The game's own answers (MainMenu.UpdateButtons): quitting from the pause menu goes to the main menu or the
        // desktop, the rest are confirmed or not
        if (acState == "QUIT_CONFIRM_STATE" && s.pause)
            json += R"(,"buttons":[{"id":"accept","text":"MAIN MENU"},{"id":"desktop","text":"DESKTOP"},{"id":"cancel","text":"CANCEL"}])";
        else
            json += R"(,"buttons":[{"id":"accept","text":"CONFIRM"},{"id":"cancel","text":"CANCEL"}])";
    }

    return json + "}";
}

const char* ListPath(std::string_view aName) noexcept
{
    for (const auto& list : kLists)
    {
        if (aName == list.pName)
            return list.pPath;
    }
    return nullptr;
}

// The movie's own input, pressed and let go
void UserEvent(const char* acpEvent) noexcept
{
    GFx::Value menu = Get(kMenu);
    for (const bool down : {true, false})
    {
        GFx::Value args[2] = {GFx::Value(acpEvent), GFx::Value(down)};
        menu.Invoke("ProcessUserEvent", nullptr, args, 2);
    }
}

// The row selected, then its event. A list with disableSelection on (the settings categories while a category's page is
// open) keeps its selection: then nothing is sent, the press would act on the old row.
bool Select(GFx::Value& aList, int aRow, bool aPress) noexcept
{
    GFx::Value entries;
    if (!aList.GetMember("entryList", &entries) || aRow < 0 || static_cast<uint32_t>(aRow) >= entries.GetArraySize())
        return false;

    aList.SetMember("selectedIndex", GFx::Value(static_cast<double>(aRow)));
    if (static_cast<int>(NumberMember(aList, "selectedIndex", -1)) != aRow)
        return false;
    GFx::Value event;
    GFx::Value args[2] = {GFx::Value(aPress ? kItemPress : kSelectionChange), GFx::Value(true)};
    s.pMovie->CreateObject(&event, "flash.events.Event", args, 2);
    return aList.Invoke("dispatchEvent", nullptr, &event, 1);
}

// A settings option's new value, through its own row as the game's arrows and clicks change it: the row's value, then
// its change event (MainMenu.onSettingsValueChange passes it to the game, with the HUD and Pip-Boy colours and the
// difficulty's own steps). The row's own onValueChange is private (namespace kind 0x05 in MainMenu.swf, MCM's and the
// game's), so it can't be called by name: what it does is dispatch SettingsOptionItem.VALUE_CHANGE from the row, bubbling,
// and that's what's done here. MainMenu listens on itself for it and reads the row as the event's target (ID, value,
// hudColorUpdate); the list keeps the row's value through it too (OptionsList.onValueChange). Calling it by name set the
// row and told the game nothing (11-06: the HUD colour sliders changed nothing)
constexpr char kOptionValueChange[] = "SettingsOptionItem::value_change";

void SetOption(int aRow, double aValue) noexcept
{
    GFx::Value list = Get(kOptionList);
    if (!Select(list, aRow, false))
    {
        spdlog::warn("Start menu: option {} not set to {} (the list didn't take the row)", aRow, aValue);
        return;
    }

    GFx::Value clipIndex, clip;
    if (!list.GetMember("selectedClipIndex", &clipIndex))
        return;
    GFx::Value args[1] = {GFx::Value(clipIndex.GetNumber())};
    if (!list.Invoke("GetClipByIndex", &clip, args, 1) || !clip.IsObject())
    {
        spdlog::warn("Start menu: option {} has no row on screen", aRow);
        return;
    }

    clip.SetMember("value", GFx::Value(aValue));
    GFx::Value entries, entry;
    if (list.GetMember("entryList", &entries) && entries.GetMember(std::to_string(aRow).c_str(), &entry))
        entry.SetMember("value", GFx::Value(aValue));

    // new Event(SettingsOptionItem.VALUE_CHANGE, bubbles, cancelable), as the row makes it
    GFx::Value event;
    GFx::Value eventArgs[3] = {GFx::Value(kOptionValueChange), GFx::Value(true), GFx::Value(true)};
    s.pMovie->CreateObject(&event, "flash.events.Event", eventArgs, 3);
    const bool sent = event.IsObject() && clip.Invoke("dispatchEvent", nullptr, &event, 1);
    if (sent)
        s.changed = true;

    // what the row holds now (its own slider may have rounded or limited it) and whether the game was told
    spdlog::info("Start menu: option {} ({}, ID {}) set to {}, the row has {}{}{}, {}, changed {}", aRow, TextMember(entry, "text"), static_cast<int>(NumberMember(clip, "ID", -1)),
                 aValue, NumberMember(clip, "value", -1.0), NumberMember(clip, "hudColorUpdate") != 0.0 ? ", the HUD colour" : "",
                 NumberMember(clip, "pipboyColorUpdate") != 0.0 ? ", the Pip-Boy colour" : "", sent ? "sent to the game" : "NOT sent (no event)", SettingsChanged());
}

void Open() noexcept
{
    GFx::Value menu = Get(kMenu);
    menu.SetMember("visible", GFx::Value(false));
    s.open = true;
    s.sent.clear();
    if (s.pause)
    {
        Overlay().SetActive(true);
        while (ShowCursor(TRUE) <= 0)
            ;
    }
    else
        Overlay().SetMenuMode(true);
    spdlog::info("Start menu: the overlay draws the {} menu", s.pause ? "pause" : "main");
}

// aShowGame: the game's own movie shows again (one of its own screens, or our main menu screen is about to take over)
void Close(bool aShowGame, bool aKeepMenuMode) noexcept
{
    if (!s.open)
        return;
    s.open = false;
    s.pendingCategory = -1;
    s.pendingSave = false;
    s.heldBack = false;
    // (a save under way finishes in the game by itself)
    s.changed = false;
    s.saving = false;
    s.leftFrames = 0;
    s.remapText.clear();
    // the input given to the game for a key to map comes back, whatever ended the mapping (at the main menu nothing
    // else would: the overlay stays in its menu mode for our main menu screen)
    if (std::exchange(s.remapping, false))
        Overlay().SetActive(true);
    s.sent.clear();
    if (aShowGame && s.pMovie)
    {
        GFx::Value menu = Get(kMenu);
        menu.SetMember("visible", GFx::Value(true));
    }
    Overlay().SendEvent("startMenu", "null");
    if (s.pause)
    {
        Overlay().SetActive(false);
        while (ShowCursor(FALSE) >= 0)
            ;
    }
    else if (!aKeepMenuMode)
        Overlay().SetMenuMode(false);
    spdlog::info("Start menu: the overlay's {} menu closed", s.pause ? "pause" : "main");
}
} // namespace

bool StartMenuBridge::Covers(const std::string& acState, bool aPauseMenu) noexcept
{
    if (acState == "MAIN_STATE")
        return aPauseMenu;
    return std::ranges::find(kCovered, acState) != kCovered.end();
}

bool StartMenuBridge::IsOpen() noexcept
{
    return s.open;
}

void StartMenuBridge::OnPageRestarted() noexcept
{
    s.sent.clear();
}

void StartMenuBridge::Run(UI* apUI) noexcept
{
    IMenu* pPause = apUI->FindMenuByName("PauseMenu");
    IMenu* pMenu = pPause ? pPause : apUI->FindMenuByName("MainMenu");
    if (pMenu != s.pMenu)
    {
        // a closed menu's movie is gone: nothing of it is touched
        if (s.open)
        {
            s.pMovie = nullptr;
            Close(false, false);
        }
        s.pMenu = pMenu;
        s.pMovie = pMenu ? static_cast<GFx::Movie*>(pMenu->pMovie) : nullptr;
        s.pause = pPause != nullptr;
        s.probedSaves = s.probedOptions = false;
    }
    // MCM's pages, over the pause menu
    ModConfigBridge::Run(s.pMovie);
    if (!s.pMovie)
        return;

    GFx::Value stateValue = Get(kState);
    const std::string state = stateValue.GetString() ? stateValue.GetString() : "";
    // With the overlay off for good (its process kept ending) the game's menus are its own again
    const bool overlayOff = Overlay().IsOff();
    const bool covered = !overlayOff && !state.empty() && Covers(state, s.pause);
    if (!covered)
    {
        // our main menu screen takes the main list; the game's own screens show themselves
        Close(overlayOff || state != "MAIN_STATE", !overlayOff && state == "MAIN_STATE");
        return;
    }

    if (!s.open)
        Open();
    else
    {
        // the movie can show itself again as it changes screens
        GFx::Value menu = Get(kMenu);
        if (NumberMember(menu, "visible") != 0.0)
            menu.SetMember("visible", GFx::Value(false));
    }

    // Mapping a key: the game's own input takes the next key, then the overlay's again
    const bool remapping = state == "INPUT_MAPPING_STATE" && IsRemapping();
    if (remapping != s.remapping)
    {
        s.remapping = remapping;
        Overlay().SetActive(!remapping);
        spdlog::info("Start menu: {}", remapping ? "waiting for the key to map" : "the key mapping ended");
        // a key mapped is a change to save (one the game refused leaves nothing: SAVE then only goes back)
        if (!remapping)
            s.changed = true;
    }
    TrackSaving(state);

    // A category clicked on another category's page: the game is going back to its category screen; press it there
    if (s.pendingCategory >= 0)
    {
        ++s.pendingFrames;
        GFx::Value categories = Get(kCategoryList);
        if (state == "SETTINGS_CATEGORY_STATE" && Select(categories, s.pendingCategory, true))
        {
            spdlog::info("Start menu: category {} opened from another one's page", s.pendingCategory);
            s.pendingCategory = -1;
        }
        else if (s.pendingFrames > 120 || (state != "SETTINGS_CATEGORY_STATE" && state != "OPTIONS_LISTS_STATE" && state != "INPUT_MAPPING_STATE"))
        {
            spdlog::warn("Start menu: category {} not opened (the menu is at {})", s.pendingCategory, state);
            s.pendingCategory = -1;
        }
    }

    // SAVE from a category's page: the game is going back to its category screen; leaving that saves, as the game's own
    // back does
    if (s.pendingSave)
    {
        ++s.pendingSaveFrames;
        if (state == "SETTINGS_CATEGORY_STATE")
        {
            s.pendingSave = false;
            UserEvent("Cancel");
            spdlog::info("Start menu: saving the settings (changed {})", SettingsChanged());
        }
        else if (s.pendingSaveFrames > 120 || !IsSettingsState(state))
        {
            spdlog::warn("Start menu: the settings not saved from {} (the menu is at {})", "SAVE", state);
            s.pendingSave = false;
        }
    }
    if (s.heldBack && !SavingSettings())
    {
        s.heldBack = false;
        spdlog::info("Start menu: the settings are saved");
    }

    std::string json = BuildState(state);
    if (json != s.sent)
    {
        Overlay().SendEvent("startMenu", json);
        s.sent = std::move(json);
    }
}

void StartMenuBridge::OnAction(const std::string& acAction, const std::string& acPayload) noexcept
{
    if (!s.open || !s.pMovie)
        return;

    // "list:row" or "row:value"
    const auto colon = acPayload.find(':');
    const std::string first = acPayload.substr(0, colon);
    const std::string second = colon == std::string::npos ? std::string{} : acPayload.substr(colon + 1);

    // Another category from a category's page (Gameplay to Display): the game takes a category only on its category
    // screen, where the list can change selection; back there first (its Cancel, as the game's own back does), the
    // press follows in Run
    GFx::Value stateValue = Get(kState);
    const std::string state = stateValue.GetString() ? stateValue.GetString() : "";
    if (acAction == "press" && first == "category" && (state == "OPTIONS_LISTS_STATE" || state == "INPUT_MAPPING_STATE"))
    {
        const int row = std::atoi(second.c_str());
        if (s.remapping || row == static_cast<int>(NumberMember(Get(kCategoryList), "selectedIndex", -1)))
            return;
        s.pendingCategory = row;
        s.pendingFrames = 0;
        UserEvent("Cancel");
        return;
    }

    // While the game writes the settings ("Saving..."), a back press would close the pause menu before they're written:
    // it waits until the game is done
    if (acAction == "event" && acPayload == "Cancel" && SavingSettings())
    {
        if (!std::exchange(s.heldBack, true))
            spdlog::info("Start menu: back held while the settings are saved");
        return;
    }

    // SAVE: the game saves its settings as they're left from their category screen; from a category's page, back there
    // first (Run presses back again once it's there)
    if (acAction == "save")
    {
        if (SavingSettings() || s.remapping)
            return;
        if (state == "SETTINGS_CATEGORY_STATE")
        {
            UserEvent("Cancel");
            spdlog::info("Start menu: saving the settings (changed {})", SettingsChanged());
        }
        else if (state == "OPTIONS_LISTS_STATE" || state == "INPUT_MAPPING_STATE")
        {
            s.pendingSave = true;
            s.pendingSaveFrames = 0;
            UserEvent("Cancel");
        }
        return;
    }

    if (acAction == "press" || acAction == "select")
    {
        const char* pPath = ListPath(first);
        GFx::Value list = pPath ? Get(pPath) : GFx::Value{};
        if (!pPath || !Select(list, std::atoi(second.c_str()), acAction == "press"))
            spdlog::warn("Start menu: {} {} not done", acAction, acPayload);
    }
    else if (acAction == "set")
        SetOption(std::atoi(first.c_str()), std::atof(second.c_str()));
    // A confirmation's answer: what its buttons call (MainMenu's ConfirmButton, QuitToDesktopButton). The movie's
    // "Accept" input reaches onAcceptPress only on the defaults and add-ons screens; elsewhere the game's Enter goes
    // through the button bar.
    else if (acAction == "button" && (acPayload == "accept" || acPayload == "desktop"))
    {
        GFx::Value menu = Get(kMenu);
        if (!menu.Invoke(acPayload == "accept" ? "onAcceptPress" : "onQuitToDesktopPress"))
            spdlog::warn("Start menu: the {} button not pressed", acPayload);
        // the defaults taken: every option back to its default, a change to save (MainMenu sets bSettingsChanged)
        else if (state == "DEFAULT_SETTINGS_CONFIRM_STATE")
            s.changed = true;
    }
    // the movie's own buttons: Accept, Cancel (back; at the pause menu's list it resumes), XButton (delete a save),
    // YButton (defaults)
    else if (acAction == "event" && (acPayload == "Accept" || acPayload == "Cancel" || acPayload == "XButton" || acPayload == "YButton" || acPayload == "Up" ||
                                     acPayload == "Down"))
    {
        UserEvent(acPayload.c_str());
        if (acPayload == "Accept" && state == "DEFAULT_SETTINGS_CONFIRM_STATE")
            s.changed = true;
    }
    else
        spdlog::warn("Start menu: unknown action {} {}", acAction, acPayload);
}
