#pragma once

#include <Windows.h>

#include <string>

// What the launcher's page asks for (/api/<name>, see Code/launcher_ui/launcher.js): the state as JSON, the patch notes,
// and its choices. Called on CEF's UI thread; copying Game Files and fetching the notes run on threads of their own.
namespace launcher::ui
{
// Once the window is there: finds the installs and staging folders, starts fetching the patch notes
void StartApi(HWND aWindow);

// The JSON answer to /api/<name> with the request's body (empty for a GET)
std::string HandleApi(const std::string& acName, const std::string& acBody);

// The page's look, for the window's colour before the page draws
std::string GetStyle();
} // namespace launcher::ui
