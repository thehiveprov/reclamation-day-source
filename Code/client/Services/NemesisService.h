#pragma once

#include <Messages/NotifyNemesis.h>

struct World;
struct TransportService;
struct UpdateEvent;
struct ConnectedEvent;
struct DisconnectedEvent;
struct OverlayRestartedEvent;
struct ActorDamageEvent;
struct RequestNemesis;
struct Actor;
struct TESObjectREFR;
struct TESForm;

/**
 * @brief Nemeses (Fallout 4, see docs/NEMESIS.md), the game's side. The server keeps the nemesis world; this game
 * reports what its player does to gang men and what they do back, and spawns the men the server sends.
 *
 * Deeds: a fight is a character of a gang the local player hit or was hit by (Actor::HookDamageActor, ActorDamageEvent).
 * A few times a second the fights are looked at: the player going down (bleedout) blames the last man to hit them; a
 * man who dies soon after the player's hit is the player's kill (his gang mates who saw it may take it personally); a
 * man who lives through it (out of combat a minute, or gone) is reported wounded, crippled or just met in a gunfight.
 * The server decides who becomes a nemesis. A gang man near death may surrender; pressing E on him opens his
 * questioning card in the overlay.
 *
 * Encounters: the server sends one (NotifyNemesis::kSpawn) when the player can be met (RequestNemesis::kStatus every
 * few seconds: outside, not busy, not in a settlement). The men are placed out of sight on the ground, as their gang's
 * encounter NPCs for their rank, named, with their rank's resistances and KARMA alignment, and marked for deletion so
 * nothing stays in saves; their server ids go back once the character sync has them. The encounter ends when its men
 * are dead or gone, the player went down, or they're far apart. Every game names the nemeses it sees (kIdentity).
 *
 * The Pip-Boy's DATA > NEMESIS tab shows the server's pages for this player (NemesisPipboy).
 *
 * KARMA: its karma and alignment go to the server; the server's karma changes go through KARMA's ModKarma. The MCM
 * page "Nemeses" switches it all off, sets how often they come (the host's settings count) and whether gang men
 * surrender. Off without a server.
 */
class NemesisService
{
public:
    NemesisService(World& aWorld, entt::dispatcher& aDispatcher, TransportService& aTransport) noexcept;
    ~NemesisService() noexcept = default;

    TP_NOCOPYMOVE(NemesisService);

    // From the activation hook (the game's thread): true when it was a surrendered man (his questioning card opens and
    // the game's own activation doesn't happen)
    bool OnActivate(TESObjectREFR* apObject, Actor* apActivator) noexcept;
    // The overlay: nemeses(request, payload), see services/nemeses.service.ts
    void OnUiRequest(const std::string& acRequest, const std::string& acPayload) noexcept;
    // A named man's temper and fighting traits as the server sent them (kIdentity), for the enemy tactics; null for
    // anyone else
    [[nodiscard]] const NotifyNemesis::Identity* TemperamentOf(uint32_t aFormId) const noexcept;

private:
    struct Settings
    {
        bool Enabled{true};
        float EncounterHours{5.f};
        int EncounterChance{50};
        int CheatDeath{25};
        bool WorldNews{true};
        bool Surrender{true};
        bool KeepFaces{false};
        bool ShowTaunts{true};

        [[nodiscard]] std::string ForServer() const;
    };

    // A character the local player is fighting
    struct Fight
    {
        uint32_t FormId{};
        std::string Gang;
        uint8_t Body{};
        uint32_t RecordId{}; // a nemesis (his identity or his encounter), 0 a stranger
        double FirstAt{};
        double PlayerHitAt{-1.0};
        double HitPlayerAt{-1.0};
        double CombatAt{};
        float Damage{};
        float LowestHealth{1.f};
        uint8_t Crippled{};
        bool Surrendered{};
        bool SurrenderRolled{};
        bool Questioned{};
        bool Fled{}; // let go and running: back as he was once he's gone
        NiPoint3 Position{};
        std::string World;
        // The wound log: his limbs as last looked at, and the local player's hits since (sent with the limb they took)
        std::array<float, 6> Limbs{};
        bool LimbsKnown{};
        float PendingDamage{};
        uint16_t PendingHits{};
        float PendingRange{};
        uint8_t PendingCause{};
        std::string PendingWeapon;
        std::string PendingAmmo;
    };

    struct EncounterMan
    {
        NemesisMan Plan;
        uint32_t FormId{};
        uint32_t ServerId{};
    };

    struct Encounter
    {
        uint32_t Id{};
        NemesisSpawn Plan;
        std::vector<EncounterMan> Men;
        double SpawnAt{};      // when to place them (a frame after it came)
        double SpawnedAt{};    // 0 until placed
        double AttackAt{};     // a call-out says his piece first
        bool Reported{};       // kSpawned sent
        bool Engaged{};        // shots were fired
    };

    // A nemesis' name and rank for the character the server says he is
    struct Identity
    {
        uint32_t RecordId{};
        std::string Name;
        uint8_t Rank{1};
        NotifyNemesis::Identity Traits{};
        bool Met{};
        double Since{};
    };

    void OnUpdate(const UpdateEvent&) noexcept;
    void OnConnected(const ConnectedEvent&) noexcept;
    void OnDisconnected(const DisconnectedEvent&) noexcept;
    void OnOverlayRestarted(const OverlayRestartedEvent&) noexcept;
    void OnDamage(const ActorDamageEvent&) noexcept;
    void OnNotify(const NotifyNemesis&) noexcept;

    void LoadSettings() noexcept;
    void SendHello() noexcept;
    void SendStatus() noexcept;
    void CheckPlace() noexcept;
    void CheckDowned() noexcept;
    void CheckFights() noexcept;
    void CheckIdentities() noexcept;
    // The Pip-Boy's NEMESIS tab: its pages asked for now and then, on news, and when the Pip-Boy opens
    void CheckPipboy() noexcept;
    void RunEncounter() noexcept;
    void PlaceEncounter() noexcept;
    void EndEncounter(uint8_t aOutcome) noexcept;
    void ClearLeftovers(bool aAll) noexcept; // aAll: the near ones too
    void ResolveLived(Fight& aFight) noexcept;
    void ResolveDeath(Fight& aFight, Actor* apActor) noexcept;
    // He gives up (aHeldUp: held up with the game's Intimidation perk, his hands already up)
    void Surrender(Fight& aFight, Actor* apActor, bool aHeldUp = false) noexcept;
    void CheckHeldUp() noexcept;
    void Question(uint8_t aQuestion, int aCaps) noexcept;
    // A surrendered man's questioning card (from his activation, or Activate down the sights at gunpoint)
    void OpenCard(uint32_t aFormId) noexcept;
    void CloseCard() noexcept;

    // The wound log: the local player's hits on him since the last look, with the limb they took
    void SendWound(Fight& aFight, Actor* apActor) noexcept;
    // Gunpoint: the surrendered man the player holds their weapon on (the bar at the top), the nemesis screen's hotkey
    // and the Pip-Boy's OPEN FULL SCREEN entry
    void CheckGunpoint() noexcept;
    void CheckScreenKey() noexcept;
    void CheckQuestionKey() noexcept;
    void CheckInterrogateRequest() noexcept;
    void OpenScreen() noexcept;
    void CloseScreen() noexcept;
    void LoadKeys() noexcept;

    // A deed or death about a character, its stranger fields filled (gang, body, where)
    void SendDeed(uint8_t aDeed, const Fight& acFight, int aAmount = 0) noexcept;
    void SendDeath(const Fight& acFight, int32_t aFlags, uint8_t aCause) noexcept;
    void SendRequest(RequestNemesis& aRequest) noexcept;
    [[nodiscard]] uint32_t RecordOf(uint32_t aFormId) const noexcept;
    Fight* FightOf(uint32_t aFormId, bool aCreate) noexcept;

    World& m_world;
    TransportService& m_transport;
    Settings m_settings;
    std::string m_sentSettings;
    std::filesystem::file_time_type m_settingsTime{};
    bool m_settingsLoaded{false};

    uint32_t m_playerId{};
    bool m_helloSent{false};
    bool m_winterOfAtom{false};
    int m_karma{};
    int m_alignment{2};
    bool m_karmaKnown{false};
    double m_tickAt{};
    double m_statusAt{};
    double m_settingsAt{};
    double m_pagesAt{};
    double m_pagesWantedAt{}; // 0: nothing new
    bool m_pipboyOpen{false};
    TESForm* m_location{nullptr};
    bool m_wasDown{false};
    uint32_t m_lastAttacker{};
    double m_lastAttackerAt{-1.0};

    std::unordered_map<uint32_t, Fight> m_fights;           // by form id
    std::unordered_map<uint32_t, Identity> m_identities;    // by server id
    std::optional<Encounter> m_encounter;
    std::vector<uint32_t> m_leftovers;                      // the men of ended encounters, deleted once far (form ids)
    double m_leftoversAt{};
    uint32_t m_questioning{};                               // the surrendered man whose card is open (form id)
    uint32_t m_gunpoint{};                                  // the surrendered man held at gunpoint (form id), 0 none
    int m_activateKey{'E'};                                 // the game's Activate key (its control map), read at gunpoint
    bool m_questionKeyDown{false};
    int m_screenKey{'N'};                                   // config/Nemeses.ini [Keys] Screen (a virtual key)
    bool m_screenKeyDown{false};
    bool m_screenWanted{false};                             // asked for while the Pip-Boy was up: opens once it's down
    bool m_screenOpen{false};

    entt::scoped_connection m_updateConnection;
    entt::scoped_connection m_connectedConnection;
    entt::scoped_connection m_disconnectedConnection;
    entt::scoped_connection m_overlayRestartedConnection;
    entt::scoped_connection m_damageConnection;
    entt::scoped_connection m_notifyConnection;
};
