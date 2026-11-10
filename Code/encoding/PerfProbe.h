#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

// What Reclamation Day costs, for performance work: the frames (their times, and our own share of each), the time
// between ticks on the server, and the network traffic by message type. Off unless the environment variable RD_PERF is
// set (a host's server inherits it from the game). Report() gives a line for the log every 5 s. Any thread.
namespace PerfProbe
{
inline const bool g_enabled = std::getenv("RD_PERF") != nullptr;

struct Traffic
{
    std::array<std::atomic<uint64_t>, 256> bytes{};
    std::array<std::atomic<uint32_t>, 256> count{};
};
inline Traffic g_sent;
inline Traffic g_received;
// what the sent messages took on the wire, compressed
inline std::atomic<uint64_t> g_sentOnWire{0};

inline std::mutex g_frameLock;
// milliseconds, this window's
inline std::vector<float> g_frames;
inline std::vector<float> g_ours;
inline auto g_windowStart = std::chrono::steady_clock::now();

[[nodiscard]] inline bool Enabled() noexcept
{
    return g_enabled;
}

// Named parts of a frame (a message type's handling, one of our updates), for finding what makes a frame slow: Frame()
// names the biggest when our share passes kSlowOursMs. Main thread only; the names must be string literals.
constexpr double kSlowOursMs = 20.0;
struct SectionTime
{
    const char* name;
    int index; // a message opcode, or -1
    double ms;
    uint32_t count;
};
inline std::vector<SectionTime> g_sections;
// the same, summed over the report window
inline std::vector<SectionTime> g_windowSections;

inline void AddSection(const char* apName, int aIndex, double aMs) noexcept
{
    for (auto& section : g_sections)
    {
        if (section.name == apName && section.index == aIndex)
        {
            section.ms += aMs;
            ++section.count;
            return;
        }
    }
    g_sections.push_back({apName, aIndex, aMs, 1});
}

// Times its scope as a section of the frame: PerfProbe::Section _("RunSpawnUpdates");
struct Section
{
    explicit Section(const char* apName, int aIndex = -1) noexcept
        : m_name(apName)
        , m_index(aIndex)
        , m_start(g_enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{})
    {
    }
    ~Section()
    {
        if (g_enabled)
            AddSection(m_name, m_index, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count());
    }
    Section(const Section&) = delete;
    Section& operator=(const Section&) = delete;

private:
    const char* m_name;
    int m_index;
    std::chrono::steady_clock::time_point m_start;
};

// Our share of a hook into the game's own frame, from any thread: calls and time, minus the game's own function it
// calls (wrap that call in a GameCall). One static HookStat per hook:
//   static PerfProbe::HookStat s_stat("ActorProcess");
//   PerfProbe::Hook _(s_stat); ... { PerfProbe::GameCall __; RealActorProcess(...); }
struct HookStat;
inline std::mutex g_hookLock;
inline std::vector<HookStat*> g_hooks;
struct HookStat
{
    explicit HookStat(const char* apName) noexcept
        : name(apName)
    {
        std::scoped_lock _(g_hookLock);
        g_hooks.push_back(this);
    }
    const char* name;
    std::atomic<uint64_t> ns{0};
    std::atomic<uint32_t> calls{0};
};

struct Hook
{
    explicit Hook(HookStat& aStat) noexcept
        : m_stat(aStat)
    {
        if (!g_enabled)
            return;
        m_prev = t_current;
        t_current = this;
        m_start = std::chrono::steady_clock::now();
    }
    ~Hook()
    {
        if (!g_enabled)
            return;
        const auto total = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
        const int64_t ours = total - m_excludedNs;
        m_stat.ns += static_cast<uint64_t>(ours > 0 ? ours : 0);
        ++m_stat.calls;
        t_current = m_prev;
        // a hook inside another: its whole time isn't the outer one's
        if (m_prev)
            m_prev->m_excludedNs += total;
    }
    Hook(const Hook&) = delete;
    Hook& operator=(const Hook&) = delete;

    inline static thread_local Hook* t_current = nullptr;
    int64_t m_excludedNs = 0;

private:
    HookStat& m_stat;
    Hook* m_prev = nullptr;
    std::chrono::steady_clock::time_point m_start;
};

// The game's own function called from a hook: not counted as ours
struct GameCall
{
    GameCall() noexcept
        : m_hook(g_enabled ? Hook::t_current : nullptr)
    {
        if (m_hook)
            m_start = std::chrono::steady_clock::now();
    }
    ~GameCall()
    {
        if (m_hook)
            m_hook->m_excludedNs += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start).count();
    }
    GameCall(const GameCall&) = delete;
    GameCall& operator=(const GameCall&) = delete;

private:
    Hook* m_hook;
    std::chrono::steady_clock::time_point m_start;
};

// Parts of the frame outside our updates (main thread): the time Present blocked (waiting on the graphics card or the
// frame cap) and our drawing at the end of the frame (outline, imgui, overlay)
inline std::vector<float> g_present;
inline std::vector<float> g_ourDrawing;
// our drawing inside the game's end of frame, set by it and read by the present hook around it
inline double g_lastOurDrawingMs = 0.0;
inline void FramePart(std::vector<float>& aTimes, double aMs) noexcept
{
    if (!g_enabled)
        return;
    std::scoped_lock _(g_frameLock);
    aTimes.push_back(static_cast<float>(aMs));
}

// A frame (or a server tick) and how much of it was our own code. Returns "Perf: slow frame ..." naming the biggest
// sections when our share passed kSlowOursMs, else empty; the sections start over every frame.
inline std::string Frame(double aFrameSeconds, double aOursSeconds) noexcept
{
    if (!g_enabled)
        return {};
    {
        std::scoped_lock _(g_frameLock);
        g_frames.push_back(static_cast<float>(aFrameSeconds * 1000.0));
        g_ours.push_back(static_cast<float>(aOursSeconds * 1000.0));
    }
    std::string out;
    if (aOursSeconds * 1000.0 >= kSlowOursMs && !g_sections.empty())
    {
        std::sort(g_sections.begin(), g_sections.end(), [](const SectionTime& a, const SectionTime& b) { return a.ms > b.ms; });
        char text[128];
        std::snprintf(text, sizeof(text), "Perf: slow frame %.1f ms, ours %.1f ms:", aFrameSeconds * 1000.0, aOursSeconds * 1000.0);
        out = text;
        for (size_t i = 0; i < g_sections.size() && i < 8; ++i)
        {
            const auto& section = g_sections[i];
            if (section.index >= 0)
                std::snprintf(text, sizeof(text), " %s%d %.1f ms x%u", section.name, section.index, section.ms, section.count);
            else
                std::snprintf(text, sizeof(text), " %s %.1f ms x%u", section.name, section.ms, section.count);
            out += text;
        }
    }
    for (const auto& section : g_sections)
    {
        bool found = false;
        for (auto& total : g_windowSections)
        {
            if (total.name == section.name && total.index == section.index)
            {
                total.ms += section.ms;
                total.count += section.count;
                found = true;
                break;
            }
        }
        if (!found)
            g_windowSections.push_back(section);
    }
    g_sections.clear();
    return out;
}

// A message sent: its size, and what it took on the wire
inline void Sent(uint8_t aOpcode, size_t aBytes, size_t aWireBytes) noexcept
{
    if (!g_enabled)
        return;
    g_sent.bytes[aOpcode] += aBytes;
    ++g_sent.count[aOpcode];
    g_sentOnWire += aWireBytes;
}

inline void Received(uint8_t aOpcode, size_t aBytes) noexcept
{
    if (!g_enabled)
        return;
    g_received.bytes[aOpcode] += aBytes;
    ++g_received.count[aOpcode];
}

namespace Detail
{
// The traffic of the window as "kB/s total; opcode:bytes/s x messages/s ..." for the busiest message types
inline std::string Describe(Traffic& aTraffic, double aSeconds, char aSide)
{
    struct Entry
    {
        int opcode;
        uint64_t bytes;
        uint32_t count;
    };
    std::vector<Entry> entries;
    uint64_t total = 0;
    for (int i = 0; i < 256; ++i)
    {
        const uint64_t bytes = aTraffic.bytes[i].exchange(0);
        const uint32_t count = aTraffic.count[i].exchange(0);
        total += bytes;
        if (count)
            entries.push_back({i, bytes, count});
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.bytes > b.bytes; });
    char text[96];
    std::snprintf(text, sizeof(text), "%.2f kB/s", static_cast<double>(total) / 1024.0 / aSeconds);
    std::string out = text;
    for (size_t i = 0; i < entries.size() && i < 8; ++i)
    {
        std::snprintf(text, sizeof(text), " %c%d:%.0fB/s*%.1f/s", aSide, entries[i].opcode, static_cast<double>(entries[i].bytes) / aSeconds,
                      static_cast<double>(entries[i].count) / aSeconds);
        out += text;
    }
    return out;
}

inline std::string DescribeTimes(std::vector<float>& aTimes)
{
    if (aTimes.empty())
        return "none";
    std::sort(aTimes.begin(), aTimes.end());
    double sum = 0.0;
    for (const float time : aTimes)
        sum += time;
    char text[96];
    std::snprintf(text, sizeof(text), "avg %.2f p95 %.2f max %.2f ms", sum / static_cast<double>(aTimes.size()), aTimes[aTimes.size() * 95 / 100], aTimes.back());
    return text;
}
} // namespace Detail

// Once 5 s have gone by since the last: a line describing them (sent messages are opcodes of the side's own sending
// kind, s<n> for the server's, c<n> for the client's), else empty
inline std::string Report(bool aServer)
{
    if (!g_enabled)
        return {};
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - g_windowStart).count();
    if (seconds < 5.0)
        return {};
    g_windowStart = now;

    std::vector<float> frames;
    std::vector<float> ours;
    std::vector<float> present;
    std::vector<float> drawing;
    {
        std::scoped_lock _(g_frameLock);
        frames.swap(g_frames);
        ours.swap(g_ours);
        present.swap(g_present);
        drawing.swap(g_ourDrawing);
    }
    const size_t frameCount = frames.size();
    std::string out = "Perf: " + std::to_string(frameCount) + (aServer ? " ticks " : " frames ") + Detail::DescribeTimes(frames) + "; ours " + Detail::DescribeTimes(ours);
    if (!aServer)
    {
        out += "; present waits " + Detail::DescribeTimes(present) + "; our drawing " + Detail::DescribeTimes(drawing);

        // our updates by part, per frame
        std::sort(g_windowSections.begin(), g_windowSections.end(), [](const SectionTime& a, const SectionTime& b) { return a.ms > b.ms; });
        const double perFrame = frameCount ? static_cast<double>(frameCount) : 1.0;
        out += "\nPerf: our updates per frame:";
        char text[128];
        for (size_t i = 0; i < g_windowSections.size() && i < 14; ++i)
        {
            const auto& section = g_windowSections[i];
            if (section.index >= 0)
                std::snprintf(text, sizeof(text), " %s%d %.3f", section.name, section.index, section.ms / perFrame);
            else
                std::snprintf(text, sizeof(text), " %s %.3f", section.name, section.ms / perFrame);
            out += text;
        }
        g_windowSections.clear();

        // our hooks inside the game's frame, per frame and calls per second
        struct HookLine
        {
            const char* name;
            double ms;
            uint32_t calls;
        };
        std::vector<HookLine> hooks;
        double hookTotal = 0.0;
        {
            std::scoped_lock _(g_hookLock);
            for (HookStat* pStat : g_hooks)
            {
                const uint64_t ns = pStat->ns.exchange(0);
                const uint32_t calls = pStat->calls.exchange(0);
                if (!calls)
                    continue;
                const double ms = static_cast<double>(ns) / 1e6;
                hookTotal += ms;
                hooks.push_back({pStat->name, ms, calls});
            }
        }
        std::sort(hooks.begin(), hooks.end(), [](const HookLine& a, const HookLine& b) { return a.ms > b.ms; });
        std::snprintf(text, sizeof(text), "\nPerf: our hooks %.3f ms per frame:", hookTotal / perFrame);
        out += text;
        for (size_t i = 0; i < hooks.size() && i < 16; ++i)
        {
            std::snprintf(text, sizeof(text), " %s %.3f (%.0f/s)", hooks[i].name, hooks[i].ms / perFrame, static_cast<double>(hooks[i].calls) / seconds);
            out += text;
        }
        out += "\nPerf: traffic";
    }
    out += "; sent " + Detail::Describe(g_sent, seconds, aServer ? 's' : 'c');
    char wire[48];
    std::snprintf(wire, sizeof(wire), "; on the wire %.2f kB/s", static_cast<double>(g_sentOnWire.exchange(0)) / 1024.0 / seconds);
    out += wire;
    out += "; received " + Detail::Describe(g_received, seconds, aServer ? 'c' : 's');
    return out;
}
} // namespace PerfProbe
