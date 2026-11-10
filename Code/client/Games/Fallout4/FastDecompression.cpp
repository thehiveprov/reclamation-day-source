#include <Loading/WholeInflate.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <vector>

// Faster decompression of the game's archives. Everything the game unpacks goes through its zlib's inflate(): mesh and
// texture blocks from the archives as an area loads (on the job threads the frame also needs: up to a quarter of a core
// while walking into new places, 2026-10-29 profile), Scaleform movies, and more. A block handed over whole is now
// decompressed by libdeflate (several times faster), anything else by zlib as before (WholeInflate.h).
// RD_INFLATE_VERIFY=1 instead lets zlib do every block and checks libdeflate gets the same bytes (tests only).

namespace
{
// Reclamation Day ids are CommonLibF4's + 1 (VersionDb::LoadFallout4): zlib's inflate(z_stream*, int flush)
constexpr uint32_t kInflate = 224012;

// Our MCM page's settings, as Smooth loading reads them
constexpr wchar_t kSettingsFile[] = L"MCM\\Settings\\ReclamationDay.ini";
constexpr wchar_t kSettingsSection[] = L"SmoothLoading";

// A caller that never hands a block over whole (it feeds a stream in pieces) stops being tried after this many
constexpr uint32_t kCallerGiveUp = 16;
constexpr size_t kCallerSlots = 64;
// Totals go to the log at most this often (s), when they changed
constexpr int64_t kReportInterval = 60;

const bool s_verify = std::getenv("RD_INFLATE_VERIFY") != nullptr;

using TInflate = int(WholeInflate::Stream* apStream, int aFlush);
TInflate* RealInflate = nullptr;

std::atomic<uint64_t> s_calls{0};
std::atomic<uint64_t> s_whole{0};
std::atomic<uint64_t> s_wholeIn{0};
std::atomic<uint64_t> s_wholeOut{0};
std::atomic<uint64_t> s_wholeNs{0};
std::atomic<uint64_t> s_leftToZlib{0};
std::atomic<uint64_t> s_verified{0};
std::atomic<uint64_t> s_mismatches{0};
std::atomic<int64_t> s_lastReport{0};
std::atomic<uint64_t> s_lastReported{0};

// Per call site (the return address): how often a block came whole, how often not
struct Caller
{
    std::atomic<uintptr_t> Address{0};
    std::atomic<uint32_t> Whole{0};
    std::atomic<uint32_t> Pieces{0};
};
std::array<Caller, kCallerSlots> s_callers;

Caller* CallerFor(uintptr_t aAddress) noexcept
{
    const size_t start = (aAddress >> 4) % kCallerSlots;
    for (size_t i = 0; i < kCallerSlots; ++i)
    {
        Caller& caller = s_callers[(start + i) % kCallerSlots];
        uintptr_t current = caller.Address.load(std::memory_order_relaxed);
        if (current == aAddress)
            return &caller;
        if (current == 0 && caller.Address.compare_exchange_strong(current, aAddress, std::memory_order_relaxed))
            return &caller;
        if (current == aAddress)
            return &caller;
    }
    return nullptr;
}

libdeflate_decompressor* Decompressor() noexcept
{
    // one per thread for as long as it lives (a few KB): the game's loading threads stay for the whole game
    thread_local libdeflate_decompressor* s_pDecompressor = libdeflate_alloc_decompressor();
    return s_pDecompressor;
}

void MaybeReport() noexcept
{
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t last = s_lastReport.load(std::memory_order_relaxed);
    if (now - last < kReportInterval || !s_lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed))
        return;
    const uint64_t calls = s_calls.load();
    if (calls == s_lastReported.exchange(calls, std::memory_order_relaxed))
        return;

    if (s_verify)
    {
        spdlog::info("Fast decompression (verifying): {} calls, {} whole blocks checked, {} mismatches, {} left to zlib", calls, s_verified.load(), s_mismatches.load(),
                     s_leftToZlib.load());
        return;
    }
    const uint64_t whole = s_whole.load();
    spdlog::info("Fast decompression: {} calls, {} whole blocks ({:.1f} MB to {:.1f} MB, {:.1f} ms in all), {} left to zlib", calls, whole, s_wholeIn.load() / 1048576.0,
                 s_wholeOut.load() / 1048576.0, s_wholeNs.load() / 1e6, s_leftToZlib.load());
}

// zlib does it; then libdeflate does the same block into a buffer of its own, and both must agree
int VerifiedInflate(WholeInflate::Stream* apStream, int aFlush, WholeInflate::Format aFormat)
{
    const uint8_t* pIn = apStream->NextIn;
    const uint32_t availIn = apStream->AvailIn;
    uint8_t* pOut = apStream->NextOut;
    const int result = RealInflate(apStream, aFlush);
    if (result != WholeInflate::kStreamEnd)
    {
        ++s_leftToZlib;
        return result;
    }

    const uint32_t zlibIn = apStream->TotalIn;
    const uint32_t zlibOut = apStream->TotalOut;
    std::vector<uint8_t> out(zlibOut + 1);
    size_t used = 0;
    size_t written = 0;
    const libdeflate_result ours = aFormat == WholeInflate::Format::kZlib
                                       ? libdeflate_zlib_decompress_ex(Decompressor(), pIn, availIn, out.data(), out.size(), &used, &written)
                                       : libdeflate_deflate_decompress_ex(Decompressor(), pIn, availIn, out.data(), out.size(), &used, &written);
    ++s_verified;
    if (ours != LIBDEFLATE_SUCCESS || used != zlibIn || written != zlibOut || std::memcmp(out.data(), pOut, zlibOut) != 0)
    {
        ++s_mismatches;
        spdlog::warn("Fast decompression: libdeflate differs from zlib ({} in / {} out, ours result {} {} in / {} out)", zlibIn, zlibOut, static_cast<int>(ours), used, written);
    }
    return result;
}

int HookInflate(WholeInflate::Stream* apStream, int aFlush)
{
    ++s_calls;
    const auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const WholeInflate::Format format = apStream ? WholeInflate::Whole(*apStream, aFlush) : WholeInflate::Format::kNone;
    if (format == WholeInflate::Format::kNone)
        return RealInflate(apStream, aFlush);

    if (s_verify)
    {
        const int result = VerifiedInflate(apStream, aFlush, format);
        MaybeReport();
        return result;
    }

    Caller* pCaller = CallerFor(caller);
    if (pCaller && pCaller->Whole.load(std::memory_order_relaxed) == 0 && pCaller->Pieces.load(std::memory_order_relaxed) >= kCallerGiveUp)
    {
        ++s_leftToZlib;
        return RealInflate(apStream, aFlush);
    }

    const uint32_t availIn = apStream->AvailIn;
    const auto start = std::chrono::steady_clock::now();
    if (WholeInflate::TryWhole(*apStream, aFlush, Decompressor()))
    {
        s_wholeNs += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        ++s_whole;
        s_wholeIn += availIn - apStream->AvailIn;
        s_wholeOut += apStream->TotalOut;
        if (pCaller)
            pCaller->Whole.fetch_add(1, std::memory_order_relaxed);
        MaybeReport();
        return WholeInflate::kStreamEnd;
    }

    ++s_leftToZlib;
    if (pCaller)
        pCaller->Pieces.fetch_add(1, std::memory_order_relaxed);
    MaybeReport();
    return RealInflate(apStream, aFlush);
}
} // namespace

static TiltedPhoques::Initializer s_fastDecompressionHooks(
    []()
    {
        const auto settings = std::filesystem::current_path() / L"Data" / kSettingsFile;
        if (GetPrivateProfileIntW(kSettingsSection, L"bFastDecompression", 1, settings.c_str()) == 0)
        {
            spdlog::info("Fast decompression: off (MCM setting)");
            return;
        }

        const VersionDbPtr<uint8_t> inflate(kInflate);
        RealInflate = reinterpret_cast<TInflate*>(inflate.Get());
        // zlib 1.2.7's inflate starts by keeping its two arguments: mov [rsp+10h], edx; mov [rsp+8], rcx
        static constexpr uint8_t kStart[] = {0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08};
        if (!RealInflate || std::memcmp(RealInflate, kStart, sizeof(kStart)) != 0)
        {
            spdlog::warn("Fast decompression: off, the game's inflate isn't where it's expected (or another mod patched it)");
            RealInflate = nullptr;
            return;
        }
        TP_HOOK(&RealInflate, HookInflate);
        spdlog::info("Fast decompression: on{}", s_verify ? " (verifying every whole block against zlib)" : "");
    });
