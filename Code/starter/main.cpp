// "Launch Reclamation Day.exe": sits next to the "Reclamation Day" folder in the beta package and starts the launcher
// inside it, so the extracted folder has one exe with the game's icon at the top.
#include <windows.h>

#include <string>

namespace
{
std::wstring GetOwnFolder()
{
    std::wstring path(MAX_PATH, L'\0');
    for (;;)
    {
        const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return {};
        if (length < path.size())
        {
            path.resize(length);
            break;
        }
        path.resize(path.size() * 2);
    }
    return path.substr(0, path.find_last_of(L'\\') + 1);
}
} // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR apArguments, int)
{
    const std::wstring folder = GetOwnFolder() + L"Reclamation Day";
    const std::wstring exe = folder + L"\\ReclamationDay.exe";

    // Arguments pass through to the launcher
    std::wstring commandLine = L"\"" + exe + L"\"";
    if (apArguments && *apArguments)
        commandLine += L" " + std::wstring(apArguments);

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startupInfo,
                        &processInfo))
    {
        const std::wstring message = L"Couldn't start " + exe +
                                     L"\n\nKeep \"Launch Reclamation Day.exe\" next to the \"Reclamation Day\" folder: "
                                     L"extract the whole zip, then start it again.";
        MessageBoxW(nullptr, message.c_str(), L"Fallout 4: Reclamation Day", MB_OK | MB_ICONERROR);
        return 1;
    }

    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return 0;
}
