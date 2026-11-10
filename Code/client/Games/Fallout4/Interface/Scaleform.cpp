#include <HivemindPCH.h>

#include <Interface/Scaleform.h>

namespace GFx
{
namespace
{
// Scaleform::GFx::Value::ObjectInterface members, CommonLibF4 ids (+1 for this client's address library)
using TObjectAddRef = void(ObjectInterface*, Value*, void*);
using TObjectRelease = void(ObjectInterface*, Value*, void*);
using TGetMember = bool(const ObjectInterface*, void*, const char*, Value*, bool);
using TSetMember = bool(ObjectInterface*, void*, const char*, const Value&, bool);
using TInvoke = bool(ObjectInterface*, void*, Value*, const char*, const Value*, size_t, bool);
using TPushBack = bool(ObjectInterface*, void*, const Value&);
using TGetArraySize = uint32_t(const ObjectInterface*, void*);
using TGetElement = bool(const ObjectInterface*, void*, uint32_t, Value*);
using TRemoveElements = bool(ObjectInterface*, void*, uint32_t, int32_t);

TObjectAddRef* ObjectAddRef()
{
    POINTER_FALLOUT4(TObjectAddRef, s_function, 244787);
    return s_function.Get();
}
TObjectRelease* ObjectRelease()
{
    POINTER_FALLOUT4(TObjectRelease, s_function, 856222);
    return s_function.Get();
}
TGetMember* GetMemberFunction()
{
    POINTER_FALLOUT4(TGetMember, s_function, 1517431);
    return s_function.Get();
}
TSetMember* SetMemberFunction()
{
    POINTER_FALLOUT4(TSetMember, s_function, 1360150);
    return s_function.Get();
}
TInvoke* InvokeFunction()
{
    POINTER_FALLOUT4(TInvoke, s_function, 655848);
    return s_function.Get();
}
TPushBack* PushBackFunction()
{
    POINTER_FALLOUT4(TPushBack, s_function, 1330476);
    return s_function.Get();
}
TGetArraySize* GetArraySizeFunction()
{
    POINTER_FALLOUT4(TGetArraySize, s_function, 254219);
    return s_function.Get();
}
// ObjectInterface::GetElement (CommonLibF4 has none: id 827659, the call in the game's loops over an array's elements
// after GetArraySize, e.g. 23219 and 138835)
TGetElement* GetElementFunction()
{
    POINTER_FALLOUT4(TGetElement, s_function, 827660);
    return s_function.Get();
}
TRemoveElements* RemoveElementsFunction()
{
    POINTER_FALLOUT4(TRemoveElements, s_function, 1286587);
    return s_function.Get();
}
} // namespace

Value::Value(const char* acpString) noexcept
    : type(static_cast<int32_t>(ValueType::kString))
{
    value.string = acpString;
}

Value::Value(double aNumber) noexcept
    : type(static_cast<int32_t>(ValueType::kNumber))
{
    value.number = aNumber;
}

Value::Value(int32_t aInteger) noexcept
    : type(static_cast<int32_t>(ValueType::kInt))
{
    value.integer = aInteger;
}

Value::Value(bool aBoolean) noexcept
    : type(static_cast<int32_t>(ValueType::kBoolean))
{
    value.boolean = aBoolean;
}

Value::Value(const Value& acRhs) noexcept
    : objectInterface(acRhs.objectInterface)
    , type(acRhs.type)
    , value(acRhs.value)
    , dataAux(acRhs.dataAux)
{
    AddRef();
}

Value& Value::operator=(const Value& acRhs) noexcept
{
    if (this == &acRhs)
        return *this;

    Release();
    objectInterface = acRhs.objectInterface;
    type = acRhs.type;
    value = acRhs.value;
    dataAux = acRhs.dataAux;
    AddRef();
    return *this;
}

Value::~Value() noexcept
{
    Release();
}

void Value::AddRef() noexcept
{
    if (IsManaged())
        ObjectAddRef()(objectInterface, this, value.data);
}

void Value::Release() noexcept
{
    if (IsManaged())
        ObjectRelease()(objectInterface, this, value.data);
    objectInterface = nullptr;
    type = static_cast<int32_t>(ValueType::kUndefined);
}

double Value::GetNumber() const noexcept
{
    switch (GetType())
    {
    case ValueType::kNumber: return value.number;
    case ValueType::kInt: return value.integer;
    case ValueType::kUInt: return value.uinteger;
    case ValueType::kBoolean: return value.boolean ? 1.0 : 0.0;
    default: return 0.0;
    }
}

bool Value::GetMember(const char* acpName, Value* apOut) const noexcept
{
    return IsObject() && objectInterface && GetMemberFunction()(objectInterface, value.data, acpName, apOut, IsDisplayObject());
}

bool Value::SetMember(const char* acpName, const Value& acValue) noexcept
{
    return IsObject() && objectInterface && SetMemberFunction()(objectInterface, value.data, acpName, acValue, IsDisplayObject());
}

bool Value::Invoke(const char* acpName, Value* apResult, const Value* apArgs, size_t aCount) noexcept
{
    // Some ActionScript natives read the argument array even with no arguments: never pass null
    static const Value s_noArgs{};
    if (!apArgs)
    {
        apArgs = &s_noArgs;
        aCount = 0;
    }

    return IsObject() && objectInterface && InvokeFunction()(objectInterface, value.data, apResult, acpName, apArgs, aCount, IsDisplayObject());
}

bool Value::PushBack(const Value& acValue) noexcept
{
    return GetType() == ValueType::kArray && objectInterface && PushBackFunction()(objectInterface, value.data, acValue);
}

uint32_t Value::GetArraySize() const noexcept
{
    return GetType() == ValueType::kArray && objectInterface ? GetArraySizeFunction()(objectInterface, value.data) : 0;
}

bool Value::GetElement(uint32_t aIndex, Value* apOut) const noexcept
{
    return apOut && GetType() == ValueType::kArray && objectInterface && GetElementFunction()(objectInterface, value.data, aIndex, apOut);
}

bool Value::RemoveElements(uint32_t aIndex, int32_t aCount) noexcept
{
    return GetType() == ValueType::kArray && objectInterface && RemoveElementsFunction()(objectInterface, value.data, aIndex, aCount);
}

// ASMovieRootBase virtual functions, slots from CommonLibF4's GFx_ASMovieRootBase.h
void* Movie::GetRoot() const noexcept
{
    return *reinterpret_cast<void* const*>(reinterpret_cast<const uint8_t*>(this) + 0x18);
}

void** Movie::GetRootVTable() const noexcept
{
    void* pRoot = GetRoot();
    return pRoot ? *static_cast<void***>(pRoot) : nullptr;
}

bool Movie::GetVariable(Value* apOut, const char* acpPath) const noexcept
{
    using TGetVariable = bool(void*, Value*, const char*);
    void** pVTable = GetRootVTable();
    return pVTable && reinterpret_cast<TGetVariable*>(pVTable[0x32])(GetRoot(), apOut, acpPath);
}

bool Movie::SetVariable(const char* acpPath, const Value& acValue) noexcept
{
    using TSetVariable = bool(void*, const char*, const Value&, int32_t);
    constexpr int32_t kSticky = 1;
    void** pVTable = GetRootVTable();
    return pVTable && reinterpret_cast<TSetVariable*>(pVTable[0x31])(GetRoot(), acpPath, acValue, kSticky);
}

void Movie::CreateObject(Value* apOut, const char* acpClassName, const Value* apArgs, uint32_t aCount) noexcept
{
    using TCreateObject = void(void*, Value*, const char*, const Value*, uint32_t);
    if (void** pVTable = GetRootVTable())
        reinterpret_cast<TCreateObject*>(pVTable[0x2E])(GetRoot(), apOut, acpClassName, apArgs, aCount);
}

void Movie::CreateArray(Value* apOut) noexcept
{
    using TCreateArray = void(void*, Value*);
    if (void** pVTable = GetRootVTable())
        reinterpret_cast<TCreateArray*>(pVTable[0x2F])(GetRoot(), apOut);
}

void Movie::CreateFunction(Value* apOut, FunctionHandler* apHandler, void* apUserData) noexcept
{
    using TCreateFunction = void(void*, Value*, FunctionHandler*, void*);
    if (void** pVTable = GetRootVTable())
        reinterpret_cast<TCreateFunction*>(pVTable[0x30])(GetRoot(), apOut, apHandler, apUserData);
}

void Movie::CreateString(Value* apOut, const char* acpString) noexcept
{
    using TCreateString = void(void*, Value*, const char*);
    if (void** pVTable = GetRootVTable())
        reinterpret_cast<TCreateString*>(pVTable[0x2C])(GetRoot(), apOut, acpString);
}

bool Movie::Invoke(const char* acpPath, Value* apResult, const Value* apArgs, uint32_t aCount) noexcept
{
    using TInvoke = bool(void*, const char*, Value*, const Value*, uint32_t);
    void** pVTable = GetRootVTable();
    return pVTable && reinterpret_cast<TInvoke*>(pVTable[0x39])(GetRoot(), acpPath, apResult, apArgs, aCount);
}

// Movie's own virtual function (slot 0x12, CommonLibF4's GFx_Player.h): the game's MovieImpl one (CommonLibF4 id
// 890828), only called when it's the one in the movie's vtable. The RectF comes back through a hidden pointer.
bool Movie::GetVisibleFrameRect(float (&aOut)[4]) const noexcept
{
    using TGetVisibleFrameRect = float*(const Movie*, float*);
    POINTER_FALLOUT4(TGetVisibleFrameRect, s_getVisibleFrameRect, 890829);
    void* const* pVTable = *reinterpret_cast<void* const* const*>(this);
    if (!pVTable || pVTable[0x12] != reinterpret_cast<void*>(s_getVisibleFrameRect.Get()))
        return false;
    s_getVisibleFrameRect.Get()(this, aOut);
    return aOut[2] > aOut[0] && aOut[3] > aOut[1];
}
} // namespace GFx
