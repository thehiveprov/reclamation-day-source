#pragma once

struct ActorState
{
    virtual ~ActorState();

    uint32_t flags1;
    uint32_t flags2;

    // flags2 bits 1-3: weapon state (CommonLibF4 WEAPON_STATE: sheathed, want to draw, drawing, drawn, want to sheathe,
    // sheathing). Drawn here includes on its way back.
    bool IsWeaponDrawn() const noexcept { return ((flags2 >> 1) & 7) >= 3; }
    // Out and staying out
    bool IsWeaponFullyDrawn() const noexcept { return ((flags2 >> 1) & 7) == 3; }
    uint32_t GetWeaponState() const noexcept { return (flags2 >> 1) & 7; }

    // Verified on Fallout 4 (read from game memory): life state is bits 17-20, 7 = essential down, 8 = bleedout
    bool IsBleedingOut() const noexcept { return ((flags1 >> 17) & 0xFu) - 7 <= 1; }

    // Verified on Fallout 4 1.10.163: IMovementState::DoGetSprinting (vtable slot 0x1F) returns flags1 bit 8
    bool IsSprinting() const noexcept { return (flags1 >> 8) & 1; }
    // Bits 0-13 of flags1 (CommonLibF4 moveMode); bit 7 is set while moving, bit 10 while swimming (seen in game)
    uint32_t GetMoveMode() const noexcept { return flags1 & 0x3FFF; }
    // flags2 bits 11-13 (CommonLibF4 stance)
    uint32_t GetStance() const noexcept { return (flags2 >> 11) & 7; }
    // flags1 bits 25-27 (CommonLibF4 meleeAttackState, after the life state's 17-20 and the knock state's 21-24): not 0
    // while a melee attack is under way (not yet seen in game: the dual wield log says when it changes)
    uint32_t GetMeleeAttackState() const noexcept { return (flags1 >> 25) & 7; }
    // flags2 bits 14-17 (CommonLibF4 gunState: GUN_STATE kDrawn 0, kRelaxed 1, kBlocked 2, kAlert 3, ...; the dual wield
    // log says it as it changes, beside the graph's own ready, alert or relaxed state)
    uint32_t GetGunState() const noexcept { return (flags2 >> 14) & 0xF; }
    // Written as it is, with nothing told to the animation graph (for the length of a shot)
    void SetGunStateRaw(uint32_t aState) noexcept { flags2 = (flags2 & ~(0xFu << 14)) | ((aState & 0xFu) << 14); }

    bool SetWeaponDrawn(bool aDraw) noexcept;
};
