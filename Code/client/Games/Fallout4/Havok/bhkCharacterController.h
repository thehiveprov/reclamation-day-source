#pragma once

// The engine's character controller (the Havok body actors walk with). Only what our movement code uses; field
// offsets and vtable slots from CommonLibF4.
//
// Slots verified on 1.10.163 by disassembling the player's own controller vtable (bhkCharProxyController, +0x2e43c28)
// in a running game: 0x39 copies 16 bytes out of the character proxy (+0x470, then +0xA0), 0x3A copies them in, 0x3C
// takes the deferred-release collision array. (Address library id 248334 points 0x70 before that vtable, don't count
// slots from it.)
struct bhkCharacterController
{
    enum VirtualSlot : uint32_t
    {
        kGetLinearVelocity = 0x39,
        kSetLinearVelocity = 0x3A,
        kIntegrateStep = 0x3C,
    };

    struct Vector4
    {
        alignas(16) float x, y, z, w;
    };

    void GetLinearVelocity(Vector4& aVelocity) const noexcept
    {
        using TGetLinearVelocity = void (*)(const bhkCharacterController*, Vector4&);
        (*reinterpret_cast<TGetLinearVelocity* const*>(this))[kGetLinearVelocity](this, aVelocity);
    }

    void SetLinearVelocity(const Vector4& acVelocity) noexcept
    {
        using TSetLinearVelocity = void (*)(bhkCharacterController*, const Vector4&);
        (*reinterpret_cast<TSetLinearVelocity* const*>(this))[kSetLinearVelocity](this, acVelocity);
    }

    // The velocity the actor wants (input/animation), in its own frame: x to the right, y forward (Havok units).
    // Read in game: (0, 5.3, 0) running forward.
    [[nodiscard]] const Vector4& GetDesiredLocalVelocity() const noexcept { return *reinterpret_cast<const Vector4*>(reinterpret_cast<const uint8_t*>(this) + 0x190); }

    [[nodiscard]] float GetFallStartHeight() const noexcept { return *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(this) + 0x328); }
    // The height a fall counts from: a ledge grabbed in the air starts it over
    void SetFallStartHeight(float aHeight) noexcept { *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(this) + 0x328) = aHeight; }
    [[nodiscard]] float GetFallTime() const noexcept { return *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(this) + 0x32C); }
};
