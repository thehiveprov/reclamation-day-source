#include <Games/Fallout4/PapyrusCall.h>

#if TP_FALLOUT4

#include <Misc/BSScript.h>
#include <Misc/GameVM.h>
#include <Misc/BSFixedString.h>

#include <functional>
#include <memory>

namespace
{
// The game's quest form type, for the VM's object handle
constexpr uint32_t kQuestFormType = 80;

struct VmPointer
{
    void* pObject = nullptr;
};

void** VmTable() noexcept
{
    return *reinterpret_cast<void***>(GameVM::Get()->virtualMachine);
}

uint64_t VmHandleOf(uint32_t aFormType, const void* apForm) noexcept
{
    auto* pVm = GameVM::Get()->virtualMachine;
    using TGetPolicy = void*(__fastcall*)(BSScript::IVirtualMachine*);
    void* pPolicy = reinterpret_cast<TGetPolicy>(VmTable()[0x33])(pVm);
    using TGetHandle = uint64_t(__fastcall*)(void*, uint32_t, const void*);
    return reinterpret_cast<TGetHandle>((*reinterpret_cast<void***>(pPolicy))[7])(pPolicy, aFormType, apForm);
}

// The game's std::function (BSTThreadScrapFunction) as Visual C++ 2012 lays it out: 0x18 bytes of room, then the
// callable (ours can't be handed over: the game calls its own layout, NemesisGame.cpp)
class GameArguments
{
public:
    using TFill = std::function<void(BSScript::Statement&)>;

    explicit GameArguments(TFill aFill) noexcept
        : m_pCallable(new (&m_room) Callable(std::make_shared<const TFill>(std::move(aFill))))
    {
    }
    ~GameArguments() { m_pCallable->DeleteThis(false); }

    GameArguments(const GameArguments&) = delete;
    GameArguments& operator=(const GameArguments&) = delete;

private:
    struct __declspec(novtable) CallableBase
    {
        virtual CallableBase* Copy(void* apWhere) const = 0;
        virtual CallableBase* Move(void* apWhere) = 0;
        virtual bool Call(BSScript::Statement& aStatement) = 0;
        virtual const std::type_info& TargetType() const = 0;
        virtual void DeleteThis(bool aFree) = 0;
        virtual ~CallableBase() = default;
        virtual const void* Get() const = 0;
    };

    struct Callable final : CallableBase
    {
        explicit Callable(std::shared_ptr<const TFill> aFill) noexcept
            : m_fill(std::move(aFill))
        {
        }

        CallableBase* Copy(void* apWhere) const override { return new (apWhere ? apWhere : ::operator new(sizeof(Callable))) Callable(m_fill); }
        CallableBase* Move(void* apWhere) override { return new (apWhere ? apWhere : ::operator new(sizeof(Callable))) Callable(std::move(m_fill)); }
        bool Call(BSScript::Statement& aStatement) override
        {
            if (m_fill && *m_fill)
                (*m_fill)(aStatement);
            return true;
        }
        const std::type_info& TargetType() const override { return typeid(TFill); }
        void DeleteThis(bool aFree) override
        {
            this->~Callable();
            if (aFree)
                ::operator delete(this);
        }
        const void* Get() const override { return m_fill.get(); }

        std::shared_ptr<const TFill> m_fill;
    };
    static_assert(sizeof(Callable) <= 0x18);

    alignas(8) uint8_t m_room[0x18];
    CallableBase* m_pCallable;
};
static_assert(sizeof(GameArguments) == 0x20);

GameArguments::TFill Fill(const std::vector<PapyrusCall::Argument>& acArguments)
{
    return [arguments = acArguments](BSScript::Statement& aStatement)
    {
        aStatement.SetSize(static_cast<uint32_t>(arguments.size()));
        for (size_t i = 0; i < arguments.size(); ++i)
            std::visit([&aStatement, i](auto aValue) { aStatement.vars[i].Set(aValue); }, arguments[i]);
    };
}
} // namespace

namespace PapyrusCall
{
bool Static(const char* acpScript, const char* acpFunction, const std::vector<Argument>& acArguments) noexcept
{
    using TDispatch = bool(__fastcall*)(BSScript::IVirtualMachine*, const BSFixedString&, const BSFixedString&, const GameArguments&, const VmPointer&);

    BSFixedString script(acpScript);
    BSFixedString function(acpFunction);
    const GameArguments arguments(Fill(acArguments));
    const VmPointer noCallback;
    const bool sent = reinterpret_cast<TDispatch>(VmTable()[0x2C])(GameVM::Get()->virtualMachine, script, function, arguments, noCallback);
    script.Release();
    function.Release();

    if (!sent)
        spdlog::error("Papyrus: couldn't call {}.{}", acpScript, acpFunction);
    return sent;
}

bool OnQuest(TESForm* apQuest, const char* acpScript, const char* acpFunction, const std::vector<Argument>& acArguments) noexcept
{
    if (!apQuest)
        return false;

    using TDispatch = bool(__fastcall*)(BSScript::IVirtualMachine*, uint64_t, const BSFixedString&, const BSFixedString&, const GameArguments&, const VmPointer&);

    BSFixedString script(acpScript);
    BSFixedString function(acpFunction);
    const GameArguments arguments(Fill(acArguments));
    const VmPointer noCallback;
    const bool sent = reinterpret_cast<TDispatch>(VmTable()[0x2E])(GameVM::Get()->virtualMachine, VmHandleOf(kQuestFormType, apQuest), script, function, arguments, noCallback);
    script.Release();
    function.Release();

    if (!sent)
        spdlog::error("Papyrus: couldn't call {}.{}", acpScript, acpFunction);
    return sent;
}
} // namespace PapyrusCall

#endif
