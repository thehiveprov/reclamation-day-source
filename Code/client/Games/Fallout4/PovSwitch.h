#pragma once

// The third person key (TogglePOVHandler, hooked in PipboyMapService.cpp). The game switches the view only when its
// CanSwitchPOV (id 1498649, read from the 1.10.163 exe) says yes: the input layers' POV user event (0x20) and camera
// switch other event (8, InputEnableLayer.EnableCamSwitch) on, the camera target's gun state not 6 or 8, its graph's
// bBlockPOVSwitch off, no menu taking the controls. A press it says no to is logged with which of these held it off
// (the layers by name), and a bBlockPOVSwitch the graph has kept on with nothing going on (a save keeps graph variables:
// 10-28, the key did nothing after a Winter of Atom start) is turned off.
namespace PovSwitch
{
// A fresh press of the key, before the game's handler sees it; aMapUp: the realtime Pip-Boy map has it. Main thread.
void OnPressed(bool aMapUp) noexcept;
} // namespace PovSwitch
