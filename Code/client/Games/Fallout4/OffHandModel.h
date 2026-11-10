#pragma once

#include <Structs/Inventory.h>
#include <NodeTools.h>

struct TESForm;
struct TESObjectREFR;

// An object mod as the game has it on an item stack (BGSObjectInstanceExtra's index data): its form, the mod it attaches
// to, its rank
struct OffHandMod
{
    uint32_t FormId{0};
    uint8_t AttachIndex{0};
    uint8_t Rank{0};
};

// The look of a weapon in a left hand (docs/DUAL_WIELD.md): the game builds a weapon's 3D with its mods' parts for a
// reference of it, so a short-lived reference of the weapon (the stack's mods put on it) is placed out of sight, its 3D
// copied once it's loaded, and the reference deleted. Copies of that hang from skeletons' WeaponLeft nodes. Main thread.
class OffHandModel
{
public:
    OffHandModel() = default;
    ~OffHandModel() noexcept;

    TP_NOCOPYMOVE(OffHandModel);

    // Starts building this weapon's look (with these mods), near apNear; the previous look goes
    void Build(TESForm* apWeapon, const Vector<OffHandMod>& acMods, TESObjectREFR* apNear) noexcept;
    // Every frame: finishes a build once the reference's 3D is there (or gives up after a while)
    void Update() noexcept;
    [[nodiscard]] bool IsReady() const noexcept { return m_pSource != nullptr; }
    [[nodiscard]] uint32_t GetWeaponId() const noexcept { return m_weaponId; }

    // A copy hung from the WeaponLeft node under apRoot (a skeleton's root), kept until detached; nothing when it isn't
    // ready or the skeleton has no such node. Again for a root it's on: nothing (a new root, a skeleton rebuilt: moved)
    void AttachTo(uint8_t* apRoot) noexcept;
    // A copy's local under WeaponLeft: half a turn about the barrel (local y). In both skeletons WeaponLeft is the mirror
    // of the right hand's WEAPON with its z (the weapon's top) turned over (Fallout4 - Meshes.ba2, read 2026-10-24), so a
    // weapon hung there as it is sits upside down in a left hand gripping it as the right hand grips; turned, it's upright
    [[nodiscard]] static NodeTools::Transform OnWeaponLeft() noexcept;
    // A copy's root is named this, and a node in it the skeleton has too (a weapon's model can have its root called
    // WEAPON, as the baton's and the 10mm's are: names are matched without case, so the right hand's own weapon bone
    // and lookups of it could take the copy for it) is renamed with this in front
    static constexpr char kModelName[] = "OffHandModel";
    void DetachFrom(uint8_t* apRoot) noexcept;
    void DetachAll() noexcept;
    // The copy on that skeleton, if there is one (its ProjectileNode is the muzzle)
    [[nodiscard]] uint8_t* GetAttached(uint8_t* apRoot) const noexcept;
    // Shown or hidden on every skeleton it's on (a weapon put away)
    void SetShown(bool aShown) noexcept;
    // Everything let go: the copies, the source, a build under way
    void Reset() noexcept;

    // The muzzle flash on every copy's ProjectileNode for aSeconds: acModel (a projectile's muzzle flash model, under
    // Meshes) loaded the first time (BSModelDB::Demand), a copy of it hung there once per copy and shown again for
    // each shot; Update puts it away again
    void Flash(const std::string& acModel, float aSeconds) noexcept;

private:
    struct Attached
    {
        uint8_t* pRoot{nullptr};
        uint8_t* pHand{nullptr};
        uint8_t* pCopy{nullptr};
        // the muzzle flash under the copy's muzzle (the copy's child: goes with it)
        uint8_t* pFlash{nullptr};
    };

    void DropBuildReference() noexcept;
    void DropFlashSource() noexcept;

    uint32_t m_weaponId{0};
    // the reference being built, and since when
    uint32_t m_buildRefId{0};
    float m_buildTime{0.f};
    // the copied 3D the copies are made from (our reference)
    uint8_t* m_pSource{nullptr};
    Vector<Attached> m_attached;
    bool m_shown{true};
    // the muzzle flash: the loaded model the flashes are copied from (our reference), its path, until when it shows
    uint8_t* m_pFlashSource{nullptr};
    std::string m_flashModel;
    int64_t m_flashUntil{0};
};
