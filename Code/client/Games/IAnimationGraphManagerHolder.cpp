#include <HivemindPCH.h>

#include <Games/Animation/IAnimationGraphManagerHolder.h>

#include <BSAnimationGraphManager.h>

bool IAnimationGraphManagerHolder::SetVariableFloat(BSFixedString* apVariable, float aValue)
{
    TP_THIS_FUNCTION(TSetFloatVariable, bool, IAnimationGraphManagerHolder, BSFixedString*, float);

    POINTER_SKYRIMSE(TSetFloatVariable, InternalSetFloatVariable, 32887);
    POINTER_FALLOUT4(TSetFloatVariable, InternalSetFloatVariable, 27401);

    return TiltedPhoques::ThisCall(InternalSetFloatVariable, this, apVariable, aValue);
}

#if TP_FALLOUT4
// The bool one of the same family of setters (true id 1437197, next to the float one's 27400; it keeps the value as a
// byte)
bool IAnimationGraphManagerHolder::SetVariableBool(BSFixedString* apVariable, bool aValue)
{
    TP_THIS_FUNCTION(TSetBoolVariable, bool, IAnimationGraphManagerHolder, BSFixedString*, bool);

    POINTER_FALLOUT4(TSetBoolVariable, InternalSetBoolVariable, 1437198);

    return TiltedPhoques::ThisCall(InternalSetBoolVariable, this, apVariable, aValue);
}
#endif

#if TP_SKYRIM64
bool IAnimationGraphManagerHolder::RevertAnimationGraphManager()
{
    TP_THIS_FUNCTION(TRevertAnimationGraphManager, bool, IAnimationGraphManagerHolder);
    POINTER_SKYRIMSE(TRevertAnimationGraphManager, InternalRevertAnimationGraphManager, 32883);

    return TiltedPhoques::ThisCall(InternalRevertAnimationGraphManager, this);
}
#endif

bool IAnimationGraphManagerHolder::IsReady()
{
    BSAnimationGraphManager* pAnimationGraph = nullptr;
    const auto result = GetBSAnimationGraph(&pAnimationGraph);

    if (pAnimationGraph)
        pAnimationGraph->Release();

    return result;
}
