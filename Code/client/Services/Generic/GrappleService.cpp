#include <Services/GrappleService.h>

#if TP_FALLOUT4

#include <Services/ParkourService.h>
#include <Services/TransportService.h>
#include <Events/UpdateEvent.h>
#include <Games/Fallout4/BikeLink.h>
#include <Messages/RequestMovementAction.h>
#include <Messages/NotifyMovementAction.h>
#include <World.h>
#include <Utils.h>
#include <Components.h>

#include <PlayerCharacter.h>
#include <Forms/ActorValueInfo.h>
#include <Forms/TESAmmo.h>
#include <Forms/TESObjectCELL.h>
#include <Games/ActorExtension.h>
#include <Games/TES.h>
#include <Interface/HudMarker.h>
#include <Misc/BSFixedString.h>
#include <Projectiles/Projectile.h>
#include <WorldRays.h>
#include <PerfProbe.h>

namespace
{
// RD_GrapplingGun.esp (Tools/BuildGrapplePlugin.js): the gun, and what's placed in the world for a shot: the spike as it
// sits on the gun (GrappleSpikeVisual.nif, no collision) and the cable (GrappleCable.nif, a unit tube along +Y)
constexpr char kPlugin[] = "RD_GrapplingGun.esp";
constexpr uint32_t kGunId = 0x800;
constexpr uint32_t kSpikeVisualId = 0x809;
constexpr uint32_t kCableVisualId = 0x80A;
// The spikes' projectiles, how far they reach and how fast they fly (game units, units/s; the builder's PROJ records)
struct SpikeKind
{
    uint32_t Projectile;
    float Range;
    float Speed;
};
constexpr SpikeKind kSpikes[] = {{0x803, 3150.f, 5000.f}, {0x804, 4200.f, 7000.f}};

// The visuals' shapes (the NIFs' own names) and the gun's nodes (GrapplingGun.nif): its spike, hidden while the spike
// is out, and the muzzle the cable comes out of. With another weapon out on the cable, the cable is in the left hand.
constexpr char kSpikeShape[] = "GrappleSpikeVisual:0";
constexpr char kCableShape[] = "GrappleCable:0";
constexpr char kGunSpikeNode[] = "GrappleSpike";
constexpr char kMuzzleNode[] = "ProjectileNode";
constexpr char kLeftHandNode[] = "LArm_Hand";
// The spike's tip is this far ahead of its origin (the shaft's rear) and bites this deep; its bound, around its middle
constexpr float kSpikeLength = 8.7f;
constexpr float kBiteDepth = 3.f;
constexpr float kSpikeBoundRadius = 6.5f;

// The winch: the pull starts at this speed and gathers speed up to the top one (units/s, units/s²); the spike comes back
// at this speed
constexpr float kPullStartSpeed = 450.f;
constexpr float kPullAcceleration = 2600.f;
constexpr float kPullTopSpeed = 1500.f;
constexpr float kRetractSpeed = 4000.f;
// The pull ends with the hands this far short of the spike when there's no ledge, the cable fixed this far out from a
// wall (the body hangs clear of it)
constexpr float kHangGap = 40.f;
constexpr float kStandOff = 30.f;
// A ledge to pull up onto: looked for from spots this far back from where the spike bit and this far under it (the
// parkour's grab finds a top 10 to 125 over the feet and at most 70 ahead of them)
constexpr float kLedgeLookBack = 30.f;
constexpr float kLedgeLookUnder[] = {60.f, 110.f, 10.f};

// Hanging: the hands this far over the feet; gravity (9.8 m/s², 70 units a metre), the swing and the climb, a little air
// resistance; the cable is never shorter than this
constexpr float kHandsOverFeet = 150.f;
constexpr float kGravity = 686.f;
constexpr float kSwingAcceleration = 450.f;
constexpr float kClimbSpeed = 160.f;
constexpr float kDrag = 0.35f;
constexpr float kMinCable = 40.f;
// a share of the pull's speed carried into the swing
constexpr float kPullIntoSwing = 0.3f;
// The body for the rays that keep the held player out of walls and find the ground: its middle over the feet, its radius
constexpr float kBodyMiddle = 64.f;
constexpr float kBodyRadius = 20.f;
constexpr float kMinGroundNormalZ = 0.6f;
// The player this far from where the winch put it was moved away (a load, a teleport): the line ends
constexpr float kMaxOffLine = 300.f;
// The muzzle is at most this far from the player's chest: further, the gun's nodes weren't placed this frame (a
// skeleton not updated has them at the world's origin), and the cable runs from the chest instead
constexpr float kMuzzleReach = 250.f;

// Reeling someone in: this fast, until this close or this long; someone taller than this breaks free at once. Reeled by
// another player's game, the line waits this much longer for them to come
constexpr float kReelSpeed = 650.f;
constexpr float kReelStop = 120.f;
constexpr float kReelMaxTime = 3.f;
constexpr float kReelMaxHeight = 280.f;
constexpr float kReelElsewhereSlack = 0.5f;
// who the spike hit, biting a character's collision: someone this close to the bite, beyond their own width
constexpr float kHookReach = 40.f;
// The spike's hit: the gun's projectile launched at who it bit from this far short of their middle (this high up them)
constexpr float kHitStandBack = 60.f;
constexpr float kHitHeight = 0.6f;
// Another player's line: forgotten when nothing more came for this long (s)
constexpr float kRemoteLineTimeout = 20.f;
// TESAmmo's projectile (AMMO_DATA at 0x160)
constexpr uintptr_t kAmmoProjectileOffset = 0x160;
// PlayerCamera: the world's field of view at 0x168, the first person weapon's at 0x16C (degrees)
constexpr uintptr_t kWorldFovOffset = 0x168;
constexpr uintptr_t kFirstPersonFovOffset = 0x16C;

// The HUD marker looks for a ledge this often (s)
constexpr float kMarkerInterval = 0.1f;

// Collision layers, as ParkourService casts its rays: the line of sight's goes through what can be seen through but not
// walked through (chain-link fences, grates, glass), which the camera's hits
constexpr uint32_t kLayerCamera = 39;
constexpr uint32_t kTransparentLayers[] = {3, 26, 28};

// PlayerCharacter::firstPerson3D (CommonLibF4, 1.10.163). NiAVObject: parent at 0x28, the local and world transforms at
// 0x30 and 0x70 (each a rotation of 3 rows of 4 floats as NiTransform stores it, row i the direction of local axis i in
// the parent's space; the translation at +0x30, the scale at +0x3C), the world bound at 0xB0 (centre, radius), the flags
// at 0x108 (bit 0: culled by the app). NiNode's children: the array at 0x128, the slots in use at 0x132. IsNode is
// vtable slot 4, GetObjectByName slot 0x2E.
constexpr uintptr_t kFirstPerson3DOffset = 0xB78;
constexpr uintptr_t kParentOffset = 0x28;
constexpr uintptr_t kLocalOffset = 0x30;
constexpr uintptr_t kWorldOffset = 0x70;
constexpr uintptr_t kWorldBoundOffset = 0xB0;
constexpr uintptr_t kFlagsOffset = 0x108;
constexpr uintptr_t kChildrenOffset = 0x128;
constexpr uintptr_t kChildCountOffset = 0x132;

GrappleService* s_pGrappleService = nullptr;
// the spike's hit on someone being launched (this thread): the launch hook lets it through
thread_local bool s_launchingHit = false;

uint32_t PluginForm(uint32_t aLocalId) noexcept
{
    const Mod* pMod = ModManager::Get()->GetByName(kPlugin);
    return pMod && pMod->IsLoaded() ? pMod->GetFormId(aLocalId) : 0;
}

// The gun and the spikes' projectiles as loaded, kept once looked up (first from a game under way, whose load order
// doesn't change): the launch hook asks for every projectile, the update every frame
uint32_t GunId() noexcept
{
    static const uint32_t s_id = PluginForm(kGunId);
    return s_id;
}

uint32_t SpikeProjectileId(size_t aKind) noexcept
{
    static const std::array<uint32_t, std::size(kSpikes)> s_ids = []
    {
        std::array<uint32_t, std::size(kSpikes)> ids{};
        for (size_t i = 0; i < ids.size(); ++i)
            ids[i] = PluginForm(kSpikes[i].Projectile);
        return ids;
    }();
    return s_ids[aKind];
}

// Which spike a projectile is (std::size(kSpikes): none of them)
size_t SpikeKindOf(const TESForm* apProjectile) noexcept
{
    for (size_t i = 0; apProjectile && i < std::size(kSpikes); ++i)
    {
        if (apProjectile->formID == SpikeProjectileId(i))
            return i;
    }
    return std::size(kSpikes);
}

// The reach of the spikes a gun loaded with apAmmo fires (the standard spike's without them)
float ReachOf(const TESForm* apAmmo) noexcept
{
    const auto* pProjectile = apAmmo ? *reinterpret_cast<const TESForm* const*>(reinterpret_cast<const uint8_t*>(apAmmo) + kAmmoProjectileOffset) : nullptr;
    const size_t kind = SpikeKindOf(pProjectile);
    return kSpikes[kind < std::size(kSpikes) ? kind : 0].Range;
}

// The closest thing on the line the spike couldn't go through: the line of sight's hit, or a see-through obstacle's (a
// chain-link fence) nearer than it
WorldRays::RayHit CastRay(TESObjectCELL* apCell, uint32_t aFilter, const NiPoint3& acFrom, const NiPoint3& acTo) noexcept
{
    const WorldRays::RayHit sight = WorldRays::PickOnce(apCell, aFilter, acFrom, acTo);
    const WorldRays::RayHit camera = WorldRays::PickOnce(apCell, (aFilter & ~0x7Fu) | kLayerCamera, acFrom, acTo);
    if (!camera.Hit || std::find(std::begin(kTransparentLayers), std::end(kTransparentLayers), camera.Layer) == std::end(kTransparentLayers))
        return sight;
    if (sight.Hit && glm::distance(glm::vec3(sight.Point), glm::vec3(acFrom)) <= glm::distance(glm::vec3(camera.Point), glm::vec3(acFrom)))
        return sight;
    return camera;
}

// PlayerCamera (CommonLibF4 id 1171980): TESCamera::currentState at 0x28, TESCameraState::id at 0x20, 0 = first person
bool IsFirstPerson() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerCamera, 1171981);
    const uint8_t* pCamera = *s_playerCamera.Get();
    if (!pCamera)
        return false;

    const uint8_t* pState = *reinterpret_cast<const uint8_t* const*>(pCamera + 0x28);
    return pState && *reinterpret_cast<const uint32_t*>(pState + 0x20) == 0;
}

// The movement input, x right and y forward from -1 to 1 (PlayerControls, CommonLibF4 id 544871: its data at 0x44 starts
// with it)
glm::vec2 GetMoveInput() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerControls, 544872);
    const uint8_t* pControls = *s_playerControls.Get();
    if (!pControls)
        return {};
    const float x = *reinterpret_cast<const float*>(pControls + 0x44);
    const float y = *reinterpret_cast<const float*>(pControls + 0x48);
    if (!std::isfinite(x) || !std::isfinite(y))
        return {};
    return {std::clamp(x, -1.f, 1.f), std::clamp(y, -1.f, 1.f)};
}

// The way the view looks and its right (world, unit), from the camera's world to clip matrix (each row's direction is one
// of the camera's axes); the player's own look without one
void GetView(PlayerCharacter* apPlayer, glm::vec3& aForward, glm::vec3& aRight) noexcept
{
    float m[4][4];
    if (HudMarker::GetWorldToClip(m))
    {
        const glm::vec3 forward(m[3][0], m[3][1], m[3][2]);
        const glm::vec3 right(m[0][0], m[0][1], m[0][2]);
        if (glm::length(forward) > 1e-6f && glm::length(right) > 1e-6f)
        {
            aForward = glm::normalize(forward);
            aRight = glm::normalize(right);
            return;
        }
    }
    const float pitch = apPlayer->rotation.x;
    const float yaw = apPlayer->rotation.z;
    aForward = glm::vec3(std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), -std::sin(pitch));
    aRight = glm::vec3(std::cos(yaw), -std::sin(yaw), 0.f);
}

uint8_t* GetObjectByName(uint8_t* apNode, const BSFixedString& acName) noexcept
{
    if (!apNode)
        return nullptr;
    using TGetObjectByName = uint8_t* (*)(uint8_t*, const BSFixedString&);
    return (*reinterpret_cast<TGetObjectByName* const*>(apNode))[0x2E](apNode, acName);
}

// A node's first child, when it's a node with one
uint8_t* GetFirstChild(uint8_t* apObject) noexcept
{
    using TIsNode = uint8_t* (*)(uint8_t*);
    uint8_t* pNode = (*reinterpret_cast<TIsNode* const*>(apObject))[4](apObject);
    if (!pNode)
        return nullptr;
    auto* const* pChildren = *reinterpret_cast<uint8_t* const* const*>(pNode + kChildrenOffset);
    const uint16_t count = *reinterpret_cast<const uint16_t*>(pNode + kChildCountOffset);
    for (uint16_t i = 0; pChildren && i < count; ++i)
    {
        if (pChildren[i])
            return pChildren[i];
    }
    return nullptr;
}

glm::vec3 GetWorldPosition(const uint8_t* apNode) noexcept
{
    const auto* pFloats = reinterpret_cast<const float*>(apNode + kWorldOffset);
    return {pFloats[12], pFloats[13], pFloats[14]};
}

// Where a node's local axis points in the world (its world rotation's row, as NiTransform keeps it), unit
glm::vec3 GetWorldAxis(const uint8_t* apNode, int aAxis) noexcept
{
    const auto* pFloats = reinterpret_cast<const float*>(apNode + kWorldOffset) + aAxis * 4;
    const glm::vec3 axis(pFloats[0], pFloats[1], pFloats[2]);
    return glm::length(axis) > 1e-6f ? glm::normalize(axis) : axis;
}

uint8_t* GetFirstPersonRoot(PlayerCharacter* apPlayer) noexcept
{
    return *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(apPlayer) + kFirstPerson3DOffset);
}

uint8_t* GetThirdPersonRoot(PlayerCharacter* apPlayer) noexcept
{
    return reinterpret_cast<uint8_t*>(apPlayer->GetNiNode());
}

// The gun's own spike shown or hidden on the skeleton under apRoot (the spike leaves the gun while it's out)
void SetGunSpikeShownOn(uint8_t* apRoot, bool aShown) noexcept
{
    static BSFixedString s_spike(kGunSpikeNode);
    if (uint8_t* pNode = GetObjectByName(apRoot, s_spike))
    {
        auto& flags = *reinterpret_cast<uint64_t*>(pNode + kFlagsOffset);
        flags = aShown ? flags & ~1ull : flags | 1ull;
    }
}

// in both views
void SetGunSpikeShown(PlayerCharacter* apPlayer, bool aShown) noexcept
{
    SetGunSpikeShownOn(GetFirstPersonRoot(apPlayer), aShown);
    SetGunSpikeShownOn(GetThirdPersonRoot(apPlayer), aShown);
}

// Where a point of the first person weapon is seen in the world: the first person weapon is drawn with its own field of
// view (PlayerCamera's firstPersonFOV, the world's worldFOV), so its point at a depth in the view shows where the world's
// point is with its sideways and upward offsets scaled by the ratio of the two views' half angle tangents
glm::vec3 FirstPersonToWorld(PlayerCharacter* apPlayer, const glm::vec3& acPoint) noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerCamera, 1171981);
    const uint8_t* pCamera = *s_playerCamera.Get();
    NiPoint3 eyePoint;
    if (!pCamera || !HudMarker::GetCameraPosition(eyePoint))
        return acPoint;
    const float worldFov = *reinterpret_cast<const float*>(pCamera + kWorldFovOffset);
    const float weaponFov = *reinterpret_cast<const float*>(pCamera + kFirstPersonFovOffset);
    if (!(worldFov > 1.f && worldFov < 179.f && weaponFov > 1.f && weaponFov < 179.f))
        return acPoint;
    glm::vec3 forward;
    glm::vec3 right;
    GetView(apPlayer, forward, right);
    const glm::vec3 up = glm::normalize(glm::cross(right, forward));
    const glm::vec3 eye = eyePoint;
    const glm::vec3 offset = acPoint - eye;
    const float scale = std::tan(glm::radians(worldFov) * 0.5f) / std::tan(glm::radians(weaponFov) * 0.5f);
    return eye + forward * glm::dot(offset, forward) + right * (glm::dot(offset, right) * scale) + up * (glm::dot(offset, up) * scale);
}

// Where the cable comes out of the gun on the skeleton under apRoot (its muzzle, looked for on the gun: its spike's
// parent is the gun's root, so not another ProjectileNode), else its left hand; null without either
std::optional<glm::vec3> GetMuzzleOn(uint8_t* apRoot) noexcept
{
    static BSFixedString s_spike(kGunSpikeNode);
    static BSFixedString s_muzzle(kMuzzleNode);
    static BSFixedString s_hand(kLeftHandNode);
    if (uint8_t* pSpike = GetObjectByName(apRoot, s_spike))
    {
        if (uint8_t* pMuzzle = GetObjectByName(*reinterpret_cast<uint8_t**>(pSpike + kParentOffset), s_muzzle))
            return GetWorldPosition(pMuzzle);
    }
    if (uint8_t* pHand = GetObjectByName(apRoot, s_hand))
        return GetWorldPosition(pHand);
    return std::nullopt;
}

// Where the cable comes out: the gun's muzzle in the view the player is in (in first person as the world sees it), the
// left hand with another weapon out, the chest without either (or with one that isn't by the player)
glm::vec3 GetMuzzle(PlayerCharacter* apPlayer) noexcept
{
    const glm::vec3 chest = glm::vec3(apPlayer->position) + glm::vec3(0.f, 0.f, 100.f);
    const bool firstPerson = IsFirstPerson();
    uint8_t* pRoot = firstPerson ? GetFirstPersonRoot(apPlayer) : GetThirdPersonRoot(apPlayer);
    if (const auto muzzle = GetMuzzleOn(pRoot))
    {
        glm::vec3 point = *muzzle;
        if (firstPerson)
        {
            // The first person skeleton isn't in the world's space (seen in game: the muzzle at (33, 15, 147), a long way
            // from the player): its Camera bone is the view (x right, y up, z back, as DualWieldPose has it), so the
            // muzzle's place in that bone's frame is its place in the world's view
            static BSFixedString s_cameraBone("Camera");
            NiPoint3 eye;
            if (uint8_t* pCameraBone = GetObjectByName(pRoot, s_cameraBone); pCameraBone && HudMarker::GetCameraPosition(eye))
            {
                const glm::vec3 offset = *muzzle - GetWorldPosition(pCameraBone);
                glm::vec3 forward;
                glm::vec3 right;
                GetView(apPlayer, forward, right);
                const glm::vec3 up = glm::normalize(glm::cross(right, forward));
                point = glm::vec3(eye) + right * glm::dot(offset, GetWorldAxis(pCameraBone, 0)) + up * glm::dot(offset, GetWorldAxis(pCameraBone, 1)) -
                        forward * glm::dot(offset, GetWorldAxis(pCameraBone, 2));
            }
            point = FirstPersonToWorld(apPlayer, point);
        }
        if (glm::distance(point, chest) <= kMuzzleReach)
            return point;
        static int64_t s_loggedAt = 0;
        const int64_t now = GetTickCount64();
        if (now - s_loggedAt > 5000)
        {
            s_loggedAt = now;
            spdlog::info("Grapple: the muzzle reads ({:.0f}, {:.0f}, {:.0f}) in {} person, {:.0f} from the player: the cable runs from the chest", point.x, point.y,
                         point.z, firstPerson ? "first" : "third", glm::distance(point, chest));
        }
    }
    return chest;
}

// One of a shot's visuals placed in the cell of apNear (a reference): its reference's form id (0 when it couldn't be)
uint32_t PlaceVisual(uint32_t aLocalId, TESObjectREFR* apNear, const NiPoint3& acAt) noexcept
{
    TESForm* pBase = TESForm::GetById(PluginForm(aLocalId));
    if (!pBase || !apNear)
        return 0;
    TESWorldSpace* pWorldSpace = apNear->GetWorldSpace();
    const uint32_t handle = ModManager::Get()->SpawnObject(pBase, acAt, NiPoint3(), pWorldSpace ? nullptr : apNear->parentCell, pWorldSpace);
    const TESObjectREFR* pRef = handle ? TESObjectREFR::GetByHandle(handle) : nullptr;
    return pRef ? pRef->formID : 0;
}

void RemoveVisual(uint32_t& aId) noexcept
{
    if (auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(aId)))
    {
        pRef->Disable();
        pRef->Delete();
    }
    aId = 0;
}

// A visual put at acAt, unturned, with its shape turned to point along acAhead (unit). Stretched (the cable): the shape's
// scale is aLength and its x and z rows are shrunk back by it, so it's aLength long along acAhead, as thick as it was,
// and its bound grows with it (the tube's normals are across its length: the stretch leaves its lighting alone). The
// shape's world transform and the bounds are written for this frame too, as the game's update would work them out.
void MoveVisual(uint32_t aId, const BSFixedString& acShape, const glm::vec3& acAt, const glm::vec3& acAhead, float aLength, bool aStretch) noexcept
{
    auto* pRef = Cast<TESObjectREFR>(TESForm::GetById(aId));
    if (!pRef)
        return;

    const glm::vec3 ahead = glm::length(acAhead) > 1e-6f ? glm::normalize(acAhead) : glm::vec3(0.f, 1.f, 0.f);
    glm::vec3 right = glm::cross(ahead, glm::vec3(0.f, 0.f, 1.f));
    right = glm::length(right) > 1e-3f ? glm::normalize(right) : glm::vec3(1.f, 0.f, 0.f);
    const glm::vec3 up = glm::cross(right, ahead);
    const float scale = aStretch ? std::max(aLength, 0.01f) : 1.f;
    const float across = 1.f / scale;
    const glm::vec3 rows[3] = {right * across, ahead, up * across};

    uint8_t* pRoot = reinterpret_cast<uint8_t*>(pRef->GetNiNode());
    uint8_t* pShape = GetObjectByName(pRoot, acShape);
    if (!pShape && pRoot)
        pShape = GetFirstChild(pRoot);
    if (pShape)
    {
        auto* pLocal = reinterpret_cast<float*>(pShape + kLocalOffset);
        for (int row = 0; row < 3; ++row)
        {
            pLocal[row * 4 + 0] = rows[row].x;
            pLocal[row * 4 + 1] = rows[row].y;
            pLocal[row * 4 + 2] = rows[row].z;
        }
        pLocal[12] = pLocal[13] = pLocal[14] = 0.f;
        pLocal[15] = scale;
    }

    pRef->SetTransform(NiPoint3(acAt), NiPoint3(), 1.f);
    if (!pShape || !pRoot)
        return;

    // the root, where the reference now is, unturned: the shape's world is its local moved there
    auto* pWorld = reinterpret_cast<float*>(pShape + kWorldOffset);
    for (int row = 0; row < 3; ++row)
    {
        pWorld[row * 4 + 0] = rows[row].x;
        pWorld[row * 4 + 1] = rows[row].y;
        pWorld[row * 4 + 2] = rows[row].z;
    }
    const glm::vec3 rootAt = GetWorldPosition(pRoot);
    pWorld[12] = rootAt.x;
    pWorld[13] = rootAt.y;
    pWorld[14] = rootAt.z;
    pWorld[15] = scale;

    const float half = (aStretch ? aLength : kSpikeLength) * 0.5f;
    const glm::vec3 centre = rootAt + ahead * half;
    const float radius = aStretch ? half + 1.f : kSpikeBoundRadius;
    for (uint8_t* pNode : {pShape, pRoot})
    {
        auto* pBound = reinterpret_cast<float*>(pNode + kWorldBoundOffset);
        pBound[0] = centre.x;
        pBound[1] = centre.y;
        pBound[2] = centre.z;
        pBound[3] = radius;
    }
}

// The server's id of a character (local or another player's), 0 until the character sync has it
uint32_t ServerIdOf(uint32_t aFormId) noexcept
{
    auto& world = World::Get();
    for (const auto entity : world.view<FormIdComponent, LocalComponent>())
    {
        if (world.get<FormIdComponent>(entity).Id == aFormId)
            return world.get<LocalComponent>(entity).Id;
    }
    for (const auto entity : world.view<FormIdComponent, RemoteComponent>())
    {
        if (world.get<FormIdComponent>(entity).Id == aFormId)
            return world.get<RemoteComponent>(entity).Id;
    }
    return 0;
}

// Who the spike hit, biting a character's collision at acPoint: the nearest loaded actor around it
Actor* FindHooked(PlayerCharacter* apPlayer, const NiPoint3& acPoint) noexcept
{
    ProcessLists* pLists = ProcessLists::Get();
    if (!pLists)
        return nullptr;

    Actor* pBest = nullptr;
    float best = std::numeric_limits<float>::max();
    for (uint32_t i = 0; i < pLists->highActorHandleArray.length; ++i)
    {
        Actor* pActor = Cast<Actor>(TESObjectREFR::GetByHandle(pLists->highActorHandleArray[i]));
        if (!pActor || pActor == apPlayer || !pActor->parentCell)
            continue;
        const NiPoint3 at = pActor->position;
        if (acPoint.z < at.z - kHookReach || acPoint.z > at.z + pActor->GetHeight() + kHookReach)
            continue;
        const NiPoint3 low = pActor->GetBoundMin();
        const NiPoint3 high = pActor->GetBoundMax();
        const float width = std::max(high.x - low.x, high.y - low.y) * 0.5f;
        const float across = std::hypot(acPoint.x - at.x, acPoint.y - at.y);
        if (across > width + kHookReach || across >= best)
            continue;
        best = across;
        pBest = pActor;
    }
    return pBest;
}
} // namespace

GrappleService::GrappleService(World& aWorld, entt::dispatcher& aDispatcher)
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&GrappleService::OnUpdate>(this);
    m_movementActionConnection = aDispatcher.sink<NotifyMovementAction>().connect<&GrappleService::OnNotifyMovementAction>(this);
    s_pGrappleService = this;
}

GrappleService::~GrappleService() noexcept
{
    s_pGrappleService = nullptr;
}

bool GrappleService::TakeLaunch(const ProjectileLaunchData& acData) noexcept
{
    GrappleService* pService = s_pGrappleService;
    if (s_launchingHit || !pService || !acData.pShooter || !acData.pProjectileBase || acData.pShooter != PlayerCharacter::Get())
        return false;

    const size_t kind = SpikeKindOf(acData.pProjectileBase);
    if (kind >= std::size(kSpikes))
        return false;

    // the shot's way from the game's angles (radians: z the heading, 0 = +y; x the pitch, down from level)
    Shot shot{};
    shot.Origin = acData.Origin;
    shot.Direction = NiPoint3(glm::vec3(std::sin(acData.fZAngle) * std::cos(acData.fXAngle), std::cos(acData.fZAngle) * std::cos(acData.fXAngle),
                                        -std::sin(acData.fXAngle)));
    shot.Speed = kSpikes[kind].Speed;
    shot.Range = kSpikes[kind].Range;
    shot.pWeapon = acData.FromWeapon.pWeapon;
    shot.pInstance = acData.FromWeapon.pWeaponData;
    shot.pAmmo = acData.pFromAmmo;
    shot.pProjectile = acData.pProjectileBase;
    std::scoped_lock _{pService->m_shotLock};
    pService->m_pendingShot = shot;
    return true;
}

bool GrappleService::TakeJump(bool aJustPressed) noexcept
{
    GrappleService* pService = s_pGrappleService;
    if (!pService || !pService->m_holding)
        return false;
    if (aJustPressed)
        pService->m_jumpPressed = true;
    return true;
}

bool GrappleService::OwnsMarker() noexcept
{
    return s_pGrappleService && s_pGrappleService->m_ownsMarker;
}

bool GrappleService::HoldsPlayer() noexcept
{
    GrappleService* pService = s_pGrappleService;
    if (!pService)
        return false;
    if (pService->m_holding)
        return true;
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    return pPlayer && std::any_of(pService->m_reeledIn.begin(), pService->m_reeledIn.end(), [pPlayer](const ReeledIn& acReel) { return acReel.ActorId == pPlayer->formID; });
}

void GrappleService::SetAiming(bool aHeld, bool aOffHand, const TESForm* apAmmo) noexcept
{
    if (GrappleService* pService = s_pGrappleService)
    {
        pService->m_aiming = aHeld;
        pService->m_offHand = aOffHand;
        pService->m_pAimAmmo = apAmmo;
    }
}

bool GrappleService::IsLineOut() noexcept
{
    return s_pGrappleService && s_pGrappleService->m_line.Now != Stage::kIdle;
}

bool GrappleService::GetHang(glm::vec3& aAnchor, bool& aOffHand) noexcept
{
    GrappleService* pService = s_pGrappleService;
    if (!pService || !pService->m_holding || (pService->m_line.Now != Stage::kHanging && pService->m_line.Now != Stage::kPulling))
        return false;
    aAnchor = pService->m_line.Anchor;
    aOffHand = pService->m_offHand;
    return true;
}

Vector<std::pair<uint32_t, glm::vec3>> GrappleService::GetRemoteHangs() noexcept
{
    Vector<std::pair<uint32_t, glm::vec3>> hangs;
    GrappleService* pService = s_pGrappleService;
    if (!pService)
        return hangs;
    for (const auto& line : pService->m_remoteLines)
    {
        const float length = glm::distance(glm::vec3(line.From), glm::vec3(line.To));
        if (!line.Retracting && !line.HookedId && line.Flown >= length - 1.f)
            hangs.emplace_back(line.ShooterId, glm::vec3(line.Spike));
    }
    return hangs;
}

void GrappleService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("Grapple");
    const float delta = std::clamp(static_cast<float>(acEvent.Delta), 0.f, 0.1f);

    std::optional<Shot> shot;
    {
        std::scoped_lock _{m_shotLock};
        shot.swap(m_pendingShot);
    }

    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer || !pPlayer->parentCell)
    {
        if (m_line.Now != Stage::kIdle)
            Finish(pPlayer, "the player went away");
        // a load: the other players' lines go with the old world (their references are let go), and no one's reeled
        for (auto& line : m_remoteLines)
        {
            RemoveVisual(line.SpikeId);
            RemoveVisual(line.CableId);
        }
        m_remoteLines.clear();
        m_reeledIn.clear();
        m_ownsMarker = false;
        m_checkedCellId = 0;
        return;
    }

    if (const uint32_t cellId = pPlayer->GetCellId(); cellId != m_checkedCellId)
    {
        m_checkedCellId = cellId;
        ClearStrayVisuals(pPlayer);
    }
    UpdateRemoteLines(delta);
    UpdateReeledIn(pPlayer, delta);

    if (shot)
    {
        if (m_line.Now == Stage::kIdle)
            Fire(pPlayer, *shot);
        else
            spdlog::info("Grapple: fired with the spike still out, nothing shot");
    }
    if (m_line.Now != Stage::kIdle)
        Advance(pPlayer, delta);
    UpdateMarker(pPlayer, delta);
}

void GrappleService::Fire(PlayerCharacter* apPlayer, const Shot& acShot) noexcept
{
    const glm::vec3 origin = acShot.Origin;
    glm::vec3 direction = acShot.Direction;
    if (!std::isfinite(direction.x) || !std::isfinite(direction.y) || !std::isfinite(direction.z) || glm::length(direction) < 1e-3f)
        return;
    direction = glm::normalize(direction);

    Line line{};
    line.Now = Stage::kFlying;
    line.CellId = apPlayer->GetCellId();
    line.Origin = acShot.Origin;
    line.Direction = NiPoint3(direction);
    line.Speed = acShot.Speed;
    line.Range = acShot.Range;
    line.pWeapon = acShot.pWeapon;
    line.pInstance = acShot.pInstance;
    line.pAmmo = acShot.pAmmo;
    line.pProjectile = acShot.pProjectile;
    line.Spike = acShot.Origin;
    line.SpikeAhead = line.Direction;

    // Where it bites: the first thing along the shot in reach; past nothing it flies out to its reach and comes back
    const NiPoint3 end(origin + direction * acShot.Range);
    const WorldRays::RayHit hit = CastRay(apPlayer->parentCell, WorldRays::GetLineOfSightFilter(apPlayer), acShot.Origin, end);
    line.Length = acShot.Range;
    if (hit.Hit)
    {
        line.Bites = true;
        line.Hit = hit.Point;
        line.HitTop = hit.NormalZ >= kMinGroundNormalZ;
        // the spike's origin stops with its tip a little into what it bit
        line.Length = std::max(0.f, glm::distance(origin, glm::vec3(hit.Point)) - (kSpikeLength - kBiteDepth));
        if (WorldRays::IsCharacterLayer(hit.Layer))
        {
            if (const Actor* pHooked = FindHooked(apPlayer, hit.Point))
                line.HookedId = pHooked->formID;
        }
    }

    line.SpikeId = PlaceVisual(kSpikeVisualId, apPlayer, acShot.Origin);
    line.CableId = PlaceVisual(kCableVisualId, apPlayer, acShot.Origin);
    m_line = line;
    SetGunSpikeShown(apPlayer, false);
    // the other players' copies fly it too, to where it stops
    SendLine(RequestMovementAction::kGrappleShot, line.Origin, NiPoint3(origin + direction * line.Length), line.Speed, 0);

    glm::vec3 view;
    glm::vec3 right;
    GetView(apPlayer, view, right);
    spdlog::info("Grapple: shot {:.0f} reach ({:.2f}, {:.2f}, {:.2f}; the view looks {:.2f}, {:.2f}, {:.2f}): {}{}, spike {:X}, cable {:X}", acShot.Range,
                 direction.x, direction.y, direction.z, view.x, view.y, view.z,
                 line.Bites ? fmt::format("bites {:.0f} away{}", line.Length, line.HitTop ? " on top of something" : "") : std::string("nothing in reach"),
                 line.HookedId ? fmt::format(", someone ({:X})", line.HookedId) : std::string(), line.SpikeId, line.CableId);
}

void GrappleService::Advance(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    // Another cell or worldspace (a door, a load): the line can't go on there
    if (apPlayer->GetCellId() != m_line.CellId)
    {
        Finish(apPlayer, "the player went somewhere else");
        return;
    }
    if (m_holding && (apPlayer->IsDead() || apPlayer->actorState.IsBleedingOut()))
    {
        LetGo(apPlayer, NiPoint3(), "the player went down");
        return;
    }

    const bool jumped = std::exchange(m_jumpPressed, false);
    switch (m_line.Now)
    {
    case Stage::kFlying:
        m_line.Flown = std::min(m_line.Flown + m_line.Speed * aDelta, m_line.Length);
        m_line.Spike = NiPoint3(glm::vec3(m_line.Origin) + glm::vec3(m_line.Direction) * m_line.Flown);
        if (m_line.Flown >= m_line.Length)
            Arrive(apPlayer);
        break;
    case Stage::kPulling:
        if (jumped)
            JumpOff(apPlayer);
        else
            Pull(apPlayer, aDelta);
        break;
    case Stage::kHanging:
        if (jumped)
            JumpOff(apPlayer);
        else
            Hang(apPlayer, aDelta);
        break;
    case Stage::kReeling: Reel(apPlayer, aDelta); break;
    case Stage::kRetracting: Retract(apPlayer, aDelta); break;
    default: break;
    }

    if (m_line.Now != Stage::kIdle)
    {
        // kept hidden: a view switch or a weapon drawn again builds the gun's 3D anew
        SetGunSpikeShown(apPlayer, false);
        Draw(apPlayer);
    }
}

void GrappleService::Arrive(PlayerCharacter* apPlayer) noexcept
{
    if (!m_line.Bites)
    {
        StartRetracting("it bit nothing in reach");
        return;
    }

    // Someone: the spike's hit (the game's own), then the winch reels them in
    if (m_line.HookedId)
    {
        auto* pActor = Cast<Actor>(TESForm::GetById(m_line.HookedId));
        if (!pActor || pActor->IsDead() || !pActor->parentCell)
        {
            StartRetracting("who it hit is dead or gone");
            return;
        }
        HitHooked(apPlayer, pActor);
        if (pActor->IsDead() || pActor->GetHeight() > kReelMaxHeight)
        {
            StartRetracting(pActor->IsDead() ? "the spike's hit killed them" : "who it hooked is too big and pulled free");
            return;
        }
        m_line.Now = Stage::kReeling;
        m_line.HookOffset = NiPoint3(glm::vec3(m_line.Spike) - glm::vec3(pActor->position));
        m_line.HookTime = 0.f;

        // run by another player's game (an NPC of theirs, or another player): that game reels them in
        const ActorExtension* pExtension = pActor->GetExtension();
        m_line.ReeledElsewhere = pExtension && pExtension->IsRemote();
        if (m_line.ReeledElsewhere)
        {
            const uint32_t serverId = ServerIdOf(pActor->formID);
            if (!serverId)
            {
                StartRetracting("who it hooked isn't known to the server, no game to reel them in");
                return;
            }
            SendLine(RequestMovementAction::kGrappleHooked, m_line.Spike, apPlayer->position, kReelSpeed, serverId);
        }
        spdlog::info("Grapple: hooked {:X}, reeling them in{}", pActor->formID, m_line.ReeledElsewhere ? " (by the game that runs them)" : "");
        return;
    }

    if (apPlayer->IsInPowerArmor())
    {
        StartRetracting("the winch can't pull power armor");
        return;
    }

    // A wall or a top: the winch pulls the player there, up onto a ledge if one is in reach of where it bit
    const glm::vec3 flat(m_line.Direction.x, m_line.Direction.y, 0.f);
    const bool level = glm::length(flat) < 1e-3f;
    m_line.Heading = level ? apPlayer->rotation.z : std::atan2(flat.x, flat.y);
    NiPoint3 start;
    NiPoint3 mark;
    m_line.ToLedge = FindLedge(apPlayer, m_line.Hit, m_line.Heading, start, mark);

    // where the cable is fixed: out from a wall toward where the shot came from (the body hangs clear of it), on a top
    // where it bit
    glm::vec3 anchor = m_line.Hit;
    if (!m_line.HitTop && !level)
        anchor -= glm::normalize(flat) * kStandOff;
    m_line.Anchor = NiPoint3(anchor);

    if (m_line.ToLedge)
    {
        m_line.PullEnd = start;
    }
    else
    {
        // the hands just short of where the cable is fixed
        const glm::vec3 hands = glm::vec3(apPlayer->position) + glm::vec3(0.f, 0.f, kHandsOverFeet);
        const glm::vec3 toAnchor = anchor - hands;
        const float distance = glm::length(toAnchor);
        const glm::vec3 handsEnd = distance > kHangGap ? anchor - toAnchor / distance * kHangGap : hands;
        m_line.PullEnd = NiPoint3(handsEnd - glm::vec3(0.f, 0.f, kHandsOverFeet));
    }

    m_line.Now = Stage::kPulling;
    m_line.PullSpeed = kPullStartSpeed;
    m_line.Velocity = NiPoint3();
    m_line.Placed = apPlayer->position;
    m_holding = true;
    // On the motorcycle: the cable takes the rider off it, and it rolls on, slows and falls over by itself
    if (BikeLink::IsOnBike())
    {
        spdlog::info("Grapple: pulled off the motorcycle");
        BikeLink::Bail();
    }
    ParkourService::HoldPlayer(apPlayer);
    spdlog::info("Grapple: the spike bit {}, pulling the player {:.0f} to {}", m_line.HitTop ? "on top of something" : "a wall",
                 glm::distance(glm::vec3(apPlayer->position), glm::vec3(m_line.PullEnd)), m_line.ToLedge ? "a ledge to grab" : "hang there");
}

void GrappleService::Pull(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    const glm::vec3 feet = apPlayer->position;
    if (glm::distance(feet, glm::vec3(m_line.Placed)) > kMaxOffLine)
    {
        Finish(apPlayer, "the player was moved away");
        return;
    }

    m_line.PullSpeed = std::min(m_line.PullSpeed + kPullAcceleration * aDelta, kPullTopSpeed);
    const glm::vec3 toEnd = glm::vec3(m_line.PullEnd) - feet;
    const float left = glm::length(toEnd);
    const float step = m_line.PullSpeed * aDelta;
    if (left <= std::max(step, 1.f))
    {
        m_line.Velocity = left > 1e-3f ? NiPoint3(toEnd / left * m_line.PullSpeed) : NiPoint3();
        Place(apPlayer, m_line.PullEnd);
        EndPull(apPlayer);
        return;
    }

    // something in the way of the body: the pull stops there, the player on the cable
    const glm::vec3 way = toEnd / left;
    const glm::vec3 next = feet + way * step;
    const glm::vec3 middle(0.f, 0.f, kBodyMiddle);
    if (CastRay(apPlayer->parentCell, WorldRays::GetLineOfSightFilter(apPlayer), NiPoint3(feet + middle), NiPoint3(next + middle + way * kBodyRadius)).Hit)
    {
        spdlog::info("Grapple: something's in the way of the pull, {:.0f} short", left);
        m_line.Velocity = NiPoint3();
        StartHanging(apPlayer);
        return;
    }
    m_line.Velocity = NiPoint3(way * m_line.PullSpeed);
    Place(apPlayer, NiPoint3(next));
}

void GrappleService::EndPull(PlayerCharacter* apPlayer) noexcept
{
    if (m_line.ToLedge)
    {
        // facing the ledge, the parkour's grab from the air takes the player up onto it (and gives its collision back at
        // the end); the parkour's moves don't start while the winch holds the player
        apPlayer->rotation.z = m_line.Heading;
        m_holding = false;
        if (m_world.ctx().at<ParkourService>().GrabFromAir(apPlayer))
        {
            StartRetracting("pulled up to the ledge, the grab takes over");
            return;
        }
        m_holding = true;
        spdlog::info("Grapple: the ledge can't be grabbed from the end of the pull, hanging there");
    }
    StartHanging(apPlayer);
}

void GrappleService::StartHanging(PlayerCharacter* apPlayer) noexcept
{
    m_line.Now = Stage::kHanging;
    m_line.Hands = NiPoint3(glm::vec3(apPlayer->position) + glm::vec3(0.f, 0.f, kHandsOverFeet));
    m_line.Cable = std::clamp(glm::distance(glm::vec3(m_line.Hands), glm::vec3(m_line.Anchor)), kMinCable, m_line.Range);
    m_line.Velocity = NiPoint3(glm::vec3(m_line.Velocity) * kPullIntoSwing);
    spdlog::info("Grapple: hanging on {:.0f} of cable", m_line.Cable);
}

void GrappleService::Hang(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    if (glm::distance(glm::vec3(apPlayer->position), glm::vec3(m_line.Placed)) > kMaxOffLine)
    {
        Finish(apPlayer, "the player was moved away");
        return;
    }

    // forward and back climb the cable, the sides swing on it (the view's right, level)
    const glm::vec2 input = GetMoveInput();
    glm::vec3 forward;
    glm::vec3 right;
    GetView(apPlayer, forward, right);
    right.z = 0.f;
    right = glm::length(right) > 1e-3f ? glm::normalize(right) : glm::vec3(1.f, 0.f, 0.f);
    m_line.Cable = std::clamp(m_line.Cable - input.y * kClimbSpeed * aDelta, kMinCable, m_line.Range);

    glm::vec3 velocity = m_line.Velocity;
    velocity += right * (input.x * kSwingAcceleration * aDelta);
    velocity.z -= kGravity * aDelta;
    velocity *= std::max(0.f, 1.f - kDrag * aDelta);

    // the cable holds the hands at most its length from where it's fixed, taking away their speed outward
    const glm::vec3 anchor = m_line.Anchor;
    glm::vec3 hands = glm::vec3(m_line.Hands) + velocity * aDelta;
    const glm::vec3 out = hands - anchor;
    if (const float length = glm::length(out); length > m_line.Cable && length > 1e-3f)
    {
        const glm::vec3 along = out / length;
        hands = anchor + along * m_line.Cable;
        if (const float outward = glm::dot(velocity, along); outward > 0.f)
            velocity -= along * outward;
    }

    const glm::vec3 lift(0.f, 0.f, kHandsOverFeet);
    const glm::vec3 middle(0.f, 0.f, kBodyMiddle);
    const glm::vec3 feet = glm::vec3(m_line.Hands) - lift;
    glm::vec3 nextFeet = hands - lift;
    TESObjectCELL* pCell = apPlayer->parentCell;
    const uint32_t filter = WorldRays::GetLineOfSightFilter(apPlayer);

    // the body stops at what it would swing into
    const glm::vec3 move = nextFeet - feet;
    if (const float moved = glm::length(move); moved > 1e-3f && CastRay(pCell, filter, NiPoint3(feet + middle), NiPoint3(nextFeet + middle + move / moved * kBodyRadius)).Hit)
    {
        hands = m_line.Hands;
        nextFeet = feet;
        velocity = glm::vec3(0.f);
    }

    // the feet on the ground (climbed down the cable, or swung low): the player stands there, off the cable
    const WorldRays::RayHit ground = CastRay(pCell, filter, NiPoint3(nextFeet + middle), NiPoint3(nextFeet - glm::vec3(0.f, 0.f, 2.f)));
    if (ground.Hit && ground.NormalZ >= kMinGroundNormalZ)
    {
        Place(apPlayer, NiPoint3(glm::vec3(nextFeet.x, nextFeet.y, ground.Point.z)));
        LetGo(apPlayer, NiPoint3(), "the feet reached the ground");
        return;
    }

    m_line.Hands = NiPoint3(hands);
    m_line.Velocity = NiPoint3(velocity);
    Place(apPlayer, NiPoint3(nextFeet));
}

void GrappleService::JumpOff(PlayerCharacter* apPlayer) noexcept
{
    // a ledge the hands reach, the way the player faces: the parkour's grab takes the player up onto it
    auto& parkour = m_world.ctx().at<ParkourService>();
    if (parkour.FindGrabSpot(apPlayer, apPlayer->position, apPlayer->rotation.z))
    {
        m_holding = false;
        if (parkour.GrabFromAir(apPlayer))
        {
            StartRetracting("pulled up onto a ledge from the cable");
            return;
        }
        m_holding = true;
    }
    LetGo(apPlayer, m_line.Velocity, "jumped off the cable");
}

void GrappleService::LetGo(PlayerCharacter* apPlayer, const NiPoint3& acVelocity, const char* acpReason) noexcept
{
    if (m_holding)
    {
        m_holding = false;
        ParkourService::ReleasePlayer(apPlayer, acVelocity);
    }
    StartRetracting(acpReason);
}

void GrappleService::Reel(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    auto* pActor = Cast<Actor>(TESForm::GetById(m_line.HookedId));
    if (!pActor || pActor->IsDead() || !pActor->parentCell)
    {
        StartRetracting("who it hooked is dead or gone");
        return;
    }

    m_line.HookTime += aDelta;
    const glm::vec3 at = pActor->position;
    const glm::vec2 toPlayer(apPlayer->position.x - at.x, apPlayer->position.y - at.y);
    const float distance = glm::length(toPlayer);
    const float maxTime = kReelMaxTime + (m_line.ReeledElsewhere ? kReelElsewhereSlack : 0.f);
    if (distance <= kReelStop || m_line.HookTime >= maxTime)
    {
        StartRetracting(distance <= kReelStop ? "reeled them in" : "they pulled free");
        return;
    }
    // their game drags them (UpdateReeledIn there); the spike stays on them as they come
    if (m_line.ReeledElsewhere)
    {
        m_line.Spike = NiPoint3(at + glm::vec3(m_line.HookOffset));
        return;
    }

    // dragged along the ground toward the player (over a drop, they fall as they're dragged)
    const float step = std::min(kReelSpeed * aDelta, distance - kReelStop);
    glm::vec3 next = at + glm::vec3(toPlayer / distance * step, 0.f);
    const WorldRays::RayHit ground = CastRay(pActor->parentCell, WorldRays::GetLineOfSightFilter(pActor), NiPoint3(next + glm::vec3(0.f, 0.f, 60.f)),
                                             NiPoint3(next - glm::vec3(0.f, 0.f, 120.f)));
    if (ground.Hit)
        next.z = ground.Point.z;
    pActor->ForcePosition(NiPoint3(next));
    m_line.Spike = NiPoint3(next + glm::vec3(m_line.HookOffset));
}

void GrappleService::StartRetracting(const char* acpReason) noexcept
{
    m_line.Now = Stage::kRetracting;
    m_line.HookedId = 0;
    m_line.ReeledElsewhere = false;
    SendLine(RequestMovementAction::kGrappleRetract, m_line.Spike, m_line.Spike, kRetractSpeed, 0);
    spdlog::info("Grapple: {}; the spike reels back in", acpReason);
}

void GrappleService::Retract(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    const glm::vec3 toGun = GetMuzzle(apPlayer) - glm::vec3(m_line.Spike);
    const float left = glm::length(toGun);
    if (left <= kRetractSpeed * aDelta)
    {
        Finish(apPlayer, "the spike is back on the gun");
        return;
    }
    m_line.Spike = NiPoint3(glm::vec3(m_line.Spike) + toGun / left * (kRetractSpeed * aDelta));
    m_line.SpikeAhead = NiPoint3(-toGun / left);
}

void GrappleService::Finish(PlayerCharacter* apPlayer, const char* acpReason) noexcept
{
    // ended before it came back (the player went away, somewhere else): the other players' copies reel it in
    if (m_line.Now != Stage::kIdle && m_line.Now != Stage::kRetracting)
        SendLine(RequestMovementAction::kGrappleRetract, m_line.Spike, m_line.Spike, kRetractSpeed, 0);
    if (m_holding)
    {
        m_holding = false;
        ParkourService::ReleasePlayer(apPlayer, NiPoint3());
    }
    RemoveVisual(m_line.SpikeId);
    RemoveVisual(m_line.CableId);
    if (apPlayer)
        SetGunSpikeShown(apPlayer, true);
    m_line = Line{};
    m_jumpPressed = false;
    spdlog::info("Grapple: done, {}", acpReason);
}

void GrappleService::Place(PlayerCharacter* apPlayer, const NiPoint3& acFeet) noexcept
{
    ParkourService::PlacePlayer(apPlayer, acFeet);
    m_line.Placed = acFeet;
}

void GrappleService::Draw(PlayerCharacter* apPlayer) noexcept
{
    static BSFixedString s_spikeShape(kSpikeShape);
    static BSFixedString s_cableShape(kCableShape);

    // the spike, pointing the shot's way (away from the gun on its way back); the cable from the muzzle to its rear
    MoveVisual(m_line.SpikeId, s_spikeShape, m_line.Spike, m_line.SpikeAhead, kSpikeLength, false);
    const glm::vec3 muzzle = GetMuzzle(apPlayer);
    const glm::vec3 span = glm::vec3(m_line.Spike) - muzzle;
    const float length = glm::length(span);
    MoveVisual(m_line.CableId, s_cableShape, muzzle, length > 1e-3f ? span / length : glm::vec3(m_line.SpikeAhead), length, true);
}

bool GrappleService::FindLedge(PlayerCharacter* apPlayer, const NiPoint3& acHit, float aHeading, NiPoint3& aStart, NiPoint3& aMark) const noexcept
{
    const auto& parkour = m_world.ctx().at<ParkourService>();
    if (!parkour.GetRules().Parkour)
        return false;

    // from spots in front of where it bit, under it: the first that has a ledge for the grab
    const float forwardX = std::sin(aHeading);
    const float forwardY = std::cos(aHeading);
    for (const float under : kLedgeLookUnder)
    {
        const NiPoint3 feet(glm::vec3(acHit.x - forwardX * kLedgeLookBack, acHit.y - forwardY * kLedgeLookBack, acHit.z - under));
        if (const auto spot = parkour.FindGrabSpot(apPlayer, feet, aHeading))
        {
            aStart = spot->Start;
            aMark = spot->Mark;
            return true;
        }
    }
    return false;
}

bool GrappleService::IsGunDrawn(PlayerCharacter* apPlayer) const noexcept
{
    const uint32_t gunId = GunId();
    const TESForm* pWeapon = gunId ? apPlayer->GetEquippedWeapon(0) : nullptr;
    return pWeapon && pWeapon->formID == gunId && apPlayer->actorState.IsWeaponDrawn();
}

void GrappleService::UpdateMarker(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    const bool drawn = IsGunDrawn(apPlayer) || m_offHand;
    m_ownsMarker = drawn || m_line.Now != Stage::kIdle;

    // Aiming: the gun's button held with its spike on it (the shot goes when it's let go), no parkour move under way
    const bool aiming = drawn && m_aiming && m_line.Now == Stage::kIdle && !m_world.ctx().at<ParkourService>().IsPlayerMoving();
    if (!aiming)
    {
        m_marker.reset();
        m_markerTimer = 0.f;
        if (m_markerShown)
        {
            HudMarker::Hide();
            m_markerShown = false;
        }
        return;
    }

    if ((m_markerTimer -= aDelta) <= 0.f)
    {
        m_markerTimer = kMarkerInterval;
        m_marker.reset();

        // where the crosshair is, in a spike's reach: where the spike bites, or the ledge there the pull would end in a
        // grab of
        NiPoint3 eye;
        glm::vec3 forward;
        glm::vec3 right;
        GetView(apPlayer, forward, right);
        const glm::vec2 flat(forward.x, forward.y);
        if (HudMarker::GetCameraPosition(eye) && glm::length(flat) > 1e-3f)
        {
            const NiPoint3 end(glm::vec3(eye) + forward * ReachOf(m_pAimAmmo));
            const WorldRays::RayHit hit = CastRay(apPlayer->parentCell, WorldRays::GetLineOfSightFilter(apPlayer), eye, end);
            NiPoint3 start;
            NiPoint3 mark;
            if (hit.Hit && !WorldRays::IsCharacterLayer(hit.Layer) && FindLedge(apPlayer, hit.Point, std::atan2(flat.x, flat.y), start, mark))
            {
                m_marker = mark;
                m_markerIcon = HudMarker::Icon::kClimb;
            }
            else if (hit.Hit)
            {
                m_marker = hit.Point;
                m_markerIcon = HudMarker::Icon::kTarget;
            }
        }
    }

    if (m_marker)
    {
        HudMarker::Show(m_markerIcon, *m_marker, aDelta);
        m_markerShown = true;
    }
    else if (m_markerShown)
    {
        HudMarker::Hide();
        m_markerShown = false;
    }
}

void GrappleService::HitHooked(PlayerCharacter* apPlayer, Actor* apActor) noexcept
{
    TESObjectCELL* pCell = apActor->parentCell;
    if (!m_line.pProjectile || !m_line.pWeapon || !pCell)
    {
        spdlog::warn("Grapple: no projectile to hit {:X} with", apActor->formID);
        return;
    }

    // The spike's hit, the game's own: the gun's projectile (as it fired, its instance and spikes) launched at who it bit
    // from just short of their middle, so the game's projectile hit deals it (the gun's damage, its perks, a sneak
    // attack's, the hit reaction, aggression and crime, the kill's experience), and the launch goes to the other players
    // as any shot does. Its record is a missile (Tools/BuildGrapplePlugin.js): it doesn't stick in them, the spike seen
    // is the line's.
    const glm::vec3 middle = glm::vec3(apActor->position) + glm::vec3(0.f, 0.f, apActor->GetHeight() * kHitHeight);
    glm::vec3 way = middle - GetMuzzle(apPlayer);
    way = glm::length(way) > 1e-3f ? glm::normalize(way) : glm::vec3(m_line.Direction);
    const glm::vec3 origin = middle - way * kHitStandBack;

    ProjectileLaunchData data{};
    data.Origin = NiPoint3(origin);
    data.pProjectileBase = m_line.pProjectile;
    data.pShooter = apPlayer;
    data.FromWeapon.pWeapon = m_line.pWeapon;
    data.FromWeapon.pWeaponData = m_line.pInstance;
    data.pFromAmmo = static_cast<TESAmmo*>(m_line.pAmmo);
    data.EquipIndex = 0;
    data.fZAngle = std::atan2(way.x, way.y);
    data.fXAngle = -std::asin(std::clamp(way.z, -1.f, 1.f));
    data.pParentCell = pCell;
    data.eCastingSource = MagicSystem::CastingSource::OTHER;
    data.fPower = 1.f;
    data.fScale = 1.f;
    data.fConeOfFireRadiusMult = 1.f;
    data.eTargetLimb = -1;
    data.bUseOrigin = true;
    data.bAllow3D = true;
    data.bAlwaysHit = true;

    s_launchingHit = true;
    BSPointerHandle<Projectile> result;
    Projectile::LaunchShared(&result, data);
    s_launchingHit = false;
    spdlog::info("Grapple: the spike's hit on {:X} ({})", apActor->formID, result.handle.iBits ? "launched" : "not launched");
}

void GrappleService::SendLine(uint8_t aStage, const NiPoint3& acFrom, const NiPoint3& acTo, float aSpeed, uint32_t aTarget) noexcept
{
    auto& transport = m_world.GetTransport();
    if (!transport.IsOnline())
        return;
    RequestMovementAction request{};
    request.Type = RequestMovementAction::kGrapple;
    request.Grapple.Stage = aStage;
    request.Grapple.From[0] = acFrom.x;
    request.Grapple.From[1] = acFrom.y;
    request.Grapple.From[2] = acFrom.z;
    request.Grapple.To[0] = acTo.x;
    request.Grapple.To[1] = acTo.y;
    request.Grapple.To[2] = acTo.z;
    request.Grapple.Speed = aSpeed;
    request.Grapple.Target = aTarget;
    transport.Send(request);
}

void GrappleService::OnNotifyMovementAction(const NotifyMovementAction& acMessage) noexcept
{
    if (acMessage.Type != RequestMovementAction::kGrapple)
        return;
    auto* pShooter = Utils::GetByServerId<Actor>(acMessage.ServerId);
    if (!pShooter)
        return;
    const auto& grapple = acMessage.Grapple;
    auto it = std::find_if(m_remoteLines.begin(), m_remoteLines.end(), [pShooter](const RemoteLine& acLine) { return acLine.ShooterId == pShooter->formID; });

    switch (grapple.Stage)
    {
    case RequestMovementAction::kGrappleShot:
    {
        // a new shot: their copy's spike leaves the gun and flies to where it stops
        if (it == m_remoteLines.end())
        {
            m_remoteLines.push_back({});
            it = m_remoteLines.end() - 1;
        }
        RemoveVisual(it->SpikeId);
        RemoveVisual(it->CableId);
        RemoteLine line{};
        line.ShooterId = pShooter->formID;
        line.From = NiPoint3(glm::vec3(grapple.From[0], grapple.From[1], grapple.From[2]));
        line.To = NiPoint3(glm::vec3(grapple.To[0], grapple.To[1], grapple.To[2]));
        line.Speed = std::max(grapple.Speed, 100.f);
        line.Spike = line.From;
        const glm::vec3 way = glm::vec3(line.To) - glm::vec3(line.From);
        line.SpikeAhead = glm::length(way) > 1e-3f ? NiPoint3(glm::normalize(way)) : NiPoint3(glm::vec3(0.f, 1.f, 0.f));
        line.SpikeId = PlaceVisual(kSpikeVisualId, pShooter, line.From);
        line.CableId = PlaceVisual(kCableVisualId, pShooter, line.From);
        *it = line;
        break;
    }
    case RequestMovementAction::kGrappleHooked:
    {
        // it hooked someone: the spike stays on them, and if this game runs them it reels them toward the shooter
        auto* pHooked = Utils::GetByServerId<Actor>(grapple.Target);
        if (it != m_remoteLines.end() && pHooked)
            it->HookedId = pHooked->formID;
        const ActorExtension* pExtension = pHooked ? pHooked->GetExtension() : nullptr;
        // a message isn't taken on trust: someone in the same cell, in a spike's reach of the shooter
        const bool inReach = pHooked && pHooked->parentCell == pShooter->parentCell &&
                             glm::distance(glm::vec3(pHooked->position), glm::vec3(pShooter->position)) <= kSpikes[std::size(kSpikes) - 1].Range + kHookReach * 4.f;
        if (pHooked && inReach && !pHooked->IsDead() && pExtension && !pExtension->IsRemote())
        {
            // the local player off their own line first
            PlayerCharacter* pPlayer = PlayerCharacter::Get();
            if (pHooked == pPlayer && m_line.Now != Stage::kIdle)
                Finish(pPlayer, "pulled by another player's spike");
            if (pHooked == pPlayer)
                ParkourService::HoldPlayer(pPlayer);
            m_reeledIn.push_back({pHooked->formID, pShooter->formID, 0.f});
            spdlog::info("Grapple: {:X} hooked {:X}, reeling them in here", pShooter->formID, pHooked->formID);
        }
        break;
    }
    case RequestMovementAction::kGrappleRetract:
    {
        if (it != m_remoteLines.end())
        {
            it->Retracting = true;
            it->HookedId = 0;
            it->Age = 0.f;
        }
        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        for (size_t i = m_reeledIn.size(); i-- > 0;)
        {
            if (m_reeledIn[i].ShooterId == pShooter->formID)
                EndReeledIn(pPlayer, i, "their spike let go");
        }
        break;
    }
    default: break;
    }
}

void GrappleService::UpdateRemoteLines(float aDelta) noexcept
{
    static BSFixedString s_spikeShape(kSpikeShape);
    static BSFixedString s_cableShape(kCableShape);
    for (size_t i = 0; i < m_remoteLines.size();)
    {
        RemoteLine& line = m_remoteLines[i];
        auto* pShooter = Cast<Actor>(TESForm::GetById(line.ShooterId));
        uint8_t* pRoot = pShooter && pShooter->parentCell ? reinterpret_cast<uint8_t*>(pShooter->GetNiNode()) : nullptr;
        const auto muzzle = pRoot ? GetMuzzleOn(pRoot) : std::nullopt;
        line.Age += aDelta;
        bool done = !pShooter || !pShooter->parentCell || line.Age > kRemoteLineTimeout;

        if (!done)
        {
            const glm::vec3 from = muzzle ? *muzzle : glm::vec3(pShooter->position) + glm::vec3(0.f, 0.f, 100.f);
            if (line.Retracting)
            {
                // back onto their gun
                const glm::vec3 toGun = from - glm::vec3(line.Spike);
                const float left = glm::length(toGun);
                if (left <= kRetractSpeed * aDelta)
                    done = true;
                else
                {
                    line.Spike = NiPoint3(glm::vec3(line.Spike) + toGun / left * (kRetractSpeed * aDelta));
                    line.SpikeAhead = NiPoint3(-toGun / left);
                }
            }
            else if (auto* pHooked = line.HookedId ? Cast<Actor>(TESForm::GetById(line.HookedId)) : nullptr)
            {
                // on who it hooked, as they come
                line.Spike = NiPoint3(glm::vec3(pHooked->position) + glm::vec3(0.f, 0.f, pHooked->GetHeight() * kHitHeight));
            }
            else
            {
                // flying out to where it stops, and held there
                const glm::vec3 way = glm::vec3(line.To) - glm::vec3(line.From);
                const float length = glm::length(way);
                line.Flown = std::min(line.Flown + line.Speed * aDelta, length);
                line.Spike = length > 1e-3f ? NiPoint3(glm::vec3(line.From) + way / length * line.Flown) : line.To;
            }

            if (!done)
            {
                if (pRoot)
                    SetGunSpikeShownOn(pRoot, false);
                MoveVisual(line.SpikeId, s_spikeShape, line.Spike, line.SpikeAhead, kSpikeLength, false);
                const glm::vec3 span = glm::vec3(line.Spike) - from;
                const float length = glm::length(span);
                MoveVisual(line.CableId, s_cableShape, from, length > 1e-3f ? span / length : glm::vec3(line.SpikeAhead), length, true);
            }
        }

        if (!done)
        {
            ++i;
            continue;
        }
        RemoveVisual(line.SpikeId);
        RemoveVisual(line.CableId);
        if (pRoot)
            SetGunSpikeShownOn(pRoot, true);
        m_remoteLines[i] = m_remoteLines.back();
        m_remoteLines.pop_back();
    }
}

void GrappleService::UpdateReeledIn(PlayerCharacter* apPlayer, float aDelta) noexcept
{
    for (size_t i = m_reeledIn.size(); i-- > 0;)
    {
        ReeledIn& reel = m_reeledIn[i];
        auto* pActor = Cast<Actor>(TESForm::GetById(reel.ActorId));
        const auto* pShooter = Cast<Actor>(TESForm::GetById(reel.ShooterId));
        if (!pActor || !pShooter || pActor->IsDead() || !pActor->parentCell || pActor->parentCell != pShooter->parentCell)
        {
            EndReeledIn(apPlayer, i, "who hooked them or who they are is gone");
            continue;
        }
        reel.Time += aDelta;
        const glm::vec3 at = pActor->position;
        const glm::vec2 toShooter(pShooter->position.x - at.x, pShooter->position.y - at.y);
        const float distance = glm::length(toShooter);
        if (distance <= kReelStop || reel.Time >= kReelMaxTime)
        {
            EndReeledIn(apPlayer, i, distance <= kReelStop ? "reeled in" : "pulled free");
            continue;
        }

        // dragged along the ground toward the shooter, as the shooter's own reel drags who it hooks
        const float step = std::min(kReelSpeed * aDelta, distance - kReelStop);
        glm::vec3 next = at + glm::vec3(toShooter / distance * step, 0.f);
        const WorldRays::RayHit ground = CastRay(pActor->parentCell, WorldRays::GetLineOfSightFilter(pActor), NiPoint3(next + glm::vec3(0.f, 0.f, 60.f)),
                                                 NiPoint3(next - glm::vec3(0.f, 0.f, 120.f)));
        if (ground.Hit)
            next.z = ground.Point.z;
        if (pActor == apPlayer)
            ParkourService::PlacePlayer(apPlayer, NiPoint3(next));
        else
            pActor->ForcePosition(NiPoint3(next));
    }
}

void GrappleService::EndReeledIn(PlayerCharacter* apPlayer, size_t aIndex, const char* acpReason) noexcept
{
    const ReeledIn reel = m_reeledIn[aIndex];
    m_reeledIn.erase(m_reeledIn.begin() + static_cast<std::ptrdiff_t>(aIndex));
    if (apPlayer && reel.ActorId == apPlayer->formID)
        ParkourService::ReleasePlayer(apPlayer, NiPoint3());
    spdlog::info("Grapple: {:X} no longer reeled in by {:X}, {}", reel.ActorId, reel.ShooterId, acpReason);
}

void GrappleService::ClearStrayVisuals(PlayerCharacter* apPlayer) noexcept
{
    TESObjectCELL* pCell = apPlayer->parentCell;
    const uint32_t spikeBase = PluginForm(kSpikeVisualId);
    const uint32_t cableBase = PluginForm(kCableVisualId);
    if (!pCell || !pCell->objects.data || !spikeBase || !cableBase)
        return;

    // the references a line here has (the local one's, the other players')
    Vector<uint32_t> ours{m_line.SpikeId, m_line.CableId};
    for (const auto& line : m_remoteLines)
    {
        ours.push_back(line.SpikeId);
        ours.push_back(line.CableId);
    }
    Vector<TESObjectREFR*> strays;
    for (TESObjectREFR* pRef : pCell->objects)
    {
        if (!pRef || !pRef->baseForm || (pRef->baseForm->formID != spikeBase && pRef->baseForm->formID != cableBase))
            continue;
        if (std::find(ours.begin(), ours.end(), pRef->formID) == ours.end())
            strays.push_back(pRef);
    }
    for (TESObjectREFR* pRef : strays)
    {
        pRef->Disable();
        pRef->Delete();
    }
    if (!strays.empty())
        spdlog::info("Grapple: {} spike and cable visual(s) left from a save taken away", strays.size());
}

#endif
