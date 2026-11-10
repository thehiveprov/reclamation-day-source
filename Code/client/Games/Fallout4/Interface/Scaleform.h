#pragma once

// Minimal bridge to the game's Scaleform (Flash) menus, from alandtse/CommonLibF4 (RE/Scaleform/GFx/GFx_Player.h,
// GFx_ASMovieRootBase.h): values, the movie's ActionScript root and the object interface.
namespace GFx
{
enum class ValueType : int32_t
{
    kUndefined,
    kNull,
    kBoolean,
    kInt,
    kUInt,
    kNumber,
    kString,
    kStringW,
    kObject,
    kArray,
    kDisplayObject,
    kClosure,

    kManagedBit = 1 << 6,
    kTypeMask = 0x8F,
};

struct ObjectInterface;

// Scaleform::GFx::Value, 0x20 bytes. Objects, arrays and display objects are reference counted ("managed").
struct Value
{
    Value() = default;
    Value(const char* acpString) noexcept;
    Value(double aNumber) noexcept;
    // An ActionScript int (natives that check the type tell it from a Number)
    Value(int32_t aInteger) noexcept;
    Value(bool aBoolean) noexcept;
    Value(const Value& acRhs) noexcept;
    Value& operator=(const Value& acRhs) noexcept;
    ~Value() noexcept;

    [[nodiscard]] ValueType GetType() const noexcept { return static_cast<ValueType>(static_cast<int32_t>(type) & static_cast<int32_t>(ValueType::kTypeMask) & ~static_cast<int32_t>(ValueType::kManagedBit)); }
    [[nodiscard]] bool IsObject() const noexcept { const auto t = GetType(); return t == ValueType::kObject || t == ValueType::kArray || t == ValueType::kDisplayObject; }
    [[nodiscard]] bool IsDisplayObject() const noexcept { return GetType() == ValueType::kDisplayObject; }
    // Managed strings (the movie's own) point to a string node whose first member is the text
    [[nodiscard]] const char* GetString() const noexcept
    {
        if (GetType() != ValueType::kString)
            return nullptr;
        return IsManaged() ? *value.managedString : value.string;
    }
    [[nodiscard]] double GetNumber() const noexcept;

    bool GetMember(const char* acpName, Value* apOut) const noexcept;
    bool SetMember(const char* acpName, const Value& acValue) noexcept;
    bool Invoke(const char* acpName, Value* apResult = nullptr, const Value* apArgs = nullptr, size_t aCount = 0) noexcept;
    bool PushBack(const Value& acValue) noexcept;
    uint32_t GetArraySize() const noexcept;
    // An array's element (ObjectInterface::GetElement)
    bool GetElement(uint32_t aIndex, Value* apOut) const noexcept;
    bool RemoveElements(uint32_t aIndex, int32_t aCount) noexcept;
    // Drops the reference without telling the game, for values of a movie that is gone (releasing would touch freed
    // memory); the object's count is leaked, which is harmless once the movie is destroyed
    void Forget() noexcept { objectInterface = nullptr; type = 0; value.data = nullptr; }

    ObjectInterface* objectInterface{nullptr}; // 00
    int32_t type{0};                           // 08
    union
    {
        double number;
        int32_t integer;
        uint32_t uinteger;
        bool boolean;
        const char* string;
        const char* const* managedString;
        void* data;
    } value{};                                 // 10
    size_t dataAux{0};                         // 18

private:
    void AddRef() noexcept;
    void Release() noexcept;
    [[nodiscard]] bool IsManaged() const noexcept { return (type & static_cast<int32_t>(ValueType::kManagedBit)) && objectInterface; }
};
static_assert(sizeof(Value) == 0x20);

struct Movie;

// Scaleform::GFx::FunctionHandler: a native function ActionScript can call (e.g. an event listener). Instances must
// outlive the movie: the game reference counts them and would free them with its own allocator at 0, so the count
// starts high enough to never get there.
struct FunctionHandler
{
    struct Params
    {
        Value* retVal;          // 00
        Movie* movie;           // 08
        Value* self;            // 10
        Value* argsWithThisRef; // 18
        Value* args;            // 20
        uint32_t argCount;      // 28
        void* userData;         // 30
    };
    static_assert(sizeof(Params) == 0x38);

    virtual ~FunctionHandler() = default;      // 00
    virtual void Call(const Params& acParams) = 0; // 01

    volatile int32_t refCount{1 << 30}; // 08
};

// Scaleform::GFx::Movie: mostly the ActionScript root (Ptr<ASMovieRootBase> at 0x18)
struct Movie
{
    void CreateFunction(Value* apOut, FunctionHandler* apHandler, void* apUserData = nullptr) noexcept;
    void CreateArray(Value* apOut) noexcept;
    bool GetVariable(Value* apOut, const char* acpPath) const noexcept;
    bool SetVariable(const char* acpPath, const Value& acValue) noexcept;
    // A plain Object without a class name
    void CreateObject(Value* apOut, const char* acpClassName = nullptr, const Value* apArgs = nullptr, uint32_t aCount = 0) noexcept;
    void CreateString(Value* apOut, const char* acpString) noexcept;
    bool Invoke(const char* acpPath, Value* apResult, const Value* apArgs, uint32_t aCount) noexcept;
    // The part of the stage that's on the screen, in stage units (left, top, right, bottom): more than the stage on a
    // screen of another shape, as the movie's scale mode fits it
    bool GetVisibleFrameRect(float (&aOut)[4]) const noexcept;

private:
    [[nodiscard]] void** GetRootVTable() const noexcept;
    [[nodiscard]] void* GetRoot() const noexcept;
};
} // namespace GFx
