// The real table type (tsl::hopscotch_map, as TiltedPhoques::Map) with the page's real listeners, in their real order.
// Runs the old handler's shape: walk the table, and from inside the "enterGame" entry add what the page adds there.
// Freed memory is kept so the old entry can be read afterwards, the way the old code read it.
#include <tsl/hopscotch_map.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

template <class T> struct KeepFreed
{
    using value_type = T;
    KeepFreed() = default;
    template <class U> KeepFreed(const KeepFreed<U>&) {}
    T* allocate(std::size_t n) { return static_cast<T*>(std::malloc(n * sizeof(T))); }
    void deallocate(T*, std::size_t) {}
    template <class U> bool operator==(const KeepFreed<U>&) const { return true; }
    template <class U> bool operator!=(const KeepFreed<U>&) const { return false; }
};

// Moves and clears as CefRefPtr does
struct Ref
{
    void* p = nullptr;
    explicit Ref(void* a) : p(a) {}
    Ref(const Ref& o) : p(o.p) {}
    Ref(Ref&& o) noexcept : p(o.p) { o.p = nullptr; }
    Ref& operator=(const Ref& o) { p = o.p; return *this; }
    Ref& operator=(Ref&& o) noexcept { p = o.p; o.p = nullptr; return *this; }
    ~Ref() { p = nullptr; }
};

using Value = std::pair<Ref, Ref>;
using Table = tsl::hopscotch_map<std::string, Value, std::hash<std::string>, std::equal_to<std::string>, KeepFreed<std::pair<std::string, Value>>>;

static void Run(const char* acpName, const std::vector<std::string>& acAtStart, const std::vector<std::string>& acInside)
{
    Table table;
    int context = 1, function = 2;
    for (const auto& name : acAtStart)
        table.emplace(name, Value(Ref(&context), Ref(&function)));

    const auto buckets = table.bucket_count();
    printf("\n%s\n  at start: %zu listeners, %zu buckets, grows when it holds %zu\n", acpName, table.size(), buckets, static_cast<size_t>(buckets * table.max_load_factor()));

    for (const auto& callback : table)
    {
        if (callback.first != "enterGame")
            continue;

        const auto* pOld = &callback;
        printf("  enterGame runs, its entry at %p, its context %s\n", static_cast<const void*>(pOld), callback.second.first.p ? "set" : "EMPTY");

        // The page's code, inside the event
        for (const auto& name : acInside)
        {
            const auto before = table.bucket_count();
            table.emplace(name, Value(Ref(&context), Ref(&function)));
            printf("    on('%s'): now %zu listeners, %zu buckets%s\n", name.c_str(), table.size(), table.bucket_count(), table.bucket_count() != before ? "   <- THE TABLE GREW" : "");
        }

        const auto* pNow = &*table.find("enterGame");
        printf("  enterGame's entry is now at %p (%s)\n", static_cast<const void*>(pNow), pNow == pOld ? "same place" : "MOVED");
        // What the old code does next: callback.second.first->Exit()
        printf("  context the old code would call Exit() on: %s\n", callback.second.first.p ? "set, fine" : "NULL, the crash");
        return;
    }
}

int main(int, char** argv)
{
    std::vector<std::string> atStart, inside;
    std::string line;
    std::ifstream start(argv[1]);
    while (std::getline(start, line))
        if (!line.empty())
            atStart.push_back(line);

    Run("Released V.1", atStart, {"hud", "crosshair", "companionName", "campCard"});
    Run("Test build with the gauges", atStart, {"hud", "crosshair", "bike", "companionName", "campCard"});
    return 0;
}
