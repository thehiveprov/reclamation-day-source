#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/GridCellCoords.h>
#include <algorithm>

struct CellIdComponent
{
    CellIdComponent() {}

    CellIdComponent(GameId aCellId)
        : Cell(aCellId)
    {
    }

    CellIdComponent(GameId aCellId, GameId aWorldSpaceId, GridCellCoords aCenterCoords)
        : Cell(aCellId)
        , WorldSpaceId(aWorldSpaceId)
        , CenterCoords(aCenterCoords)
    {
    }

    bool operator==(const CellIdComponent& acRhs) const noexcept { return Cell == acRhs.Cell; }

    bool operator!=(const CellIdComponent& acRhs) const noexcept { return !operator==(acRhs); }

    operator bool() const noexcept { return *this != CellIdComponent{}; }

    bool IsInInteriorCell() const noexcept { return !WorldSpaceId; }

    bool IsInRange(const CellIdComponent& acRhs, bool aIsDragon) const noexcept
    {
        if (IsInInteriorCell())
            return Cell == acRhs.Cell;

        if (WorldSpaceId != acRhs.WorldSpaceId)
            return false;

        if (aIsDragon)
            return GridCellCoords::IsCellInGridCell(acRhs.CenterCoords, CenterCoords, true);

        return GridCellCoords::IsCellInGrid(acRhs.CenterCoords, CenterCoords, GetGridsToLoad(acRhs));
    }

    // The square of cells both sides span: a player's game may load more than the default (graphics presets raise
    // uGridsToLoad), and what's in there needs to be synced for it; never less than the default
    [[nodiscard]] int32_t GetGridsToLoad(const CellIdComponent& acRhs) const noexcept
    {
        return std::max({GridCellCoords::m_gridsToLoad, static_cast<int32_t>(GridsToLoad), static_cast<int32_t>(acRhs.GridsToLoad)});
    }

    GameId Cell{};
    GameId WorldSpaceId{};
    GridCellCoords CenterCoords{};
    // A player's: how many cells across the square their game keeps loaded is (0: a character's, the default)
    uint8_t GridsToLoad{0};
};
