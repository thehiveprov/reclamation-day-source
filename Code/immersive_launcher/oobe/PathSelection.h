// Copyright (C) 2021 TiltedPhoques SRL.
// For licensing information see LICENSE at the root of this distribution.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace oobe
{
bool SelectInstall(bool aForceSelect);

// A copy of the game on this PC: found from Bethesda's and Steam's install records, or picked by hand
struct GameInstall
{
    std::filesystem::path exe;
    std::wstring source; // where it was found, for the launcher's list
    std::wstring version;
    bool supported = false; // the version Reclamation Day runs on
};

// Each copy the install records know of, once
std::vector<GameInstall> FindGameInstalls();
// The copy chosen before, if it's still there
std::optional<GameInstall> GetChosenInstall();
GameInstall DescribeInstall(const std::filesystem::path& acExe, std::wstring aSource);
// Why that exe can't be used, empty when it can
std::wstring CheckGameExe(const std::filesystem::path& acExe);
bool SaveChosenInstall(const std::filesystem::path& acExe);
} // namespace oobe
