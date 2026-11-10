#include <Services/CinematicService.h>

#if TP_FALLOUT4

#include <Events/UpdateEvent.h>
#include <World.h>
#include <PlayerCharacter.h>
#include <Forms/TESGlobal.h>
#include <Interface/HudMarker.h>
#include <BSGraphics/BSGraphicsRenderer.h>
#include <WinterOfAtom.h>
#include <HivemindApp.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <PerfProbe.h>

namespace
{
// PlayerCamera (CommonLibF4 id 1171980): currentState at 0x28 (a TESCameraState: its id at 0x20), cameraStates at 0xE0
constexpr uint32_t kFirstPersonState = 0;
constexpr uint32_t kFreeState = 3;
constexpr uint32_t kThirdPersonState = 8;
constexpr size_t kCurrentState = 0x28;
constexpr size_t kStateId = 0x20;
constexpr size_t kStates = 0xE0;
// FreeCameraState (1.10.163): GetTranslation reads +0x28/+0x2C/+0x30; GetRotation builds its rotation from +0x34 and
// +0x38 (radians; its input step adds to one and takes from the other, both wrapped, neither clamped)
constexpr size_t kFreeTranslation = 0x28;
constexpr size_t kFreeAngles[2] = {0x34, 0x38};

// calibration: each test angle is held this many frames before the view is read (the world camera follows a frame late)
constexpr float kTestAngle = 0.4f;
constexpr int kFramesPerStep = 3;
constexpr int kFlying = 4;
// a measurement that hasn't finished by then never will (10-22: the frames it waited on never came, the camera stuck)
constexpr float kMeasureSeconds = 3.f;
// a cinematic starts once the game draws this many frames between two of its 0.2 s checks (25 a second)
constexpr uint64_t kDrawingFrames = 5;

// NiAVObject flags, bit 0 hidden (as SelectionOutline): the player's body is shown for the cinematic (10-22 test 15:
// "my character was still being culled"), and hidden again after if it was
constexpr size_t kObjectFlags = 0x108;
constexpr uint64_t kObjectHidden = 1;

uint64_t* PlayerFlags() noexcept
{
    PlayerCharacter* pPlayer = PlayerCharacter::Get();
    NiNode* pRoot = pPlayer ? pPlayer->GetNiNode() : nullptr;
    return pRoot ? reinterpret_cast<uint64_t*>(reinterpret_cast<uint8_t*>(pRoot) + kObjectFlags) : nullptr;
}

constexpr float kPi = 3.14159265f;

// not a cinematic: turns the player around in place, no move (a Papyrus SetAngle on the player is a teleport, which
// closes the face editor it's meant to turn the player toward)
constexpr int kTurnAround = 100;

// the cinematics by number (RDWoA_CinematicRequest)
const char* CinematicName(int aNumber) noexcept
{
    switch (aNumber)
    {
    case 1: return "Opening";
    default: return nullptr;
    }
}

uint8_t* PlayerCamera() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_playerCamera, 1171981);
    return *s_playerCamera.Get();
}

uint8_t* CameraState(uint32_t aState) noexcept
{
    uint8_t* pCamera = PlayerCamera();
    return pCamera ? *reinterpret_cast<uint8_t**>(pCamera + kStates + aState * sizeof(void*)) : nullptr;
}

uint32_t CurrentStateId() noexcept
{
    const uint8_t* pCamera = PlayerCamera();
    const uint8_t* pState = pCamera ? *reinterpret_cast<uint8_t* const*>(pCamera + kCurrentState) : nullptr;
    return pState ? *reinterpret_cast<const uint32_t*>(pState + kStateId) : kThirdPersonState;
}

// PlayerCamera::SetState with one of its states (as PipboyMapService)
void SetCameraState(uint32_t aState) noexcept
{
    using TSetState = void(uint8_t*, uint8_t*);
    POINTER_FALLOUT4(TSetState, s_setState, 858848);
    uint8_t* pCamera = PlayerCamera();
    if (uint8_t* pState = CameraState(aState); pCamera && pState)
        s_setState.Get()(pCamera, pState);
}

// The free camera's input context (UserEvents::INPUT_CONTEXT_ID::kTFC): it maps the left and right mouse buttons to
// WorldZUp/WorldZDown and the wheel to CameraZUp/CameraZDown, over main gameplay's attack, aim and zoom. The game keeps
// it on exactly while the free camera is the camera's state, each place by hand: the console's tfc pushes and pops it,
// PlayerCamera::LoadGame (617926) pushes it for a save made in the free camera, and PlayerCamera::SaveGame (168565,
// from PlayerCharacter::SaveGame) pops it at its start and pushes it back at its end whenever the free camera is
// current. PlayerCamera::SetState doesn't. So a save during a cinematic that never pushed it left one on that nothing
// took off: Start Me Up's save, 20 s into the start (10-28 23:36:49 and 23:51:17, both mid-opening), then no attacking
// or zooming for the rest of the session, reloads included.
constexpr int32_t kFreeCameraContext = 10;

// ControlMap (CommonLibF4 id 325206): contextPriorityStack, a BSTArray of context ids, data at +0x128, size at +0x138
uint8_t* ControlMap() noexcept
{
    POINTER_FALLOUT4(uint8_t*, s_controlMap, 325207);
    return *s_controlMap.Get();
}

std::string InputContexts() noexcept
{
    const uint8_t* pMap = ControlMap();
    if (!pMap)
        return "?";
    const auto* pIds = *reinterpret_cast<const int32_t* const*>(pMap + 0x128);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(pMap + 0x138);
    std::string out;
    for (uint32_t i = 0; pIds && i < count && i < 16; ++i)
        out += fmt::format("{}{}", out.empty() ? "" : " ", pIds[i]);
    return "[" + out + "]";
}

bool HasFreeCameraContext() noexcept
{
    const uint8_t* pMap = ControlMap();
    const auto* pIds = pMap ? *reinterpret_cast<const int32_t* const*>(pMap + 0x128) : nullptr;
    const uint32_t count = pMap ? *reinterpret_cast<const uint32_t*>(pMap + 0x138) : 0;
    for (uint32_t i = 0; pIds && i < count && i < 16; ++i)
        if (pIds[i] == kFreeCameraContext)
            return true;
    return false;
}

// ControlMap::PushInputContext (1404410), as the console's tfc
void PushFreeCameraContext() noexcept
{
    using TPush = void(uint8_t*, int32_t);
    POINTER_FALLOUT4(TPush, s_push, 1404411);
    if (uint8_t* pMap = ControlMap())
        s_push.Get()(pMap, kFreeCameraContext);
}

// ControlMap::PopInputContext (74587) takes off the topmost one and says whether there was one: all of them, how many
int PopFreeCameraContexts() noexcept
{
    using TPop = bool(uint8_t*, int32_t);
    POINTER_FALLOUT4(TPop, s_pop, 74588);
    uint8_t* pMap = ControlMap();
    int count = 0;
    while (pMap && count < 16 && s_pop.Get()(pMap, kFreeCameraContext))
        ++count;
    return count;
}

// Where the world camera looks: heading (0 = +y, clockwise) and pitch (up positive), radians. The world to clip
// matrix's last row is the distance ahead, so its direction is the view's.
bool ViewAngles(float& aHeading, float& aPitch) noexcept
{
    float m[4][4];
    if (!HudMarker::GetWorldToClip(m))
        return false;
    const float x = m[3][0], y = m[3][1], z = m[3][2];
    const float length = std::sqrt(x * x + y * y + z * z);
    if (length < 1e-6f)
        return false;
    aHeading = std::atan2(x, y);
    aPitch = std::asin(std::clamp(z / length, -1.f, 1.f));
    return true;
}

float WrapAngle(float aAngle) noexcept
{
    while (aAngle > kPi)
        aAngle -= 2.f * kPi;
    while (aAngle < -kPi)
        aAngle += 2.f * kPi;
    return aAngle;
}

float GlobalValue(uint32_t aLocalId) noexcept
{
    const TESGlobal* pGlobal = WinterOfAtom::GetGlobal(aLocalId);
    return pGlobal ? pGlobal->f : 0.f;
}

void SetGlobal(uint32_t aLocalId, float aValue) noexcept
{
    if (TESGlobal* pGlobal = WinterOfAtom::GetGlobal(aLocalId))
        pGlobal->f = aValue;
}

bool SkipPressed() noexcept
{
    auto* pWindow = BSGraphics::GetMainWindow();
    if (!pWindow || !pWindow->IsForeground())
        return false;
    return (GetAsyncKeyState(VK_SPACE) & 0x8000) || (GetAsyncKeyState(VK_RETURN) & 0x8000) || (GetAsyncKeyState(VK_ESCAPE) & 0x8000);
}

float CatmullRom(float aP0, float aP1, float aP2, float aP3, float aU) noexcept
{
    const float u2 = aU * aU;
    const float u3 = u2 * aU;
    return 0.5f * (2.f * aP1 + (aP2 - aP0) * aU + (2.f * aP0 - 5.f * aP1 + 4.f * aP2 - aP3) * u2 + (3.f * aP1 - aP0 - 3.f * aP2 + aP3) * u3);
}

float SmootherStep(float aX) noexcept
{
    aX = std::clamp(aX, 0.f, 1.f);
    return aX * aX * aX * (aX * (aX * 6.f - 15.f) + 10.f);
}

// a number, or "player" and three numbers
template <class TPoint> bool ReadPoint(std::istringstream& aIn, TPoint& aOut) noexcept
{
    std::string word;
    if (!(aIn >> word))
        return false;
    int first = 0;
    aOut.PlayerRelative = word == "player";
    if (!aOut.PlayerRelative)
    {
        char* pEnd = nullptr;
        aOut.Value[0] = std::strtof(word.c_str(), &pEnd);
        if (pEnd == word.c_str())
            return false;
        first = 1;
    }
    for (int i = first; i < 3; ++i)
        if (!(aIn >> aOut.Value[i]))
            return false;
    return true;
}
} // namespace

CinematicService::CinematicService(World& aWorld, entt::dispatcher& aDispatcher) noexcept
    : m_world(aWorld)
{
    m_updateConnection = aDispatcher.sink<UpdateEvent>().connect<&CinematicService::OnUpdate>(this);
}

void CinematicService::OnUpdate(const UpdateEvent& acEvent) noexcept
{
    PerfProbe::Section _perf("Cinematic");
    if (!m_playing)
    {
        m_sinceCheck += acEvent.Delta;
        if (m_sinceCheck < 0.2)
            return;
        m_sinceCheck = 0.0;

        // a free camera input context with the camera in plain first or third person was left by someone (see
        // kFreeCameraContext): off, as the game would have taken it off leaving the free camera
        if (const uint32_t state = CurrentStateId(); (state == kFirstPersonState || state == kThirdPersonState) && HasFreeCameraContext())
        {
            const std::string before = InputContexts();
            const int removed = PopFreeCameraContexts();
            spdlog::warn("CinematicService: {} free camera input context{} on in camera state {} (input contexts {}): taken off, now {}", removed,
                         removed == 1 ? " was" : "s were", state, before, InputContexts());
        }

        // the game drawing again: a request comes right after the script moves the player, and a move is a load, where
        // the script VM goes on but frames don't (10-22 test 12: one frame in the measurement's 3 s, a hitch, then gameplay)
        const uint64_t frames = HivemindApp::GetFrameCount();
        const bool drawing = frames - m_checkFrame >= kDrawingFrames;
        m_checkFrame = frames;

        const int request = static_cast<int>(GlobalValue(WinterOfAtom::kCinematicRequest));
        PlayerCharacter* pPlayer = PlayerCharacter::Get();
        if (request <= 0 || !pPlayer)
            return;
        if (request == kTurnAround)
        {
            pPlayer->SetRotation(pPlayer->rotation.x, pPlayer->rotation.y, WrapAngle(pPlayer->rotation.z + kPi));
            spdlog::info("CinematicService: turned the player around to heading {:.3f}", pPlayer->rotation.z);
            SetGlobal(WinterOfAtom::kCinematicDone, static_cast<float>(request));
            SetGlobal(WinterOfAtom::kCinematicRequest, 0.f);
            return;
        }
        if (!drawing)
            return;
        if (!Start(request))
        {
            // nothing to play: say so at once, so the script waiting for it goes on
            SetGlobal(WinterOfAtom::kCinematicDone, static_cast<float>(request));
            SetGlobal(WinterOfAtom::kCinematicRequest, 0.f);
        }
        return;
    }

    // skippable from the start, measuring included, by a key pressed after it began (not the Enter that closed a menu)
    if (!SkipPressed())
        m_skipArmed = true;
    else if (m_skipArmed)
    {
        Finish(m_step == kFlying ? "skipped" : "skipped while measuring");
        return;
    }
    if (m_step != kFlying)
    {
        // a frame's time at most a tenth of a second: the first frame after a load's hitch brought several seconds at
        // once and ended the measurement before its first step (10-28, a save loaded in the start's opening)
        m_measureTime += std::min(static_cast<float>(acEvent.Delta), 0.1f);
        if (m_measureTime >= kMeasureSeconds)
        {
            Finish(fmt::format("gave up measuring the free camera at step {} (frame {}, waiting for {})", m_step, HivemindApp::GetFrameCount(), m_waitUntil).c_str());
            return;
        }
    }
    if (!Calibrate())
        return;

    m_time += static_cast<float>(acEvent.Delta);
    if (m_time >= m_duration)
    {
        Finish("ended");
        return;
    }
    Fly();
}

bool CinematicService::Start(int aNumber) noexcept
{
    const char* cpName = CinematicName(aNumber);
    if (!cpName)
    {
        spdlog::warn("CinematicService: no cinematic {}", aNumber);
        return false;
    }

    const auto path = std::filesystem::current_path() / L"Data" / L"RDWoA" / L"Cinematics" / (std::string(cpName) + ".txt");
    std::ifstream file(path);
    if (!file)
    {
        spdlog::warn("CinematicService: {} has no path file ({})", cpName, path.string());
        return false;
    }

    m_keys.clear();
    for (std::string line; std::getline(file, line);)
    {
        if (const size_t comment = line.find('#'); comment != std::string::npos)
            line.erase(comment);
        std::istringstream in(line);
        Key key;
        if (!(in >> key.Time))
            continue;
        if (!ReadPoint(in, key.Position) || !ReadPoint(in, key.Look))
        {
            spdlog::warn("CinematicService: {}: a key it can't read: {}", cpName, line);
            continue;
        }
        m_keys.push_back(key);
    }
    std::ranges::sort(m_keys, [](const Key& acLeft, const Key& acRight) { return acLeft.Time < acRight.Time; });
    if (m_keys.size() < 2 || !CameraState(kFreeState))
    {
        spdlog::warn("CinematicService: {}: {} keys, free camera {}", cpName, m_keys.size(), CameraState(kFreeState) ? "found" : "missing");
        m_keys.clear();
        return false;
    }

    m_duration = m_keys.back().Time;
    m_time = 0.f;
    m_step = 0;
    m_waitUntil = 0;
    m_measureTime = 0.f;
    m_skipArmed = false;
    m_previousState = CurrentStateId();
    m_playing = aNumber;
    SetCameraState(kFreeState);
    // the free camera's input context with it, as the game keeps them: a save meanwhile then takes it off and puts it
    // back, rather than putting one on that nothing takes off (kFreeCameraContext)
    PushFreeCameraContext();
    m_playerWasHidden = false;
    uint64_t* pFlags = PlayerFlags();
    if (pFlags && (*pFlags & kObjectHidden))
    {
        m_playerWasHidden = true;
        *pFlags &= ~kObjectHidden;
    }
    spdlog::info("CinematicService: playing {} ({} keys, {:.1f} s) from camera state {}; the player's 3D {} (flags {:#x}){}; input contexts {}", cpName, m_keys.size(),
                 m_duration, m_previousState, pFlags ? "found" : "missing", pFlags ? *pFlags : 0, m_playerWasHidden ? ", was hidden: shown" : "", InputContexts());
    return true;
}

bool CinematicService::Calibrate() noexcept
{
    uint8_t* pState = CameraState(kFreeState);
    if (!pState)
    {
        Finish("the free camera went away");
        return false;
    }
    if (m_step == kFlying)
        return true;

    // the camera waits at the path's start while measuring
    float start[3];
    PointAt(0.f, false, start);
    std::memcpy(pState + kFreeTranslation, start, sizeof(start));
    auto angle = [pState](int aField) -> float& { return *reinterpret_cast<float*>(pState + kFreeAngles[aField]); };
    const float tests[3][2] = {{0.f, 0.f}, {kTestAngle, 0.f}, {0.f, kTestAngle}};

    if (HivemindApp::GetFrameCount() < m_waitUntil)
    {
        angle(0) = tests[m_step - 1][0];
        angle(1) = tests[m_step - 1][1];
        return false;
    }

    if (m_step > 0)
    {
        float heading = 0.f, pitch = 0.f;
        if (!ViewAngles(heading, pitch))
        {
            Finish("no view to measure");
            return false;
        }
        if (m_step == 1)
        {
            m_baseHeading = heading;
            m_basePitch = pitch;
        }
        else
        {
            m_measured[m_step - 2][0] = WrapAngle(heading - m_baseHeading) / kTestAngle;
            m_measured[m_step - 2][1] = (pitch - m_basePitch) / kTestAngle;
        }
    }

    if (m_step == 3)
    {
        // the field that turns the view is the yaw, the other one tilts it; both should move it about a radian a radian
        m_yawField = std::abs(m_measured[0][0]) >= std::abs(m_measured[1][0]) ? 0 : 1;
        m_yawRate = m_measured[m_yawField][0];
        m_pitchRate = m_measured[1 - m_yawField][1];
        spdlog::info("CinematicService: free camera at angles 0 looks at heading {:.3f} pitch {:.3f}; +0x34 turns {:.3f} tilts {:.3f}, "
                     "+0x38 turns {:.3f} tilts {:.3f} per radian: yaw is +{:#x}",
                     m_baseHeading, m_basePitch, m_measured[0][0], m_measured[0][1], m_measured[1][0], m_measured[1][1], kFreeAngles[m_yawField]);
        if (std::abs(m_yawRate) < 0.5f || std::abs(m_yawRate) > 1.5f || std::abs(m_pitchRate) < 0.5f || std::abs(m_pitchRate) > 1.5f)
        {
            Finish("its angles didn't measure as a turn and a tilt");
            return false;
        }
        m_step = kFlying;
        return true;
    }

    angle(0) = tests[m_step][0];
    angle(1) = tests[m_step][1];
    ++m_step;
    m_waitUntil = HivemindApp::GetFrameCount() + kFramesPerStep;
    return false;
}

void CinematicService::Fly() noexcept
{
    uint8_t* pState = CameraState(kFreeState);
    if (!pState)
        return;

    float position[3], look[3];
    PointAt(m_time, false, position);
    PointAt(m_time, true, look);
    const float dx = look[0] - position[0], dy = look[1] - position[1], dz = look[2] - position[2];
    const float heading = std::atan2(dx, dy);
    const float pitch = std::atan2(dz, std::sqrt(dx * dx + dy * dy));

    std::memcpy(pState + kFreeTranslation, position, sizeof(position));
    *reinterpret_cast<float*>(pState + kFreeAngles[m_yawField]) = WrapAngle(heading - m_baseHeading) / m_yawRate;
    *reinterpret_cast<float*>(pState + kFreeAngles[1 - m_yawField]) = (pitch - m_basePitch) / m_pitchRate;
}

void CinematicService::Finish(const char* acpReason) noexcept
{
    SetCameraState(m_previousState == kFreeState ? kThirdPersonState : m_previousState);
    // out of the free camera: its input context off, this one's and any more (a save made in the free camera that was
    // loaded into this cinematic's start brings its own)
    const std::string contexts = InputContexts();
    const int removed = PopFreeCameraContexts();
    if (uint64_t* pFlags = PlayerFlags(); pFlags && m_playerWasHidden)
        *pFlags |= kObjectHidden;
    SetGlobal(WinterOfAtom::kCinematicDone, static_cast<float>(m_playing));
    SetGlobal(WinterOfAtom::kCinematicRequest, 0.f);
    spdlog::info("CinematicService: cinematic {} {} after {:.1f} s; free camera input contexts taken off {} (input contexts {}, now {})", m_playing, acpReason,
                 m_time, removed, contexts, InputContexts());
    m_playing = 0;
    m_keys.clear();
}

void CinematicService::ToWorld(const Point& acPoint, float aOut[3]) const noexcept
{
    if (!acPoint.PlayerRelative)
    {
        std::memcpy(aOut, acPoint.Value, sizeof(acPoint.Value));
        return;
    }
    const PlayerCharacter* pPlayer = PlayerCharacter::Get();
    if (!pPlayer)
    {
        std::memcpy(aOut, acPoint.Value, sizeof(acPoint.Value));
        return;
    }
    // the player's frame: right, forward (its heading, 0 = +y, clockwise) and up
    const float heading = pPlayer->rotation.z;
    const float forward[2] = {std::sin(heading), std::cos(heading)};
    const float right[2] = {std::cos(heading), -std::sin(heading)};
    aOut[0] = pPlayer->position.x + right[0] * acPoint.Value[0] + forward[0] * acPoint.Value[1];
    aOut[1] = pPlayer->position.y + right[1] * acPoint.Value[0] + forward[1] * acPoint.Value[1];
    aOut[2] = pPlayer->position.z + acPoint.Value[2];
}

void CinematicService::PointAt(float aTime, bool aLook, float aOut[3]) const noexcept
{
    const size_t count = m_keys.size();
    if (count == 0)
        return;

    const float time = m_duration * SmootherStep(m_duration > 0.f ? aTime / m_duration : 1.f);
    size_t i = 0;
    while (i + 2 < count && m_keys[i + 1].Time <= time)
        ++i;
    const float span = std::max(m_keys[std::min(i + 1, count - 1)].Time - m_keys[i].Time, 1e-3f);
    const float u = std::clamp((time - m_keys[i].Time) / span, 0.f, 1.f);

    float p[4][3];
    const size_t at[4] = {i > 0 ? i - 1 : 0, i, std::min(i + 1, count - 1), std::min(i + 2, count - 1)};
    for (int k = 0; k < 4; ++k)
        ToWorld(aLook ? m_keys[at[k]].Look : m_keys[at[k]].Position, p[k]);
    for (int axis = 0; axis < 3; ++axis)
        aOut[axis] = CatmullRom(p[0][axis], p[1][axis], p[2][axis], p[3][axis], u);
}

#endif
