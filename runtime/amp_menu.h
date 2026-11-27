// SPDX-License-Identifier: MIT
// The PC SETTINGS entry of the game's Options and pause menus: its files made from the player's disc
// (runtime/amp_menu_files.cpp) and the hooks that open the window's settings from it (runtime/amp_menu.cpp).
#pragma once

#include <string>

namespace amp::menu
{
    // The game's menu and message files and the two panels' scenes edited from the disc at `iso` and given to the
    // archive overlay; false (the game's own files kept) when the disc is not the one expected or an edit does not fit.
    bool prepare(const std::string &iso);
}
