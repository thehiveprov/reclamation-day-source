#include "CrashHandler.h"
#include <DbgHelp.h>
#include <Windows.h>
#include <shellapi.h>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <strsafe.h>
#include <thread>

#include <spdlog/async.h>

// Our runtime's C++ exception in flight on this thread (the one std::terminate was called for), null if none
extern "C" void** __cdecl __current_exception();

using time_point = std::chrono::system_clock::time_point;

namespace
{
// Memory around the crash site and around what each register points to, added to the dump: the game's code is mapped
// into the launcher rather than loaded from its own file, so without this a debugger can't even disassemble the crash.
struct ExtraDumpRegions
{
    std::vector<std::pair<uint64_t, uint32_t>> regions;
    size_t next = 0;
};

void AddRegion(ExtraDumpRegions& aRegions, HANDLE ahProcess, uint64_t aAddress, uint32_t aBefore, uint32_t aAfter)
{
    if (aAddress < 0x10000)
        return;

    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQueryEx(ahProcess, reinterpret_cast<void*>(aAddress), &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return;

    // Stay inside the committed region the address is in
    const uint64_t regionStart = reinterpret_cast<uint64_t>(info.BaseAddress);
    const uint64_t regionEnd = regionStart + info.RegionSize;
    const uint64_t start = std::max(aAddress - aBefore, regionStart);
    const uint64_t end = std::min(aAddress + aAfter, regionEnd);
    if (end > start)
        aRegions.regions.emplace_back(start, static_cast<uint32_t>(end - start));
}

BOOL CALLBACK DumpCallback(PVOID apParam, const PMINIDUMP_CALLBACK_INPUT apInput, PMINIDUMP_CALLBACK_OUTPUT apOutput)
{
    if (!apInput || !apOutput)
        return TRUE;

    if (apInput->CallbackType == MemoryCallback)
    {
        auto* pRegions = static_cast<ExtraDumpRegions*>(apParam);
        if (pRegions->next >= pRegions->regions.size())
            return FALSE; // no more regions

        const auto& [base, size] = pRegions->regions[pRegions->next++];
        apOutput->MemoryBase = base;
        apOutput->MemorySize = size;
        return TRUE;
    }

    return TRUE;
}
} // namespace

std::string SerializeTimePoint(const time_point& time, const std::string& format)
{
    std::time_t tt = std::chrono::system_clock::to_time_t(time);
    std::tm tm = *std::gmtime(&tt); // GMT (UTC)
    // std::tm tm = *std::localtime(&tt); //Locale time-zone, usually UTC by default.
    std::stringstream ss;
    ss << std::put_time(&tm, format.c_str());
    return ss.str();
}

namespace
{
// A game that draws no frame for this long is stuck: a dump shows where
constexpr uint64_t kFreezeMilliseconds = 20000;
// Behind a loading screen it may take far longer: a save's load can draw no frame while it reads (71 s for the co-op
// save on 2026-11-06 16:43, the main thread pumping the load's messages, then it went on), longer from a slow drive. A
// load that's really stuck (a lock nobody lets go of, as on 2026-10-17) still gets its dump, later
constexpr uint64_t kLoadingFreezeMilliseconds = 150000;

std::atomic<uint64_t> s_lastFrameTick{0}; // GetTickCount64 of the game's last frame, 0 before the first
std::atomic<bool> s_lastFrameLoading{false}; // the game's last frame was behind its loading screen
std::atomic<DWORD> s_mainThreadId{0};

// Dumps are written by a thread of our own: a crashing thread can have no stack left (a stack overflow), a frozen one
// can't write anything
HANDLE s_hDumpRequest = nullptr;
// Set once the crash's dump is written
std::atomic<bool> s_dumpDone{false};
PEXCEPTION_POINTERS s_pDumpException = nullptr;
DWORD s_dumpThreadId = 0;

enum class CrashKind
{
    kFatal,     // an exception that always ends the game, seen first chance
    kUnhandled, // an exception nothing handled: a C++ exception nothing caught in particular
    kAbort,     // abort(), which std::terminate ends with
};
CrashKind s_crashKind = CrashKind::kFatal;
// For kAbort: the C++ exception std::terminate was called for, if any
const EXCEPTION_RECORD* s_pAbortException = nullptr;

// Exceptions that end the game. Seen first chance (before the game's own handlers), so only these: others, C++
// exceptions in particular, are caught and handled all the time. Those nothing handles are dumped at the end instead
// (HookNtRaiseException, UnhandledExceptionHandler).
bool IsFatal(DWORD aCode) noexcept
{
    switch (aCode)
    {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_IN_PAGE_ERROR:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case 0xC0000374: // STATUS_HEAP_CORRUPTION
    case 0xC0000409: // STATUS_STACK_BUFFER_OVERRUN (also fail fast)
        return true;
    default: return false;
    }
}

void AddContextRegions(ExtraDumpRegions& aRegions, HANDLE ahProcess, const CONTEXT& acContext)
{
    AddRegion(aRegions, ahProcess, acContext.Rip, 0x800, 0x800);
    for (const uint64_t value : {acContext.Rax, acContext.Rbx, acContext.Rcx, acContext.Rdx, acContext.Rsi, acContext.Rdi, acContext.Rbp, acContext.R8, acContext.R9, acContext.R10,
                                 acContext.R11, acContext.R12, acContext.R13, acContext.R14, acContext.R15})
        AddRegion(aRegions, ahProcess, value, 0x100, 0x400);
    AddRegion(aRegions, ahProcess, acContext.Rsp, 0x100, 0x4000);
}

// No MiniDumpWithDataSegs: the game is mapped inside the launcher, so the data segments were most of the game's memory
// and the dumps 1.5-2 GB. Threads, their stacks, the memory those point to and the extra regions are what finding a
// crash needs.
constexpr auto kDumpSettings = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithProcessThreadData | MiniDumpWithHandleData |
                                                          MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpScanMemory);

// A new dump's path: crash_UTC_<time><suffix>.dmp next to the game, the oldest ones removed
std::filesystem::path NewDumpPath(const char* apSuffix)
{
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    auto subPath = std::filesystem::path(exePath).parent_path();

    CrashHandler::RemovePreviousDump(subPath);

    std::ostringstream oss;
    oss << "crash_" << SerializeTimePoint(std::chrono::system_clock::now(), "UTC_%Y-%m-%d_%H-%M-%S") << apSuffix << ".dmp";
    return subPath / oss.str();
}

// A crash's dump, written from inside the game (the crashed thread's exception and context are here); the game ends
// after it either way
void WriteDump(PEXCEPTION_POINTERS apException, DWORD aThreadId)
{
    const auto subPath = NewDumpPath("");
    const HANDLE hDumpFile = CreateFileW(subPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDumpFile == INVALID_HANDLE_VALUE)
    {
        spdlog::error("Couldn't create the dump {}: error {}", subPath.string(), GetLastError());
        return;
    }

    ExtraDumpRegions extra;
    if (apException && apException->ContextRecord)
        AddContextRegions(extra, GetCurrentProcess(), *apException->ContextRecord);

    MINIDUMP_CALLBACK_INFORMATION callback{};
    callback.CallbackRoutine = &DumpCallback;
    callback.CallbackParam = &extra;

    MINIDUMP_EXCEPTION_INFORMATION exception{};
    exception.ThreadId = aThreadId;
    exception.ExceptionPointers = apException;
    exception.ClientPointers = FALSE;

    const BOOL written = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hDumpFile, kDumpSettings, apException ? &exception : NULL, NULL, &callback);
    CloseHandle(hDumpFile);

    if (written)
        spdlog::error("Dump written: {}", subPath.filename().string());
    else
        spdlog::error("Writing the dump {} failed: error {}", subPath.filename().string(), GetLastError());
}

// How long a freeze's dump writer is waited for; it's never stopped (it may have the game's threads suspended)
constexpr DWORD kDumpWriterWait = 60000;

// A freeze's dump (crash_UTC_<time>_freeze.dmp), written by another process: our own exe started again with
// --write-dump (RunDumpWriter). MiniDumpWriteDump suspends every other thread of the process it's called in while it
// writes; called inside the game, a suspended thread holding a lock it needs (the heap during a load) left it waiting
// for good, and the whole game with it (2026-10-27 13:47, loading a save: a 0-byte dump, no CPU used, the game killed
// 7 minutes later). From outside, nothing it waits on is the game's.
void WriteFreezeDump()
{
    const DWORD mainThreadId = s_mainThreadId.load();
    if (const HANDLE hThread = mainThreadId ? OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, mainThreadId) : nullptr)
    {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL;
        bool read = false;
        if (SuspendThread(hThread) != static_cast<DWORD>(-1))
        {
            read = GetThreadContext(hThread, &context) != FALSE;
            ResumeThread(hThread);
        }
        CloseHandle(hThread);
        // (logged once the thread runs again: logging can wait on a lock the suspended thread holds)
        if (read)
            spdlog::error("Main thread is at {:#x}", context.Rip);
    }

    const auto dumpPath = NewDumpPath("_freeze");
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    const std::wstring folder = std::filesystem::path(exePath).parent_path().wstring();
    std::wstring commandLine = L"\"" + std::wstring(exePath) + L"\" --write-dump " + std::to_wstring(GetCurrentProcessId()) + L" " + std::to_wstring(mainThreadId) +
                               L" \"" + dumpPath.wstring() + L"\"";

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};
    if (!CreateProcessW(exePath, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, folder.c_str(), &startupInfo, &processInfo))
    {
        spdlog::error("Couldn't start the dump writer: error {}", GetLastError());
        return;
    }
    CloseHandle(processInfo.hThread);

    DWORD exitCode = STILL_ACTIVE;
    if (WaitForSingleObject(processInfo.hProcess, kDumpWriterWait) == WAIT_OBJECT_0)
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hProcess);

    if (exitCode == 0)
        spdlog::error("Dump written: {}", dumpPath.filename().string());
    else if (exitCode == STILL_ACTIVE)
        spdlog::error("The dump writer is still writing {} after {} s", dumpPath.filename().string(), kDumpWriterWait / 1000);
    else
        spdlog::error("Writing the dump {} failed: the writer ended with {}", dumpPath.filename().string(), exitCode);
}

// What MSVC throws with a C++ exception (ehdata.h): offsets relative to the module the throw was compiled in
struct ThrowInfo
{
    uint32_t attributes;
    int32_t destructor;
    int32_t forwardCompat;
    int32_t catchableTypes; // CatchableTypeArray
};

struct CatchableTypeArray
{
    int32_t count;
    int32_t types[1]; // CatchableType: the thrown type first, then its bases
};

struct CatchableType
{
    uint32_t properties;
    int32_t type; // TypeDescriptor
    int32_t thisDisplacement[3];
    int32_t size;
    int32_t copyFunction;
};

struct TypeDescriptor
{
    const void* pVTable;
    void* pSpare;
    char name[1]; // decorated: ".?AVbad_function_call@std@@"
};

const void* Readable(uint64_t aAddress, size_t aSize) noexcept
{
    MEMORY_BASIC_INFORMATION info{};
    if (aAddress < 0x10000 || !VirtualQuery(reinterpret_cast<void*>(aAddress), &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return nullptr;
    return aAddress + aSize <= reinterpret_cast<uint64_t>(info.BaseAddress) + info.RegionSize ? reinterpret_cast<const void*>(aAddress) : nullptr;
}

// "std::bad_function_call (thrown in msvcp110.dll)" for a C++ exception (code 0xE06D7363), empty for other records or
// one that can't be read. Its parameters: a magic number, the thrown object, its ThrowInfo and the module the
// ThrowInfo's offsets are relative to. Read with no exception handling of our own: a fault here would be a crash the
// handlers above wait on.
std::string DescribeThrown(const EXCEPTION_RECORD* apRecord)
{
    if (!Readable(reinterpret_cast<uint64_t>(apRecord), sizeof(EXCEPTION_RECORD)) || apRecord->ExceptionCode != 0xE06D7363 || apRecord->NumberParameters < 4)
        return {};
    const ULONG_PTR magic = apRecord->ExceptionInformation[0];
    if (magic != 0x19930520 && magic != 0x19930521 && magic != 0x19930522)
        return {};

    const uint64_t module = apRecord->ExceptionInformation[3];
    const auto* pThrowInfo = static_cast<const ThrowInfo*>(Readable(apRecord->ExceptionInformation[2], sizeof(ThrowInfo)));
    if (!pThrowInfo || !module)
        return {};
    const auto* pTypes = static_cast<const CatchableTypeArray*>(Readable(module + pThrowInfo->catchableTypes, sizeof(CatchableTypeArray)));
    if (!pTypes || pTypes->count < 1)
        return {};
    const auto* pType = static_cast<const CatchableType*>(Readable(module + pTypes->types[0], sizeof(CatchableType)));
    if (!pType)
        return {};

    const uint64_t name = module + pType->type + offsetof(TypeDescriptor, name);
    char decorated[256]{};
    for (size_t i = 0; i + 1 < sizeof(decorated) && Readable(name + i, 1); ++i)
    {
        decorated[i] = *reinterpret_cast<const char*>(name + i);
        if (!decorated[i])
            break;
    }
    if (decorated[0] != '.')
        return {};

    // Without the dot it undecorates as a type: "class std::bad_function_call"
    std::string description = decorated;
    char undecorated[512]{};
    if (UnDecorateSymbolName(decorated + 1, undecorated, sizeof(undecorated), UNDNAME_32_BIT_DECODE | UNDNAME_NO_ARGUMENTS))
    {
        description = undecorated;
        for (const std::string_view prefix : {"class ", "struct "})
        {
            if (description.starts_with(prefix))
                description.erase(0, prefix.size());
        }
    }

    char modulePath[MAX_PATH]{};
    if (GetModuleFileNameA(reinterpret_cast<HMODULE>(module), modulePath, sizeof(modulePath)))
        description += fmt::format(" (thrown in {})", std::filesystem::path(modulePath).filename().string());
    return description;
}

DWORD WINAPI DumpThread(LPVOID)
{
    bool frozen = false;
    for (;;)
    {
        // A crash
        if (WaitForSingleObject(s_hDumpRequest, 1000) == WAIT_OBJECT_0)
        {
            const EXCEPTION_RECORD* pRecord = s_pDumpException->ExceptionRecord;
            const char* pMainThread = s_dumpThreadId == s_mainThreadId.load() ? " (the game's main thread)" : "";
            if (s_crashKind == CrashKind::kAbort)
            {
                if (const std::string thrown = DescribeThrown(s_pAbortException); !thrown.empty())
                    spdlog::error("Crash occurred! abort() on thread {}{}: std::terminate, nothing caught the C++ exception {}", s_dumpThreadId, pMainThread, thrown);
                else
                    spdlog::error("Crash occurred! abort() on thread {}{}", s_dumpThreadId, pMainThread);
            }
            else if (const std::string thrown = DescribeThrown(pRecord); !thrown.empty())
                spdlog::error("Crash occurred! Nothing caught the C++ exception {} on thread {}{}", thrown, s_dumpThreadId, pMainThread);
            else
                spdlog::error("Crash occurred! {} {:#x} at {} on thread {}{}", s_crashKind == CrashKind::kUnhandled ? "Nothing handled exception" : "Exception",
                              pRecord->ExceptionCode, pRecord->ExceptionAddress, s_dumpThreadId, pMainThread);
            WriteDump(s_pDumpException, s_dumpThreadId);
            CrashHandler::FlushLog();
            s_dumpDone = true;
            continue;
        }

        // A freeze: once per freeze
        const uint64_t lastFrame = s_lastFrameTick.load();
        if (lastFrame == 0)
            continue;

        const uint64_t since = GetTickCount64() - lastFrame;
        const bool loading = s_lastFrameLoading.load();
        if (since < (loading ? kLoadingFreezeMilliseconds : kFreezeMilliseconds))
        {
            if (frozen)
                spdlog::warn("The game is drawing frames again");
            frozen = false;
            continue;
        }

        if (frozen)
            continue;
        frozen = true;

        spdlog::error("The game has drawn no frame for {} s{} (frozen?), saving a dump of where it is", since / 1000, loading ? " behind its loading screen" : "");
        WriteFreezeDump();
        CrashHandler::FlushLog();
    }
}

// Hands a crash to the dump thread, then waits until the dump is written, or a minute: spinning, as a wait's calls could
// need more stack than a thread that overflowed its own has left (the game's job threads have none kept for this; one
// ended the game with an empty dump, 2026-10-14). False when a crash was handed over before: another thread crashing
// meanwhile (one fault can hit several of the game's parallel jobs), or the same exception seen first chance, waits for
// that one's dump, going on would end the game before it's written.
bool DumpCrash(PEXCEPTION_POINTERS apException, CrashKind aKind, const EXCEPTION_RECORD* apAbortException = nullptr) noexcept
{
    static std::atomic<int> s_alreadyCrashed{0};

    const bool first = s_alreadyCrashed++ == 0;
    if (!s_hDumpRequest)
        return false;

    // Nothing more on this thread than asking for the dump: after a stack overflow there's almost no stack left
    if (first)
    {
        s_pDumpException = apException;
        s_dumpThreadId = GetCurrentThreadId();
        s_crashKind = aKind;
        s_pAbortException = apAbortException;
        SetEvent(s_hDumpRequest);
    }

    for (const uint64_t until = GetTickCount64() + 60000; !s_dumpDone.load() && GetTickCount64() < until;)
        YieldProcessor();
    return first;
}

// An exception nothing handled, or abort(): the game ends here, after our dump. Left to Windows, error reporting saves a
// dump of its own when LocalDumps is enabled (EA's and NVIDIA's installers enable it): 1.8 GB per crash in
// AppData\Local\CrashDumps.
void EndGame(PEXCEPTION_POINTERS apException, CrashKind aKind, UINT aExitCode, const EXCEPTION_RECORD* apAbortException = nullptr) noexcept
{
    // Dumped already when the exception always ends the game (VectoredExceptionHandler, first chance)
    if (!DumpCrash(apException, aKind, apAbortException) && aKind == CrashKind::kUnhandled)
    {
        const std::string thrown = DescribeThrown(apException->ExceptionRecord);
        spdlog::error("Unhandled exception {:#x}{}, ending the game", apException->ExceptionRecord->ExceptionCode,
                      thrown.empty() ? std::string() : " (C++ exception " + thrown + ")");
        CrashHandler::FlushLog();
    }
    TerminateProcess(GetCurrentProcess(), aExitCode);
}

// Where an exception nothing handled ends up: dispatch found no handler, so it's raised a second time (for a debugger)
// and Windows error reporting ends the game. A thread's top-level filter (UnhandledExceptionHandler) can't take one
// thrown in the game's code: Windows can't unwind the game's frames (the loader only knows our exe's own function
// table, ExeLoader::LoadExceptionTable), the search falls off the stack before reaching it. The dog adoption crash
// (2026-10-17, std::bad_function_call thrown in msvcp110 under a script call) ended the game this way without a word.
using TNtRaiseException = LONG(PEXCEPTION_RECORD apRecord, PCONTEXT apContext, BOOLEAN aFirstChance);
TNtRaiseException* RealNtRaiseException = nullptr;
bool s_raiseHookFailed = false;

LONG HookNtRaiseException(PEXCEPTION_RECORD apRecord, PCONTEXT apContext, BOOLEAN aFirstChance)
{
    // First chance comes here only with a debugger attached, which gets the second one too
    if (!aFirstChance && !IsDebuggerPresent())
    {
        EXCEPTION_POINTERS pointers{apRecord, apContext};
        EndGame(&pointers, CrashKind::kUnhandled, apRecord->ExceptionCode);
    }
    return RealNtRaiseException(apRecord, apContext, aFirstChance);
}

// abort(), which std::terminate ends with (a C++ exception left a noexcept function or a std::thread's), ours or the
// game's runtime's (msvcr110, registered in OnFrame): SIGABRT comes before a fail-fast that no handler sees. Only our
// runtime's exception in flight can be named; the game's abort() logs without one.
void __cdecl OnAbort(int)
{
    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = 0x40000015; // STATUS_FATAL_APP_EXIT, what abort() reports
    record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
    EXCEPTION_POINTERS pointers{&record, &context};

    void** ppInFlight = __current_exception();
    EndGame(&pointers, CrashKind::kAbort, 3, ppInFlight ? static_cast<const EXCEPTION_RECORD*>(*ppInFlight) : nullptr);
}
} // namespace

LONG WINAPI VectoredExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo)
{
    if (IsFatal(pExceptionInfo->ExceptionRecord->ExceptionCode))
        DumpCrash(pExceptionInfo, CrashKind::kFatal);

    return EXCEPTION_CONTINUE_SEARCH;
}

// A crash nothing handled that reached the top of its thread (one whose stack Windows can walk, see
// HookNtRaiseException)
LONG WINAPI UnhandledExceptionHandler(PEXCEPTION_POINTERS pExceptionInfo)
{
    EndGame(pExceptionInfo, CrashKind::kUnhandled, pExceptionInfo->ExceptionRecord->ExceptionCode);
    return EXCEPTION_EXECUTE_HANDLER;
}

CrashHandler::CrashHandler()
{
    s_hDumpRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (s_hDumpRequest)
    {
        if (const HANDLE hThread = CreateThread(nullptr, 256 * 1024, &DumpThread, nullptr, 0, nullptr))
            CloseHandle(hThread);
    }

    AddVectoredExceptionHandler(1, &VectoredExceptionHandler);
    SetUnhandledExceptionFilter(&UnhandledExceptionHandler);

    // MinHook replaces the pointer with the way to the original when the hook is in
    const auto pNtRaiseException = reinterpret_cast<TNtRaiseException*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtRaiseException"));
    RealNtRaiseException = pNtRaiseException;
    if (RealNtRaiseException)
        TP_HOOK_IMMEDIATE(&RealNtRaiseException, &HookNtRaiseException);
    s_raiseHookFailed = RealNtRaiseException == pNtRaiseException;

    signal(SIGABRT, &OnAbort);
}

CrashHandler::~CrashHandler()
{
}

void CrashHandler::FlushLog() noexcept
{
    spdlog::default_logger()->flush();
    // The flush is queued too: written once the thread has taken everything before it
    const auto pPool = spdlog::thread_pool();
    for (int i = 0; pPool && i < 100 && pPool->queue_size() > 0; ++i)
        Sleep(10);
    if (pPool)
        Sleep(20);
}

void CrashHandler::OnFrame(bool aLoadingScreen) noexcept
{
    if (s_mainThreadId.load() == 0)
    {
        s_mainThreadId = GetCurrentThreadId();
        // Room for the exception handler when the main thread overflows its stack
        ULONG guarantee = 64 * 1024;
        SetThreadStackGuarantee(&guarantee);

        // Here rather than in the constructor: the log exists by now, and so does the game's runtime
        if (s_raiseHookFailed)
            spdlog::warn("Crash handler: NtRaiseException isn't hooked, a C++ exception nothing catches in the game's code ends the game without a dump");

        // The game's runtime keeps its own signal handlers: its abort() never reaches the one we gave ours
        using TSignal = void (*(__cdecl*)(int, void(__cdecl*)(int)))(int);
        if (const HMODULE hGameRuntime = GetModuleHandleW(L"msvcr110.dll"))
        {
            if (const auto pSignal = reinterpret_cast<TSignal>(GetProcAddress(hGameRuntime, "signal")))
                pSignal(SIGABRT, &OnAbort);
        }
    }

    s_lastFrameLoading = aLoadingScreen;
    s_lastFrameTick = GetTickCount64();
}

void CrashHandler::RemovePreviousDump(std::filesystem::path path)
{
    // Keep the most recent dumps instead of deleting all of them: several game instances can share this folder, and
    // a second crash used to wipe the first one's dump before anyone could look at it.
    constexpr size_t kDumpsToKeep = 3;

    // non-throwing overloads: this runs inside the exception handler, a missing folder must not throw
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> dumps;
    for (auto& entry : std::filesystem::directory_iterator(path, ec))
    {
        const auto filename = entry.path().filename().string();
        if (filename.starts_with("crash_") && entry.path().extension() == ".dmp")
            dumps.push_back(entry);
    }

    if (dumps.size() < kDumpsToKeep)
        return;

    // Names are crash_UTC_<timestamp>.dmp, so name order is age order; keep room for the dump about to be written
    std::sort(dumps.begin(), dumps.end(), [](const auto& acLhs, const auto& acRhs) { return acLhs.path().filename() < acRhs.path().filename(); });
    for (size_t i = 0; i + kDumpsToKeep <= dumps.size(); ++i)
        DeleteFileA(dumps[i].path().string().c_str());
}

void CrashHandler::CompressDumps(std::filesystem::path path)
{
    // Zipped crash_*.zip next to the game, the .dmp removed once the zip is complete. Only one game instance does it
    // at a time; a dump still being written (another instance crashing right now) is left for next time.
    constexpr size_t kZipsToKeep = 10;

    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"Local\\ReclamationDayDumpCompression");
    if (!hMutex)
        return;
    if (const DWORD result = WaitForSingleObject(hMutex, 0); result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
    {
        // Another instance is compressing
        CloseHandle(hMutex);
        return;
    }

    std::error_code ec;
    std::vector<std::filesystem::path> dumps;
    for (auto& entry : std::filesystem::directory_iterator(path, ec))
    {
        const auto filename = entry.path().filename().string();

        // Left over from a compression that was interrupted (game closed meanwhile)
        if (filename.starts_with("crash_") && filename.ends_with(".partial.zip"))
        {
            std::error_code removeError;
            std::filesystem::remove(entry.path(), removeError);
            continue;
        }

        if (!filename.starts_with("crash_") || entry.path().extension() != ".dmp")
            continue;

        const auto age = std::filesystem::file_time_type::clock::now() - entry.last_write_time(ec);
        if (!ec && age > std::chrono::minutes(1))
            dumps.push_back(entry.path());
    }

    spdlog::info("Crash dumps to compress: {}", dumps.size());

    wchar_t systemDir[MAX_PATH];
    GetSystemDirectoryW(systemDir, MAX_PATH);
    const auto tar = std::filesystem::path(systemDir) / L"tar.exe";

    for (const auto& dump : dumps)
    {
        auto zip = dump;
        zip.replace_extension(".zip");
        auto partial = dump;
        partial.replace_extension(".partial.zip");

        // tar -a picks the format from the extension; run from the folder so the zip holds just the file name
        std::wstring commandLine = L"\"" + tar.wstring() + L"\" -a -c -f \"" + partial.filename().wstring() + L"\" \"" + dump.filename().wstring() + L"\"";

        STARTUPINFOW startupInfo{sizeof(startupInfo)};
        PROCESS_INFORMATION processInfo{};
        if (!CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, path.wstring().c_str(), &startupInfo, &processInfo))
        {
            spdlog::warn("Couldn't start tar to compress {}: error {}", dump.filename().string(), GetLastError());
            break;
        }

        WaitForSingleObject(processInfo.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
        CloseHandle(processInfo.hProcess);
        CloseHandle(processInfo.hThread);

        if (exitCode != 0 || !std::filesystem::exists(partial, ec))
        {
            spdlog::warn("Compressing {} failed (tar exit code {})", dump.filename().string(), exitCode);
            std::filesystem::remove(partial, ec);
            continue;
        }

        std::filesystem::rename(partial, zip, ec);
        if (ec)
        {
            spdlog::warn("Couldn't rename the compressed dump {}: {}", zip.filename().string(), ec.message());
            continue;
        }

        std::filesystem::remove(dump, ec);
        spdlog::info("Compressed crash dump {}", zip.filename().string());
    }

    // Keep the newest zips only
    std::vector<std::filesystem::path> zips;
    for (auto& entry : std::filesystem::directory_iterator(path, ec))
    {
        const auto filename = entry.path().filename().string();
        if (filename.starts_with("crash_") && entry.path().extension() == ".zip" && filename.find(".partial") == std::string::npos)
            zips.push_back(entry.path());
    }
    std::sort(zips.begin(), zips.end());
    for (size_t i = 0; i + kZipsToKeep < zips.size(); ++i)
        std::filesystem::remove(zips[i], ec);

    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
}

// Our exe started with --write-dump <process id> <its main thread's id> <dump path>, by a frozen game's watchdog
// (WriteFreezeDump): that process's dump written from here, the main thread's code and what its registers point to
// added as the game's own dumps add them. 0 when written; -1 when the command line isn't one (the launcher goes on).
int RunDumpWriter(int argc, char** argv)
{
    if (argc < 2 || strcmp(argv[1], "--write-dump") != 0)
        return -1;

    int count = 0;
    LPWSTR* ppArguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!ppArguments || count < 5)
        return 2;
    const DWORD processId = wcstoul(ppArguments[2], nullptr, 10);
    const DWORD mainThreadId = wcstoul(ppArguments[3], nullptr, 10);
    const std::wstring dumpPath = ppArguments[4];
    LocalFree(ppArguments);

    const HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ | PROCESS_DUP_HANDLE, FALSE, processId);
    if (!hProcess)
        return 3;

    ExtraDumpRegions extra;
    if (const HANDLE hThread = mainThreadId ? OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, mainThreadId) : nullptr)
    {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (SuspendThread(hThread) != static_cast<DWORD>(-1))
        {
            if (GetThreadContext(hThread, &context))
                AddContextRegions(extra, hProcess, context);
            ResumeThread(hThread);
        }
        CloseHandle(hThread);
    }

    const HANDLE hDumpFile = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hDumpFile == INVALID_HANDLE_VALUE)
    {
        CloseHandle(hProcess);
        return 4;
    }

    MINIDUMP_CALLBACK_INFORMATION callback{};
    callback.CallbackRoutine = &DumpCallback;
    callback.CallbackParam = &extra;
    const BOOL written = MiniDumpWriteDump(hProcess, processId, hDumpFile, kDumpSettings, NULL, NULL, &callback);
    CloseHandle(hDumpFile);
    CloseHandle(hProcess);
    return written ? 0 : 5;
}
