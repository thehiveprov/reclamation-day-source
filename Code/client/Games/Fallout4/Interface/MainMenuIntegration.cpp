#include <HivemindPCH.h>

#include <Interface/MainMenuIntegration.h>
#include <Interface/Scaleform.h>
#include <Interface/UI.h>
#include <Interface/IMenu.h>
#include <Interface/Updater.h>
#include <Interface/LanBrowser.h>
#include <Interface/MenuSounds.h>
#include <Interface/LoadOrderMatch.h>
#include <Interface/ModConfigBridge.h>
#include <Interface/StartMenuBridge.h>
#include <Interface/WorldSettings.h>

#include <PlayerCharacter.h>
#include <Games/TES.h>
#include <Forms/TESObjectCELL.h>
#include <Forms/TESWorldSpace.h>
#include <World.h>
#include <Services/OverlayService.h>
#include <Services/SteamService.h>
#include <Services/TransportService.h>
#include <SaveFolder.h>
#include <CoopWorlds.h>
#include <MiniJson.h>
#include <Requirements.h>
#include <StartMeUp.h>
#include <WinterOfAtom.h>
#include <Forms/TESGlobal.h>

#include <TlHelp32.h>
#include <ShlObj.h>

#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <set>

// The main menu in the overlay, over the game's own (Scaleform, MainMenu.swf).
//
// While the game's menu is at its main list (root.Menu_mc.currentState "MAIN_STATE") with none of the game's pop-ups
// over it, the game's menu is hidden and the overlay shows the main menu: CONTINUE, NEW GAME, LOAD (character and save
// cards), MULTIPLAYER (co-op characters and worlds), the game's other entries (settings, creations, credits, ...), QUIT.
// Its choices come back as actions (QueueMainMenuAction). The overlay asks its own "are you sure?" and then calls what
// the game's menu calls once its confirmation is accepted (BGSCodeObj ContinueGame, StartNewGame, onQuitPress); saves
// load like the game's load screen does; the game's other entries are pressed in its list and the overlay comes back
// once its menu is at the main list again. Playing a co-op character loads its save in the world's co-op save folder,
// and once the player is in the world, the game connects to the world's server.
//
// A MULTIPLAYER entry is also added to the game's own list (root.Menu_mc.MainPanel_mc.List_mc, a BSScrollingList whose
// entries are {text, index}; the game's own entries use indices 0-12), caught by a listener on the list's
// "BSScrollingList::itemPress" event that runs before the game's own: it opens the overlay should the game's list show.
namespace
{
constexpr int kMultiplayer = 1000;

constexpr char kListPath[] = "root.Menu_mc.MainPanel_mc.List_mc";
constexpr char kMenuPath[] = "root.Menu_mc";
constexpr char kItemPressEvent[] = "BSScrollingList::itemPress";

std::filesystem::path GetGameFolder() noexcept
{
    char modulePath[MAX_PATH];
    GetModuleFileNameA(nullptr, modulePath, sizeof(modulePath));
    return std::filesystem::path(modulePath).parent_path();
}

// Recent servers, newest first, kept next to the game
std::vector<std::string> LoadRecentServers() noexcept
{
    std::vector<std::string> servers;
    std::ifstream file(GetGameFolder() / "ReclamationDay_RecentServers.txt");
    for (std::string line; std::getline(file, line) && servers.size() < 5;)
    {
        if (!line.empty())
            servers.push_back(line);
    }
    return servers;
}

void RememberServer(const std::string& acAddress) noexcept
{
    auto servers = LoadRecentServers();
    std::erase(servers, acAddress);
    servers.insert(servers.begin(), acAddress);

    std::ofstream file(GetGameFolder() / "ReclamationDay_RecentServers.txt", std::ios::trunc);
    for (size_t i = 0; i < servers.size() && i < 5; ++i)
        file << servers[i] << '\n';
}

constexpr wchar_t kServerExe[] = L"ReclamationDayServer.exe";

// The server of the world this game hosts. It belongs to the host's session: started when a hosted save is played or
// created, stopped when the host goes back to the main menu, and ended by Windows with this game (its job object
// kills it once the game's handle to the job closes, whichever way the game exits).
struct HostedServer
{
    HANDLE hProcess{nullptr};
    HANDLE hJob{nullptr};
    std::string worldId;
    // given to it at its start (RD_HOST_KEY) and sent back when this game joins it: how it knows its host
    std::string hostKey;
} s_hostedServer;

// 128 random bits as hex
std::string MakeHostKey() noexcept
{
    std::random_device random;
    std::string key;
    for (int i = 0; i < 4; ++i)
        key += fmt::format("{:08x}", random());
    return key;
}

bool IsHostedServerRunning() noexcept
{
    return s_hostedServer.hProcess && WaitForSingleObject(s_hostedServer.hProcess, 0) == WAIT_TIMEOUT;
}

void StopLocalServer() noexcept
{
    if (!s_hostedServer.hProcess)
        return;

    // friends joined through Steam come through this game: not any more
    World::Get().ctx().at<SteamService>().StopHosting();

    if (IsHostedServerRunning())
    {
        TerminateProcess(s_hostedServer.hProcess, 0);
        WaitForSingleObject(s_hostedServer.hProcess, 3000);
        spdlog::info("Stopped the local server of world {}", s_hostedServer.worldId);
    }

    CloseHandle(s_hostedServer.hProcess);
    CloseHandle(s_hostedServer.hJob);
    s_hostedServer = {};
}

// Servers left over from earlier sessions (a game that closed before its job existed, one started by hand): only one
// server can have the port
void StopOtherServers() noexcept
{
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE)
        return;

    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(hSnapshot, &entry); ok; ok = Process32NextW(hSnapshot, &entry))
    {
        if (_wcsicmp(entry.szExeFile, kServerExe) != 0)
            continue;

        if (HANDLE hProcess = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID))
        {
            TerminateProcess(hProcess, 0);
            WaitForSingleObject(hProcess, 3000);
            CloseHandle(hProcess);
            spdlog::info("Stopped a leftover server (process {})", entry.th32ProcessID);
        }
    }

    CloseHandle(hSnapshot);
}

// Starts the server of a hosted world next to the game, without a console window (closing a visible one shut the
// server down). A fresh one for each session: the world's own server is kept only while it's still running for it.
bool StartLocalServer(const CoopWorlds::World& acWorld) noexcept
{
    if (IsHostedServerRunning() && s_hostedServer.worldId == acWorld.id)
        return true;

    StopLocalServer();
    StopOtherServers();

    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!hJob || !SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        spdlog::warn("Couldn't make the server's job object (error {}), it won't end with the game", GetLastError());

    // The world's settlements are kept in its folder (the server's WorkshopService), passed down through the
    // environment it inherits
    const auto worldFolder = SaveFolder::GetGameUserFolder() / acWorld.saveFolder;
    SetEnvironmentVariableW(L"RD_WORLD_DATA", worldFolder.wstring().c_str());
    // ...and its name, what other games on the network list it as (the server browsers' LAN tab)
    SetEnvironmentVariableW(L"RD_WORLD_NAME", std::filesystem::path(acWorld.name).wstring().c_str());
    // ...and the key that says this game is its host (not whoever comes through 127.0.0.1: friends joining through
    // Steam come through this game)
    const std::string hostKey = MakeHostKey();
    SetEnvironmentVariableW(L"RD_HOST_KEY", std::filesystem::path(hostKey).wstring().c_str());

    const auto folder = GetGameFolder();
    std::wstring commandLine = L"\"" + (folder / kServerExe).wstring() + L"\"";
    STARTUPINFOW startupInfo{sizeof(startupInfo)};
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, folder.wstring().c_str(), &startupInfo, &processInfo))
    {
        spdlog::error("Couldn't start the server: error {}", GetLastError());
        if (hJob)
            CloseHandle(hJob);
        return false;
    }

    // In the job before it runs, so nothing it starts escapes it
    if (hJob && !AssignProcessToJobObject(hJob, processInfo.hProcess))
        spdlog::warn("Couldn't put the server in its job object (error {}), it won't end with the game", GetLastError());
    ResumeThread(processInfo.hThread);
    CloseHandle(processInfo.hThread);

    s_hostedServer = {processInfo.hProcess, hJob, acWorld.id, hostKey};
    spdlog::info("Started the local server for world \"{}\" ({}), process {}", acWorld.name, acWorld.id, processInfo.dwProcessId);
    return true;
}

// Zips the crash dumps and logs into one file on the desktop, returns its name
std::string ExportCrashReports() noexcept
{
    PWSTR pDesktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &pDesktop)))
        return "";
    const std::filesystem::path desktop(pDesktop);
    CoTaskMemFree(pDesktop);

    const auto folder = GetGameFolder();
    std::wstring files;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
    {
        const auto name = entry.path().filename().wstring();
        if (name.starts_with(L"crash_") && entry.path().extension() == L".zip")
            files += L" \"" + name + L"\"";
    }
    files += L" logs";

    const auto time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    localtime_s(&tm, &time);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M", &tm);
    const std::string fileName = std::string("ReclamationDay_CrashReport_") + stamp + ".zip";

    wchar_t systemDir[MAX_PATH];
    GetSystemDirectoryW(systemDir, MAX_PATH);
    std::wstring commandLine = L"\"" + (std::filesystem::path(systemDir) / L"tar.exe").wstring() + L"\" -a -c -f \"" + (desktop / fileName).wstring() + L"\"" + files;

    STARTUPINFOW startupInfo{sizeof(startupInfo)};
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder.wstring().c_str(), &startupInfo, &processInfo))
        return "";

    WaitForSingleObject(processInfo.hProcess, 60000);
    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    spdlog::info("Exported crash reports to {} (tar exit code {})", fileName, exitCode);
    return exitCode == 0 ? fileName : "";
}

struct MainMenuState
{
    IMenu* pMenu = nullptr;
    GFx::Movie* pMovie = nullptr;
    GFx::Value list;
    bool listenerAdded = false;
    bool multiplayerPressed = false; // handled on the next frame (not inside the event)
    std::string status;              // shown on MULTIPLAYER and on the multiplayer screens

    void Forget() noexcept
    {
        list.Forget();
        *this = MainMenuState{};
    }
};

// Never destroyed: at exit the menu's movie may already be gone and releasing its objects would crash
MainMenuState& s_state = *new MainMenuState;

struct ItemPressHandler : GFx::FunctionHandler
{
    void Call(const Params& acParams) override
    {
        GFx::Value selected, index;
        if (!s_state.list.GetMember("selectedEntry", &selected) || !selected.GetMember("index", &index))
            return;

        if (static_cast<int>(index.GetNumber()) != kMultiplayer)
            return; // the game's own entries

        // Ours: the game must not see it
        if (acParams.argCount > 0)
            acParams.args[0].Invoke("stopImmediatePropagation");
        s_state.multiplayerPressed = true;
    }
};

ItemPressHandler s_itemPressHandler;

GFx::Value MakeEntry(const char* acpText, int aIndex) noexcept
{
    GFx::Value entry;
    s_state.pMovie->CreateObject(&entry);
    GFx::Value text;
    s_state.pMovie->CreateString(&text, acpText);
    entry.SetMember("text", text);
    entry.SetMember("index", GFx::Value(static_cast<double>(aIndex)));
    return entry;
}

GFx::Value GetEntries() noexcept
{
    GFx::Value entries;
    s_state.list.GetMember("entryList", &entries);
    return entries;
}

std::string GetMultiplayerText() noexcept
{
    return s_state.status.empty() ? "MULTIPLAYER" : "MULTIPLAYER - " + s_state.status;
}

// The MULTIPLAYER entry, if there, gets the current status text
bool UpdateMultiplayerEntry() noexcept
{
    GFx::Value entries = GetEntries();
    for (uint32_t i = 0; i < entries.GetArraySize(); ++i)
    {
        GFx::Value entry, index;
        if (entries.GetMember(std::to_string(i).c_str(), &entry) && entry.GetMember("index", &index) && static_cast<int>(index.GetNumber()) == kMultiplayer)
        {
            GFx::Value text;
            s_state.pMovie->CreateString(&text, GetMultiplayerText().c_str());
            entry.SetMember("text", text);
            s_state.list.Invoke("InvalidateData");
            return true;
        }
    }
    return false;
}

// Selects one of the game's own main menu entries and presses it like a click
bool PressGameEntry(int aGameIndex) noexcept
{
    GFx::Value entries = GetEntries();
    for (uint32_t i = 0; i < entries.GetArraySize(); ++i)
    {
        GFx::Value entry, index;
        if (!entries.GetMember(std::to_string(i).c_str(), &entry) || !entry.GetMember("index", &index) || static_cast<int>(index.GetNumber()) != aGameIndex)
            continue;

        s_state.list.SetMember("selectedIndex", GFx::Value(static_cast<double>(i)));

        GFx::Value event;
        GFx::Value args[2] = {GFx::Value(kItemPressEvent), GFx::Value(true)};
        s_state.pMovie->CreateObject(&event, "flash.events.Event", args, 2);
        return s_state.list.Invoke("dispatchEvent", nullptr, &event, 1);
    }

    spdlog::warn("MainMenu: game entry {} not found", aGameIndex);
    return false;
}

void SetMainMenuVisible(bool aVisible) noexcept
{
    GFx::Value menu;
    if (s_state.pMovie && s_state.pMovie->GetVariable(&menu, kMenuPath))
        menu.SetMember("visible", GFx::Value(aVisible));
}

// Adds MULTIPLAYER after CONTINUE and the listener for it
void Install() noexcept
{
    if (!s_state.list.IsObject() && !s_state.pMovie->GetVariable(&s_state.list, kListPath))
        return;

    GFx::Value entries = GetEntries();
    if (entries.GetArraySize() == 0)
        return; // not filled in yet

    if (!s_state.listenerAdded)
    {
        GFx::Value function;
        s_state.pMovie->CreateFunction(&function, &s_itemPressHandler);

        // Higher priority than the menu's own listener, so ours runs first and can stop the event
        GFx::Value eventName(kItemPressEvent);
        GFx::Value args[4] = {eventName, function, GFx::Value(false), GFx::Value(100.0)};
        s_state.listenerAdded = s_state.list.Invoke("addEventListener", nullptr, args, 4);
        spdlog::info("MainMenu: item press listener added: {}", s_state.listenerAdded);
    }

    if (!UpdateMultiplayerEntry())
    {
        GFx::Value args[3] = {GFx::Value(1.0), GFx::Value(0.0), MakeEntry(GetMultiplayerText().c_str(), kMultiplayer)};
        entries.Invoke("splice", nullptr, args, 3);
        s_state.list.Invoke("InvalidateData");
        spdlog::info("MainMenu: MULTIPLAYER entry added");
    }
}

// Save folders: the co-op worlds' (see CoopWorlds) are only used while playing multiplayer, so single-player saves are
// never written to, and the game's own CONTINUE/LOAD never show co-op saves
std::string s_baseSaveFolder; // the normal one (Saves\, or Saves\Player2\ for a second instance)

std::string GetBaseSaveFolder() noexcept
{
    if (s_baseSaveFolder.empty())
        s_baseSaveFolder = SaveFolder::GetLocalSavePath();
    return s_baseSaveFolder;
}

bool UsingCoopSaves() noexcept
{
    return !s_baseSaveFolder.empty() && SaveFolder::GetLocalSavePath().starts_with(CoopWorlds::GetRootFolder(s_baseSaveFolder));
}

void UseNormalSaves() noexcept
{
    if (UsingCoopSaves())
        SaveFolder::SetLocalSavePath(GetBaseSaveFolder());
}

// The address as connected to: "localhost" is 127.0.0.1, the port defaults to 10578
std::string NormalizeAddress(std::string aAddress) noexcept
{
    aAddress.erase(0, aAddress.find_first_not_of(' '));
    aAddress.erase(aAddress.find_last_not_of(' ') + 1);
    if (aAddress == "localhost" || aAddress.starts_with("localhost:"))
        aAddress.replace(0, 9, "127.0.0.1");
    if (!aAddress.empty() && aAddress.find(':') == std::string::npos)
        aAddress += ":10578";
    return aAddress;
}

// Where the game connects once the chosen save is loaded
std::string s_pendingServer;

// A friend's world to join through Steam (its host's SteamID, its name), opened on the multiplayer screen once the
// main menu is back
uint64_t s_steamJoin = 0;
std::string s_steamJoinName;

// The hosted server's port (it's started with the default)
constexpr uint16_t kHostedPort = 10578;

// A co-op game is only played connected: losing the server, or never reaching it after loading, saves and goes back to
// the main menu, and going back to the main menu leaves the server
enum class Session
{
    kNone,
    kConnecting,
    kPlaying,
    kLeaving,
};

Session s_session = Session::kNone;
std::chrono::steady_clock::time_point s_sessionTime; // when connecting or leaving started
std::string s_sessionAddress;
std::string s_leaveReason; // shown on MULTIPLAYER once back at the main menu
bool s_forcedQuit = false;

// Set once a game is being played, so the main menu showing again means the player came back from it
bool s_wasInGame = false;

bool IsInWorld() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    return pPlayer && pPlayer->GetNiNode() && pPlayer->parentCell;
}

// Past the game's start: not in the pre-war worlds or house (where Start Me Up's start happens too) nor inside
// Vault 111. A new co-op character only connects once there. Fallout4.esm forms, whose load index is always 00.
// Winter of Atom's start happens out by Diamond City's gate, so in the prequel campaign it is the start's own flag.
bool IsPastPrologue() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell)
        return false;

    if (WinterOfAtom::IsPrequelLoaded())
    {
        const TESGlobal* pStartDone = WinterOfAtom::GetGlobal(WinterOfAtom::kStartDone);
        return pStartDone && pStartDone->f >= 1.f;
    }

    constexpr uint32_t kPrologueCells[] = {0xA801E /* PrewarPlayerHouse01 */, 0x16D8 /* Vault111Cryo */, 0x16D9 /* Vault111Start */};
    constexpr uint32_t kPrologueWorlds[] = {0xA7FF4 /* SanctuaryHillsWorld */, 0xCA260 /* PrewarVault111 */, 0xECCFD /* PrewarTVStudio */};

    if (std::ranges::find(kPrologueCells, pPlayer->parentCell->formID) != std::end(kPrologueCells))
        return false;

    const TESWorldSpace* pWorld = pPlayer->GetWorldSpace();
    return !pWorld || std::ranges::find(kPrologueWorlds, pWorld->formID) == std::end(kPrologueWorlds);
}

// What Game.QuitToMainMenu does once the screen has faded out: Main::resetGame (Main singleton, CommonLibF4 id 756304,
// resetGame at 0x25), without a save
void QuitToMainMenu() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_main, 756305);
    if (uint8_t* pMain = *s_main.Get())
        pMain[0x25] = 1;
}

void LeaveToMainMenu(std::string aReason) noexcept
{
    spdlog::warn("MainMenu: {}, saving and going back to the main menu", aReason);
    s_leaveReason = std::move(aReason);
    s_session = Session::kLeaving;
    s_sessionTime = std::chrono::steady_clock::now();
    s_forcedQuit = false;
    SaveFolder::SaveAndQuitToMainMenu();
}

void RunSession() noexcept
{
    using namespace std::chrono_literals;

    const auto elapsed = std::chrono::steady_clock::now() - s_sessionTime;
    const bool online = World::Get().GetTransport().IsOnline();

    switch (s_session)
    {
    case Session::kConnecting:
    {
        // Through Steam the host may be asked first (a minute to answer); a host that turned the join down sends the
        // player back at once
        const bool steam = s_sessionAddress.starts_with(TiltedPhoques::Client::kPeerToPeerPrefix);
        if (online)
        {
            s_session = Session::kPlaying;
            spdlog::info("MainMenu: playing co-op on {} as character {:08X}", s_sessionAddress, SaveFolder::GetCurrentCharacterId());
        }
        else if (World::Get().GetTransport().TakeRefused())
            LeaveToMainMenu("THE HOST DIDN'T LET YOU IN");
        else if (elapsed > (steam ? 70s : 20s))
            LeaveToMainMenu(steam ? "THE HOST DIDN'T ANSWER" : "COULDN'T CONNECT TO " + s_sessionAddress);
        break;
    }
    case Session::kPlaying:
        if (!online && IsInWorld())
            LeaveToMainMenu("DISCONNECTED FROM THE SERVER");
        break;
    case Session::kLeaving:
        // The exit save can't always be made (the pause menu asks to quit without one then)
        if (elapsed > 10s && !s_forcedQuit)
        {
            spdlog::warn("MainMenu: still in game, going back to the main menu without saving");
            s_forcedQuit = true;
            QuitToMainMenu();
        }
        break;
    default: break;
    }
}

// Once a save is loaded and the player is in the world, connect to the chosen server
void RunPendingConnect() noexcept
{
    if (s_pendingServer.empty())
        return;

    // A new character first plays the start (Start Me Up's or the game's prologue) offline
    if (!IsInWorld() || !IsPastPrologue())
        return;

    auto& transport = World::Get().GetTransport();
    if (transport.IsOnline())
    {
        s_pendingServer.clear();
        s_session = Session::kPlaying;
        return;
    }

    // A moment after getting in, like connecting by hand
    static int s_framesInGame = 0;
    if (++s_framesInGame < 180)
        return;
    s_framesInGame = 0;

    const std::string address = NormalizeAddress(s_pendingServer);

    s_pendingServer.clear();
    s_session = Session::kConnecting;
    s_sessionTime = std::chrono::steady_clock::now();
    s_sessionAddress = address;
    World::Get().GetRunner().Queue([address] {
        const bool result = World::Get().GetTransport().Connect(address);
        spdlog::info("Connecting to {} from the main menu choice, request accepted: {}", address, result);
    });
}

// The overlay's multiplayer screens
OverlayService& GetOverlay() noexcept
{
    return World::Get().ctx().at<OverlayService>();
}

bool s_screenOpen = false;
bool s_screenDataLoaded = false; // the game's data was loaded when the overlay last sent its data
int s_framesAtMenu = 0;          // since the main menu appeared

// Actions from the overlay, which arrive on CEF's thread; run on the main loop, which also runs at the main menu
std::mutex s_actionsLock;
std::vector<std::pair<std::string, std::string>> s_actions;

// Start Me Up's pre-war memory choice removed (see StartMeUp::BlockPrewarMemory)
bool s_blockedPrewarMemory = false;

// A save chosen to play: its load is queued, starting it closes the main menu (and these screens with it)
bool s_loadQueued = false;
// What the overlay says while it's waiting for that
std::string s_loadQueuedMessage;
std::chrono::steady_clock::time_point s_loadQueuedTime;

// The other campaign was picked: the game restarts at this time (none: zero)
std::chrono::steady_clock::time_point s_restartAt{};

// The main menu shows while the game still loads its plugins: TESDataHandler (ModManager here) is only set a moment
// later, loadingFiles (0x1010) is set while loading, and compiledFileCollection.files (0xFC0, size at 0xFD0) lists the
// loaded plugins (alandtse/CommonLibF4, checked in game: 9 plugins, loadingFiles 0 once loaded)
bool IsGameDataLoaded() noexcept
{
    const auto* pManager = reinterpret_cast<const uint8_t*>(ModManager::Get());
    return pManager && pManager[0x1010] == 0 && *reinterpret_cast<const uint32_t*>(pManager + 0xFD0) > 0;
}

// The normal save folder as a world without a server, to list the single-player characters like a world's
CoopWorlds::World GetSinglePlayerWorld() noexcept
{
    CoopWorlds::World world;
    world.name = "Single player";
    world.saveFolder = GetBaseSaveFolder();
    return world;
}

// root.Menu_mc.currentState: "MAIN_STATE" while the game's menu shows its main list, another state in its own screens
// (settings, help, add-ons, login, its confirmations; MainMenu.swf's *_STATE constants are these strings)
std::string GetMenuState() noexcept
{
    GFx::Value state;
    if (!s_state.pMovie || !s_state.pMovie->GetVariable(&state, "root.Menu_mc.currentState") || !state.GetString())
        return {};
    return state.GetString();
}

// The game's own pop-ups and screens over the main menu (message boxes, credits, creations, the console): the overlay
// steps aside while one is open
bool IsNativeMenuOpen(UI* apUI) noexcept
{
    for (const char* name : {"MessageBoxMenu", "CreditsMenu", "CreationClubMenu", "Console", "PlayBinkMenu", "LoadingMenu", "TitleSequenceMenu"})
    {
        IMenu* pMenu = apUI->FindMenuByName(name);
        if (!pMenu)
            continue;
        for (IMenu* pOpen : apUI->MenuStack)
        {
            if (pOpen == pMenu)
                return true;
        }
    }
    return false;
}

// A function of the main menu's code object (BGSCodeObj), as MainMenu.swf calls them: ContinueGame and StartNewGame are
// what it runs once its own confirmation is accepted, onQuitPress quits after its confirmation. The overlay asks first.
bool CallMenuCode(const char* acpFunction) noexcept
{
    GFx::Value codeObject;
    if (!s_state.pMovie || !s_state.pMovie->GetVariable(&codeObject, "root.Menu_mc.BGSCodeObj") || !codeObject.IsObject())
    {
        spdlog::warn("MainMenu: the menu's code object isn't there, can't call {}", acpFunction);
        return false;
    }

    const bool result = codeObject.Invoke(acpFunction);
    spdlog::info("MainMenu: {} on the menu's code object: {}", acpFunction, result);
    return result;
}

// The game's own entries the overlay hands back to the game (its CONTINUE, NEW, LOAD and QUIT are the overlay's)
std::string BuildNativeEntries() noexcept
{
    constexpr int kOverlayEntries[] = {0 /* CONTINUE */, 1 /* NEW */, 4 /* LOAD */, 10 /* QUIT */, kMultiplayer};
    static const std::map<std::string, std::string> s_names{{"$ADD-ONS", "ADD-ONS"},     {"$CREATION CLUB", "CREATIONS"}, {"$CREATIONSNEW", "CREATIONS"},
                                                             {"$BETHESDA.NET", "MODS"},   {"$SETTINGS", "SETTINGS"},       {"$CREW", "CREDITS"},
                                                             {"$CREDITS", "CREDITS"},     {"$HELP", "HELP"}};

    std::string json;
    if (!s_state.list.IsObject())
        return json;

    GFx::Value entries = GetEntries();
    for (uint32_t i = 0; i < entries.GetArraySize(); ++i)
    {
        GFx::Value entry, index, text;
        if (!entries.GetMember(std::to_string(i).c_str(), &entry) || !entry.GetMember("index", &index) || !entry.GetMember("text", &text) || !text.GetString())
            continue;

        const int gameIndex = static_cast<int>(index.GetNumber());
        if (std::ranges::find(kOverlayEntries, gameIndex) != std::end(kOverlayEntries))
            continue;

        std::string name = text.GetString();
        if (const auto it = s_names.find(name); it != s_names.end())
            name = it->second;
        else if (name.starts_with('$'))
            name.erase(0, 1);

        json += fmt::format(R"({}{{"index":{},"text":{}}})", json.empty() ? "" : ",", gameIndex, MiniJson::Quote(name));
    }
    return json;
}

// Creations (plugins beyond the game and its six DLC, or any light plugin) are loaded: the game's new game prompt then
// warns that achievements are disabled. TESDataHandler's loaded files (standard at 0xFC0, size 0xFD0; light at 0xFD8,
// size 0xFE8).
bool AreCreationsLoaded() noexcept
{
    const auto* pManager = reinterpret_cast<const uint8_t*>(ModManager::Get());
    return pManager && (*reinterpret_cast<const uint32_t*>(pManager + 0xFD0) > 7 || *reinterpret_cast<const uint32_t*>(pManager + 0xFE8) > 0);
}

constexpr uint32_t kThumbnailWidth = 220;

int64_t ToUnixMilliseconds(std::filesystem::file_time_type aTime) noexcept
{
    const auto systemTime = std::chrono::clock_cast<std::chrono::system_clock>(aTime);
    return std::chrono::duration_cast<std::chrono::milliseconds>(systemTime.time_since_epoch()).count();
}

std::string ToJson(const SaveFolder::SaveInfo& acSave, bool aThumbnail) noexcept
{
    return fmt::format(R"({{"file":{},"name":{},"kind":{},"number":{},"level":{},"location":{},"playTime":{},"modified":{},"thumbnail":{}}})",
                       MiniJson::Quote(acSave.GetName()), MiniJson::Quote(acSave.playerName), MiniJson::Quote(acSave.kind), acSave.number, acSave.level,
                       MiniJson::Quote(acSave.location), MiniJson::Quote(acSave.playTime), ToUnixMilliseconds(acSave.modified),
                       MiniJson::Quote(aThumbnail ? SaveFolder::ReadThumbnail(acSave, kThumbnailWidth) : std::string{}));
}

std::string ToJson(const CoopWorlds::World& acWorld, size_t aCharacterCount) noexcept
{
    return fmt::format(R"({{"id":{},"name":{},"hosted":{},"address":{},"characters":{}}})", MiniJson::Quote(acWorld.id), MiniJson::Quote(acWorld.name),
                       acWorld.hosted ? "true" : "false", MiniJson::Quote(acWorld.address), aCharacterCount);
}

std::string ToJson(const CoopWorlds::World& acWorld, const CoopWorlds::Character& acCharacter) noexcept
{
    return fmt::format(R"({{"id":{},"worldId":{},"saves":{},"newest":{}}})", MiniJson::Quote(acCharacter.id), MiniJson::Quote(acWorld.id), acCharacter.saves.size(),
                       ToJson(acCharacter.saves.front(), true));
}

// The mods Reclamation Day needs (Requirements.h): co-op is only played with all of them (StartSession)
using Requirements::Requirement;

// The game's own folder, where the launcher was pointed at (empty if it can't tell)
std::filesystem::path GetFallout4Folder() noexcept
{
    wchar_t path[MAX_PATH * 2]{};
    DWORD size = sizeof(path);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\TiltedPhoques\\TiltedEvolution\\Fallout4", L"TitlePath", RRF_RT_REG_SZ, nullptr, path, &size) != ERROR_SUCCESS)
        return {};
    return path;
}

// The plugins switched on in plugins.txt (lowercase): one per line, active ones start with '*'
std::set<std::string> ReadActivePlugins() noexcept
{
    std::set<std::string> active;
    PWSTR pLocalAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &pLocalAppData)))
        return active;
    const std::filesystem::path path = std::filesystem::path(pLocalAppData) / L"Fallout4" / L"plugins.txt";
    CoTaskMemFree(pLocalAppData);

    std::ifstream file(path);
    for (std::string line; std::getline(file, line);)
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.size() > 1 && line[0] == '*')
        {
            std::transform(line.begin(), line.end(), line.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            active.insert(line.substr(1));
        }
    }
    return active;
}

// Switched on in plugins.txt, or already in the game's load order. Not only the load order: it's only compiled a few
// seconds after the game's data is loaded, and the main menu asks before that (it said everything was missing).
bool IsPluginActive(const char* apPlugin, const std::set<std::string>& acActive) noexcept
{
    std::string name = apPlugin;
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (acActive.contains(name))
        return true;

    const ModManager* pManager = ModManager::Get();
    const Mod* pMod = pManager ? pManager->GetByName(apPlugin) : nullptr;
    return pMod && pMod->GetId() != 0xFF;
}

// The requirements that aren't there, each with the file it lacks
std::vector<std::pair<const Requirement*, const char*>> FindMissingRequirements(bool aGameDataLoaded) noexcept
{
    const auto activePlugins = ReadActivePlugins();
    const auto game = GetFallout4Folder();

    std::vector<std::pair<const Requirement*, const char*>> missing;
    for (const auto& requirement : Requirements::kAll)
    {
        // Without plugins.txt (a mod manager keeping its own), only the game's load order can tell, once loaded
        if (requirement.kind == Requirements::Kind::kPlugin && activePlugins.empty() && !aGameDataLoaded)
            continue;
        // Without the launcher's record of the game's folder, there's nowhere to look
        if (requirement.kind == Requirements::Kind::kF4seFile && game.empty())
            continue;

        for (const char* pFile : requirement.files)
        {
            std::error_code ec;
            bool found = false;
            switch (requirement.kind)
            {
            case Requirements::Kind::kPlugin: found = IsPluginActive(pFile, activePlugins); break;
            case Requirements::Kind::kDll: found = GetModuleHandleA(pFile) != nullptr; break;
            case Requirements::Kind::kF4seFile: found = std::filesystem::exists(game / Requirements::InstalledPath(requirement, pFile), ec); break;
            }
            if (!found)
            {
                missing.emplace_back(&requirement, pFile);
                break;
            }
        }
    }
    return missing;
}

std::string BuildMissingRequirements(bool aGameDataLoaded) noexcept
{
    static std::set<std::string> s_logged;
    std::string json;
    for (const auto& [pRequirement, pMissing] : FindMissingRequirements(aGameDataLoaded))
    {
        json += fmt::format(R"({}{{"name":{},"url":{}}})", json.empty() ? "" : ",", MiniJson::Quote(fmt::format("{} ({})", pRequirement->pName, pRequirement->pWhich)),
                            MiniJson::Quote(Requirements::DownloadUrl(*pRequirement)));
        if (s_logged.insert(pMissing).second)
            spdlog::warn("MainMenu: required {} ({}) isn't loaded", pRequirement->pName, pMissing);
    }
    return json;
}

bool IsRequirementUrl(const std::string& acUrl) noexcept
{
    return std::ranges::any_of(Requirements::kAll, [&acUrl](const Requirement& acRequirement) { return acUrl == Requirements::DownloadUrl(acRequirement); });
}

// The update the main menu offers (null when this is the latest release), see Updater
std::string BuildUpdate() noexcept
{
    const auto status = Updater::GetStatus();
    const char* pState = nullptr;
    switch (status.state)
    {
    case Updater::State::kAvailable: pState = "available"; break;
    case Updater::State::kUpdating:
    case Updater::State::kReady: pState = "updating"; break;
    case Updater::State::kFailed: pState = "failed"; break;
    default: return "null";
    }
    return fmt::format(R"({{"version":{},"current":{},"state":"{}","message":{}}})", MiniJson::Quote(status.latestVersion), MiniJson::Quote(BUILD_COMMIT), pState,
                       MiniJson::Quote(status.message));
}

// Everything the first screens show: the single-player characters and newest save (CONTINUE), the co-op worlds and
// their characters (with their newest save), recent servers, the game's own entries, required mods that are missing
std::string BuildData(const std::string& acSelectedWorldId) noexcept
{
    std::string worlds, characters;
    for (const auto& world : CoopWorlds::ListWorlds(GetBaseSaveFolder()))
    {
        const auto worldCharacters = CoopWorlds::ListCharacters(world);
        worlds += (worlds.empty() ? "" : ",") + ToJson(world, worldCharacters.size());

        for (const auto& character : worldCharacters)
            characters += (characters.empty() ? "" : ",") + ToJson(world, character);
    }

    std::string singlePlayerCharacters, continueSave = "null";
    const auto singlePlayerWorld = GetSinglePlayerWorld();
    for (const auto& character : CoopWorlds::ListCharacters(singlePlayerWorld))
    {
        singlePlayerCharacters += (singlePlayerCharacters.empty() ? "" : ",") + ToJson(singlePlayerWorld, character);
        if (continueSave == "null")
            continueSave = ToJson(character.saves.front(), true);
    }

    std::string recentServers;
    for (const auto& server : LoadRecentServers())
        recentServers += (recentServers.empty() ? "" : ",") + MiniJson::Quote(server);

    const bool dataLoaded = IsGameDataLoaded();
    return fmt::format(
        R"({{"status":{},"dataLoaded":{},"creationsLoaded":{},"startMeUp":{},"selectedWorldId":{},"continueSave":{},"singlePlayerCharacters":[{}],"nativeEntries":[{}],"recentServers":[{}],"worlds":[{}],"characters":[{}],"missingRequirements":[{}],"update":{},"campaign":{}}})",
        MiniJson::Quote(s_state.status), dataLoaded ? "true" : "false", dataLoaded && AreCreationsLoaded() ? "true" : "false",
        StartMeUp::IsInstalled(dataLoaded) ? "true" : "false", MiniJson::Quote(acSelectedWorldId), continueSave, singlePlayerCharacters, BuildNativeEntries(),
        recentServers, worlds, characters, BuildMissingRequirements(dataLoaded), BuildUpdate(), MiniJson::Quote(Updater::GetCampaign()));
}

void SendData(const std::string& acSelectedWorldId = {}) noexcept
{
    GetOverlay().SendEvent("mainMenuData", BuildData(acSelectedWorldId));
}

void SendStatus(const std::string& acMessage, bool aBusy, bool aError) noexcept
{
    GetOverlay().SendEvent("mainMenuStatus", fmt::format(R"({{"message":{},"busy":{},"error":{}}})", MiniJson::Quote(acMessage), aBusy ? "true" : "false",
                                                         aError ? "true" : "false"));
}

// A character's saves: in a co-op world, or the single-player ones (no world id)
void SendSaves(const std::string& acWorldId, const std::string& acCharacterId) noexcept
{
    std::string saves;
    if (const auto world = acWorldId.empty() ? std::optional(GetSinglePlayerWorld()) : CoopWorlds::FindWorld(GetBaseSaveFolder(), acWorldId))
    {
        for (const auto& character : CoopWorlds::ListCharacters(*world))
        {
            if (character.id != acCharacterId)
                continue;
            for (const auto& save : character.saves)
                saves += (saves.empty() ? "" : ",") + ToJson(save, true);
        }
    }

    GetOverlay().SendEvent("mainMenuSaves",
                           fmt::format(R"({{"worldId":{},"characterId":{},"saves":[{}]}})", MiniJson::Quote(acWorldId), MiniJson::Quote(acCharacterId), saves));
}

// Single-player saves a co-op character can start from: past Vault 111, of this campaign, newest first
std::vector<SaveFolder::SaveInfo> ListSourceSaves() noexcept
{
    std::vector<SaveFolder::SaveInfo> sources;
    for (auto& save : SaveFolder::ListSaves(SaveFolder::GetGameUserFolder() / GetBaseSaveFolder()))
    {
        if (save.IsPastVault111() && Updater::IsOfThisCampaign(save.plugins))
            sources.push_back(std::move(save));
        if (sources.size() == 24)
            break;
    }
    return sources;
}

void SendSourceSaves() noexcept
{
    std::string saves;
    for (const auto& save : ListSourceSaves())
        saves += (saves.empty() ? "" : ",") + ToJson(save, true);

    GetOverlay().SendEvent("mainMenuSourceSaves", fmt::format(R"({{"saves":[{}]}})", saves));
}

void OpenScreen() noexcept
{
    GetBaseSaveFolder(); // remembered before anything changes it
    CoopWorlds::AdoptLooseSaves(GetBaseSaveFolder());

    SetMainMenuVisible(false);
    s_screenOpen = true;
    s_screenDataLoaded = IsGameDataLoaded();
    GetOverlay().SetMenuMode(true);
    GetOverlay().SendEvent("mainMenuOpen", BuildData({}));
    spdlog::info("MainMenu: overlay main menu opened");
}

// aKeepMenuMode: the overlay stays up for the game's menu screens it draws (StartMenuBridge)
void CloseScreen(bool aShowMainMenu, bool aKeepMenuMode = false) noexcept
{
    if (!s_screenOpen)
        return;

    s_screenOpen = false;
    GetOverlay().SendEvent("mainMenuClose", {});
    if (!aKeepMenuMode)
        GetOverlay().SetMenuMode(false);
    if (aShowMainMenu)
    {
        SetMainMenuVisible(true);
        UpdateMultiplayerEntry();
    }
    spdlog::info("MainMenu: multiplayer screens closed");
}

// Points the game at the world's saves and the connection at its server: the world's own (started if needed) or the
// one it was joined on
bool StartSession(const CoopWorlds::World& acWorld) noexcept
{
    // Everyone plays with all the required mods (the title screen lists the missing ones, with where to get them)
    if (const auto missing = FindMissingRequirements(true); !missing.empty())
    {
        std::string names;
        for (const auto& [pRequirement, pFile] : missing)
            names += (names.empty() ? "" : ", ") + std::string(pRequirement->pName);
        spdlog::warn("MainMenu: not playing world \"{}\", missing {}", acWorld.name, names);
        SendStatus("INSTALL THESE FIRST: " + names, false, true);
        return false;
    }

    if (acWorld.hosted && !StartLocalServer(acWorld))
    {
        SendStatus("COULDN'T START THE SERVER", false, true);
        return false;
    }

    s_pendingServer = acWorld.hosted ? "127.0.0.1" : acWorld.address;
    World::Get().GetTransport().SetHostKey(acWorld.hosted ? s_hostedServer.hostKey : std::string());

    // What Steam friends see, and the hosted world's door for them
    auto& steam = World::Get().ctx().at<SteamService>();
    if (acWorld.hosted)
        steam.StartHosting(acWorld.id, acWorld.name, SteamService::ParsePrivacy(acWorld.privacy), kHostedPort);
    else
    {
        const std::string_view prefix = TiltedPhoques::Client::kPeerToPeerPrefix;
        const uint64_t host = acWorld.address.starts_with(prefix) ? std::strtoull(acWorld.address.c_str() + prefix.size(), nullptr, 10) : 0;
        steam.SetPlaying(acWorld.name, host);
    }
    s_state.status = acWorld.hosted ? "HOSTING " + acWorld.name : "JOINING " + acWorld.name;
    SaveFolder::SetLocalSavePath(acWorld.saveFolder);
    spdlog::info("MainMenu: playing world \"{}\" ({}) on {}", acWorld.name, acWorld.id, s_pendingServer);
    return true;
}

void CancelSession() noexcept
{
    s_pendingServer.clear();
    s_loadQueued = false;
    s_state.status.clear();
    UseNormalSaves();
}

// Starting a game needs the game's data: the main menu shows before its plugins are loaded
bool CheckGameDataLoaded() noexcept
{
    if (IsGameDataLoaded())
        return true;

    SendStatus("THE GAME IS STILL LOADING, TRY AGAIN IN A MOMENT", false, true);
    return false;
}

// Waiting for the game to start what was chosen (loading or a new game closes the main menu, and the overlay with it)
void WaitForGameStart(const char* acpMessage) noexcept
{
    s_loadQueued = true;
    s_loadQueuedTime = std::chrono::steady_clock::now();
    s_loadQueuedMessage = acpMessage;
    SendStatus(acpMessage, true, false);
}

// Loads a save of the current save folder
void LoadFromCurrentFolder(const std::string& acSaveName) noexcept
{
    SaveFolder::BuildSaveList();
    if (!SaveFolder::LoadSave(acSaveName))
    {
        CancelSession();
        SendStatus("COULDN'T LOAD THAT SAVE", false, true);
        return;
    }

    WaitForGameStart("LOADING...");
}

void Play(const CoopWorlds::World& acWorld, const std::string& acSaveName) noexcept
{
    if (!CheckGameDataLoaded() || !StartSession(acWorld))
        return;

    LoadFromCurrentFolder(acSaveName);
}

// A single-player save: the normal save folder, no server
void PlaySinglePlayer(const std::string& acSaveName) noexcept
{
    if (!CheckGameDataLoaded())
        return;

    CancelSession();
    LoadFromCurrentFolder(acSaveName);
}

// The load queued for a chosen save should start (closing the main menu) on the game's next frames
void RunQueuedLoad() noexcept
{
    using namespace std::chrono_literals;

    if (s_loadQueued && std::chrono::steady_clock::now() - s_loadQueuedTime > 15s)
    {
        spdlog::warn("MainMenu: the chosen save didn't start loading");
        CancelSession();
        SendStatus("COULDN'T LOAD THAT SAVE", false, true);
    }
}

void HandleAction(const std::string& acAction, const std::string& acPayload) noexcept
{
    spdlog::info("MainMenu: action {} {}", acAction, acPayload);

    const auto getWorld = [&acPayload]() { return CoopWorlds::FindWorld(GetBaseSaveFolder(), MiniJson::GetString(acPayload, "worldId")); };

    if (acAction == "continue")
    {
        // The newest single-player save, as the game's CONTINUE (asked in the overlay). With two campaigns, the newest
        // of this one's: the game's own would take the newest of all.
        if (!CheckGameDataLoaded())
            return;
        if (!Updater::GetCampaign().empty())
        {
            const auto characters = CoopWorlds::ListCharacters(GetSinglePlayerWorld());
            if (characters.empty())
                return SendStatus("NO SAVES OF THIS CAMPAIGN YET", false, true);
            return PlaySinglePlayer(characters.front().saves.front().GetName());
        }
        CancelSession();
        SaveFolder::BuildSaveList();
        if (CallMenuCode("ContinueGame"))
            WaitForGameStart("LOADING...");
    }
    else if (acAction == "switchCampaign")
    {
        // The other campaign's plugins load at the next start (Updater: campaign.txt, then plugins.txt): written, then
        // the game restarts a moment later, once the overlay has shown why
        const std::string campaign = MiniJson::GetString(acPayload, "campaign");
        const std::string& current = Updater::GetCampaign();
        if (current.empty() || campaign == current || (campaign != "commonwealth" && campaign != "winter-of-atom"))
            return;
        if (!Updater::SetCampaign(campaign))
            return SendStatus("COULDN'T SWITCH THE CAMPAIGN", false, true);
        CancelSession();
        SendStatus(campaign == "winter-of-atom" ? "RESTARTING INTO WINTER OF ATOM..." : "RESTARTING INTO 2287...", true, false);
        s_restartAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    }
    else if (acAction == "newGame")
    {
        // A single-player new game, as the game's NEW (asked in the overlay)
        if (!CheckGameDataLoaded())
            return;
        CancelSession();
        if (CallMenuCode("StartNewGame"))
            WaitForGameStart("STARTING A NEW GAME...");
    }
    else if (acAction == "native")
    {
        // One of the game's own screens (settings, creations, credits, ...): the overlay comes back once the game's
        // menu is back at its main list
        const int index = std::atoi(MiniJson::GetString(acPayload, "index").c_str());
        CloseScreen(true);
        spdlog::info("MainMenu: opening the game's entry {}: {}", index, PressGameEntry(index));
    }
    else if (acAction == "quit")
        CallMenuCode("onQuitPress");
    else if (acAction == "refresh")
        SendData();
    else if (acAction == "uiReady")
    {
        // The overlay's page finished loading after the screen opened: our mainMenuOpen went nowhere (the overlay
        // stayed empty over the game's main menu), so it's sent again
        GetOverlay().SendEvent("mainMenuOpen", BuildData({}));
        spdlog::info("MainMenu: the overlay loaded after the main menu opened, opened it again");
        // A page started again while a chosen save loads: back to the waiting, not the title's buttons
        if (s_loadQueued)
            SendStatus(s_loadQueuedMessage, true, false);
    }
    else if (acAction == "listSaves")
        SendSaves(MiniJson::GetString(acPayload, "worldId"), MiniJson::GetString(acPayload, "characterId"));
    else if (acAction == "listSourceSaves")
        SendSourceSaves();
    else if (acAction == "createWorld")
    {
        const std::string name = MiniJson::GetString(acPayload, "name");
        if (name.empty())
            return SendStatus("NAME YOUR WORLD", false, true);

        SendData(CoopWorlds::CreateWorld(GetBaseSaveFolder(), name, true, {}).id);
    }
    else if (acAction == "addServer")
    {
        const std::string address = NormalizeAddress(MiniJson::GetString(acPayload, "address"));
        if (address.empty() || address.find(' ') != std::string::npos)
            return SendStatus("ENTER THE SERVER'S ADDRESS", false, true);

        RememberServer(address);

        // One world per server: joining the same address again picks its world
        for (const auto& world : CoopWorlds::ListWorlds(GetBaseSaveFolder()))
        {
            if (!world.hosted && world.address == address)
                return SendData(world.id);
        }
        SendData(CoopWorlds::CreateWorld(GetBaseSaveFolder(), MiniJson::GetString(acPayload, "name"), false, address).id);
    }
    else if (acAction == "play" && MiniJson::GetString(acPayload, "worldId").empty())
    {
        const std::string save = MiniJson::GetString(acPayload, "save");
        const auto saves = SaveFolder::ListSaves(SaveFolder::GetGameUserFolder() / GetBaseSaveFolder());
        if (std::ranges::none_of(saves, [&save](const auto& acSave) { return acSave.GetName() == save && Updater::IsOfThisCampaign(acSave.plugins); }))
            return SendStatus("THAT SAVE IS GONE", false, true);

        PlaySinglePlayer(save);
    }
    else if (acAction == "play")
    {
        const auto world = getWorld();
        const std::string save = MiniJson::GetString(acPayload, "save");
        const auto saves = world ? SaveFolder::ListSaves(SaveFolder::GetGameUserFolder() / world->saveFolder) : std::vector<SaveFolder::SaveInfo>{};
        if (std::ranges::none_of(saves, [&save](const auto& acSave) { return acSave.GetName() == save && Updater::IsOfThisCampaign(acSave.plugins); }))
            return SendStatus("THAT SAVE IS GONE", false, true);

        Play(*world, save);
    }
    // DELETE on one of a character's saves (single player without a world id): to the Recycle Bin, then the saves and the
    // characters (their counts and newest saves) again
    else if (acAction == "deleteSave")
    {
        const std::string worldId = MiniJson::GetString(acPayload, "worldId");
        const auto world = worldId.empty() ? std::optional(GetSinglePlayerWorld()) : getWorld();
        const std::string save = MiniJson::GetString(acPayload, "save");
        const auto saves = world ? SaveFolder::ListSaves(SaveFolder::GetGameUserFolder() / world->saveFolder) : std::vector<SaveFolder::SaveInfo>{};
        const auto found = std::ranges::find_if(saves, [&save](const auto& acSave) { return acSave.GetName() == save && Updater::IsOfThisCampaign(acSave.plugins); });
        if (found == saves.end())
            return SendStatus("THAT SAVE IS GONE", false, true);
        if (!SaveFolder::DeleteSave(*found))
            return SendStatus("COULDN'T DELETE THAT SAVE", false, true);

        SendSaves(worldId, MiniJson::GetString(acPayload, "characterId"));
        SendData();
    }
    else if (acAction == "newCharacter")
    {
        // A new game saved in the world's folder: a character of its own (a new game gets a new character id). With
        // Start Me Up Redux its quick start skips the prologue; connecting waits until the start is done.
        const auto world = getWorld();
        if (!world || !CheckGameDataLoaded() || !StartSession(*world))
            return;

        if (!s_blockedPrewarMemory)
            s_blockedPrewarMemory = StartMeUp::BlockPrewarMemory();
        spdlog::info("MainMenu: Start Me Up installed: {}, its pre-war memory choice removed: {}", StartMeUp::IsInstalled(true), s_blockedPrewarMemory);

        // Asked in the overlay: straight to the game's new game, without its own confirmation
        spdlog::info("MainMenu: starting a new co-op character in \"{}\"", world->name);
        if (CallMenuCode("StartNewGame"))
            WaitForGameStart("STARTING A NEW GAME...");
        else
            CancelSession();
    }
    else if (acAction == "newCharacterFrom")
    {
        // A copy of a single-player save under a new character id: a co-op character of its own
        const auto world = getWorld();
        const std::string source = MiniJson::GetString(acPayload, "save");
        const auto sources = ListSourceSaves();
        const auto it = std::ranges::find_if(sources, [&source](const auto& acSave) { return acSave.GetName() == source; });
        if (!world || it == sources.end())
            return SendStatus("THAT SAVE IS GONE", false, true);
        if (!CheckGameDataLoaded())
            return;

        std::filesystem::path target;
        if (!SaveFolder::CopySave(*it, SaveFolder::GetGameUserFolder() / world->saveFolder, SaveFolder::NewCharacterId(), &target))
            return SendStatus("COULDN'T COPY THE SAVE", false, true);

        Play(*world, target.stem().string());
    }
    else if (acAction == "exportCrashReports")
    {
        const std::string fileName = ExportCrashReports();
        SendStatus(fileName.empty() ? "EXPORT FAILED" : "SAVED TO DESKTOP: " + fileName, false, fileName.empty());
    }
    else if (acAction == "update")
    {
        // Downloads in the background; RunUpdater shows the progress and restarts once it's in
        Updater::StartUpdate();
        SendData();
    }
    else if (acAction == "openUrl")
    {
        // A missing mod's download page, in the player's browser. Only those: the overlay can't open anything else.
        const std::string url = MiniJson::GetString(acPayload, "url");
        if (IsRequirementUrl(url))
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// The update check runs from the start; the main menu shows what it found, then the update's progress, and restarts
// the game once it's installed
void RunUpdater() noexcept
{
    Updater::StartCheck();

    static Updater::State s_lastState = Updater::State::kIdle;
    static std::string s_lastMessage;
    const auto status = Updater::GetStatus();

    if (status.state == Updater::State::kReady)
        Updater::Restart();

    const bool stateChanged = status.state != s_lastState;
    const bool messageChanged = status.message != s_lastMessage;
    s_lastState = status.state;
    s_lastMessage = status.message;

    if (!s_screenOpen)
        return;
    if (stateChanged)
        SendData();
    if (status.state == Updater::State::kUpdating && messageChanged)
        SendStatus(status.message, true, false);
    else if (status.state == Updater::State::kFailed && stateChanged)
        SendStatus(status.message, false, true);
}

// The friend's world joined through Steam (JoinSteamWorld): its world here, one for each host, opened on the screen
void RunSteamJoin() noexcept
{
    const std::string address = TiltedPhoques::Client::kPeerToPeerPrefix + std::to_string(std::exchange(s_steamJoin, 0));
    std::string id;
    for (const auto& world : CoopWorlds::ListWorlds(GetBaseSaveFolder()))
    {
        if (!world.hosted && world.address == address)
            id = world.id;
    }
    if (id.empty())
        id = CoopWorlds::CreateWorld(GetBaseSaveFolder(), s_steamJoinName, false, address).id;
    SendData(id);
    GetOverlay().SendEvent("mainMenuOpenWorld", MiniJson::Quote(id));
}

void RunActions() noexcept
{
    std::vector<std::pair<std::string, std::string>> actions;
    {
        std::scoped_lock _(s_actionsLock);
        actions.swap(s_actions);
    }

    for (const auto& [action, payload] : actions)
    {
        // The page is up. If it's one coming back after its process ended, it gets the game's state
        if (action == "uiReady" && GetOverlay().OnPageReady())
        {
            StartMenuBridge::OnPageRestarted();
            ModConfigBridge::OnPageRestarted();
        }

        // The game's start menu as the overlay draws it (the pause menu, settings, ...), wherever the menu is
        if (action.starts_with("startMenu."))
            StartMenuBridge::OnAction(action.substr(std::size("startMenu.") - 1), payload);
        // MCM's pages over the pause menu
        else if (action.starts_with("modConfig."))
            ModConfigBridge::OnAction(action.substr(std::size("modConfig.") - 1), payload);
        // The F2 overlay's WORLD SETTINGS, in the world
        else if (action.starts_with("worldSettings."))
            WorldSettings::OnAction(action.substr(std::size("worldSettings.") - 1), payload);
        else if (s_screenOpen)
            HandleAction(action, payload);
    }
}
} // namespace

bool IsHostingWorld() noexcept
{
    return IsHostedServerRunning();
}

bool IsPlayingCoopSave() noexcept
{
    return UsingCoopSaves();
}

void QueueMainMenuAction(std::string aAction, std::string aPayload) noexcept
{
    std::scoped_lock _(s_actionsLock);
    s_actions.emplace_back(std::move(aAction), std::move(aPayload));
}

void RunMainMenuIntegration() noexcept
{
    // the other campaign: straight back to the game, without the launcher's window
    if (s_restartAt != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= s_restartAt)
        Updater::Restart(true);

    RunUpdater();
    LanBrowser::SendResults();
    MenuSounds::PlayQueued();
    LoadOrderMatch::RunQueued();
    WorldSettings::Run();

    UI* pUI = UI::Get();
    if (!pUI)
        return;

    // New co-op characters can't stay in Start Me Up's pre-war memory: tried about once a second until its plugin is
    // loaded (also right before starting a new character)
    static int s_prewarFrames = 0;
    if (!s_blockedPrewarMemory && ++s_prewarFrames % 60 == 0 && IsGameDataLoaded())
        s_blockedPrewarMemory = StartMeUp::BlockPrewarMemory();

    // The pause menu and the main menu's screens past its main list, drawn by the overlay
    StartMenuBridge::Run(pUI);

    IMenu* pMainMenu = pUI->FindMenuByName("MainMenu");

    // The menu closed (a save is loading) or another one opened: its movie objects are gone
    if (pMainMenu != s_state.pMenu)
    {
        const std::string status = s_state.status;
        s_state.Forget();
        s_state.pMenu = pMainMenu;
        s_state.pMovie = pMainMenu ? static_cast<GFx::Movie*>(pMainMenu->pMovie) : nullptr;
        s_loadQueued = false;
        s_framesAtMenu = 0;
        CloseScreen(false);

        if (pMainMenu)
        {
            // Leaving the world leaves the server. Called here directly: queued work only runs in game.
            auto& transport = World::Get().GetTransport();
            if (transport.IsOnline())
            {
                spdlog::info("MainMenu: back at the main menu, leaving the server");
                transport.Close();
            }
            s_session = Session::kNone;

            // The host leaving ends the hosted world: its server stops (anyone still on it is told they lost it)
            StopLocalServer();
            World::Get().ctx().at<SteamService>().SetPlaying({}, 0);

            // Back from a game: a session chosen for it (e.g. a new character quit during the start) is over too
            if (s_wasInGame)
            {
                s_wasInGame = false;
                s_pendingServer.clear();
            }
            else
                s_state.status = status;

            if (!s_leaveReason.empty())
                s_state.status = std::exchange(s_leaveReason, {});

            // Without a session waiting (e.g. quit a co-op game): the normal saves again
            if (s_pendingServer.empty())
                UseNormalSaves();
        }
    }

    if (!pMainMenu || !s_state.pMovie)
    {
        if (!pMainMenu && IsInWorld())
            s_wasInGame = true;
        RunPendingConnect();
        RunSession();
        RunActions();
        return;
    }

    // Every half second: the menu rebuilds its list when coming back to it (e.g. from settings)
    static int s_frames = 0;
    if (++s_frames % 30 == 0 && !s_screenOpen)
        Install();

    // The overlay is the main menu: shown while the game's menu is at its main list with none of the game's own pop-ups
    // or screens over it (the game's menu hidden underneath), from a moment after the menu appears (its list filled,
    // the renderer ready). Closed for the game's screens, back once the game's menu is at its main list again.
    const std::string menuState = GetMenuState();
    const bool atMainList = s_state.list.IsObject() && menuState == "MAIN_STATE";
    const bool nativeMenuOpen = IsNativeMenuOpen(pUI);
    // With the overlay off for good (its process kept ending) this is the game's own menu again
    const bool overlayOff = GetOverlay().IsOff();
    // the screens the overlay draws (settings, help, ...: StartMenuBridge) keep the game's menu hidden and the overlay up
    const bool bridged = !overlayOff && !nativeMenuOpen && StartMenuBridge::Covers(menuState, false);
    if (s_screenOpen && (overlayOff || nativeMenuOpen || !atMainList))
        CloseScreen(!bridged, bridged);
    else if (!overlayOff && !s_screenOpen && atMainList && !nativeMenuOpen && !s_loadQueued && s_framesAtMenu >= 30)
        OpenScreen();
    ++s_framesAtMenu;

    if (std::exchange(s_state.multiplayerPressed, false) && !overlayOff && !s_screenOpen && !nativeMenuOpen)
        OpenScreen();

    // Opened before the game had loaded its plugins: once it has, CONTINUE and NEW GAME can be used
    if (s_screenOpen && !s_screenDataLoaded && IsGameDataLoaded())
    {
        s_screenDataLoaded = true;
        SendData();
    }

    // A friend's world to join through Steam: picked on the multiplayer screen (its characters, or a new one)
    if (s_steamJoin && s_screenOpen)
        RunSteamJoin();

    RunActions();
    RunQueuedLoad();
}

void JoinSteamWorld(uint64_t aHostSteamId, std::string aWorldName) noexcept
{
    s_steamJoin = aHostSteamId;
    s_steamJoinName = std::move(aWorldName);
    // From a world being played: saved and left first (the main menu then opens the friend's world)
    if (IsInWorld() && s_session != Session::kLeaving)
        LeaveToMainMenu("JOINING " + s_steamJoinName);
}

void SetHostedWorldPrivacy(const std::string& acWorldId, const std::string& acPrivacy) noexcept
{
    auto world = CoopWorlds::FindWorld(GetBaseSaveFolder(), acWorldId);
    if (!world || !world->hosted)
        return;
    world->privacy = acPrivacy;
    CoopWorlds::SaveWorld(*world);
}
