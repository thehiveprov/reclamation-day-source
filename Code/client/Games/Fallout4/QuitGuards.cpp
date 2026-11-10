// The game's quit ends the UI (id 548587's singleton) and VATS-side time state (id 570121's) in its shutdown (id 234348),
// and only afterwards deletes the player (id 440700, the last reference to id 303410). A slow-time effect still on the
// player then runs SlowTimeEffect's destructor (id 77625), which takes itself off the UI's menu open and close events
// and resets the time state, through those two null singletons. Seen 2026-11-04 20:41: reading 0x18 in the event source's
// lock (id 1425657) on the way out, a crash report and a dump for a quit that had already finished the game. There's no UI
// or time left to tell by then: everything but those two calls is the base class's destructor.
//
// Ids in this file's comment are the address library's; the code's are Reclamation Day's, which are those + 1.

namespace
{
struct SlowTimeEffect;

TP_THIS_FUNCTION(TSlowTimeEffectDestructor, void, SlowTimeEffect);
TP_THIS_FUNCTION(TScriptEffectDestructor, void, SlowTimeEffect);

TSlowTimeEffectDestructor* RealSlowTimeEffectDestructor = nullptr;

void TP_MAKE_THISCALL(HookSlowTimeEffectDestructor, SlowTimeEffect)
{
    POINTER_FALLOUT4(void*, s_ui, 548588);
    POINTER_FALLOUT4(void*, s_time, 570122);
    if (*s_ui.Get() && *s_time.Get())
    {
        TiltedPhoques::ThisCall(RealSlowTimeEffectDestructor, apThis);
        return;
    }

    POINTER_FALLOUT4(TScriptEffectDestructor, s_base, 1009694);
    TiltedPhoques::ThisCall(s_base.Get(), apThis);
}

TiltedPhoques::Initializer s_quitGuards(
    []()
    {
        POINTER_FALLOUT4(TSlowTimeEffectDestructor, s_destructor, 77626);

        RealSlowTimeEffectDestructor = s_destructor.Get();

        TP_HOOK(&RealSlowTimeEffectDestructor, HookSlowTimeEffectDestructor);
    });
} // namespace
