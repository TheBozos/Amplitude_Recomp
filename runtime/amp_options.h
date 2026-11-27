// The player's options for Amplitude (runtime/amp_options.cpp): the graphics options the native renderer and the
// window follow, the cheats, and quitting. Set from the window's Amplitude tab (F2, runtime/app/amp_settings_ui.cpp),
// read by the game thread: every value is atomic. The graphics options are kept in the user folder's amplitude.ini;
// the cheats start from their environment variables (AMP_AUTOPILOT, AMP_UNLOCK_ALL, AMP_INFINITE_ENERGY) and are not
// kept.
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>

namespace amp::options
{
    // Widescreen: the 3D drawn with a camera a third wider (16:9 over the game's 4:3), the HUD and menus kept at 4:3
    // in the middle, the picture shown at 16:9 (AMP_WIDESCREEN=1 at the first start).
    bool widescreen();
    void setWidescreen(bool on);
    // The x scale the native renderer gives the frame's clip space: 3/4 in widescreen, 1 otherwise.
    float clipScaleX();
    // Render-to-texture images (the track's surface, the monitors, the HUD's pictures) drawn at this many times their
    // GS size: 1 as the console, 2 sharper.
    int imageScale();
    void setImageScale(int scale);

    // The sound's lag the song's clock allows for (ms): the game times the gems and judges the hits on the EE's clock
    // since the song started (its task manager's timer), as if the music were heard as it is mixed; the port's sound
    // is heard some 50 ms later (the output's queue and the device). The clock is held back by this much
    // (AMP_AUDIO_OFFSET, or the Amplitude tab; 0: as the console).
    int audioOffsetMs();
    void setAudioOffsetMs(int ms);

    enum class Cheat
    {
        Autopilot,      // the game's own autopilot: it plays the gems itself
        UnlockAll,      // the game's own unlock-all flag: every arena and song open
        InfiniteEnergy, // the energy meter kept full: a song cannot be lost
        Count
    };
    bool cheat(Cheat c);
    void setCheat(Cheat c, bool on);
    // Once a game frame (the game thread): the cheats applied to the game's memory; one turned off undoes what it set.
    void applyCheats(uint8_t *ram);

    // amplitude.ini in the user folder `folder` (empty: nothing kept, the environment's switches only); load at startup,
    // save after a change.
    void load(const std::filesystem::path &folder);
    void save();

    // Quitting the game from the window's tab: the port's stop (the window closes once the game has ended).
    void setQuit(std::function<void()> quit);
    void quit();

    // The settings window opened on the Amplitude tab (the game's PC SETTINGS entry, runtime/amp_menu.cpp): the
    // window's request (bozo::app::requestSettings), nothing in a headless run.
    void setOpenSettings(std::function<void()> open);
    void openSettings();

    // Save states from the Amplitude tab: the window's own save and load of a slot (the Save states tab's, F5 / F7's).
    void setStateActions(std::function<void(int)> save, std::function<void(int)> load);
    bool hasStates();
    void saveState(int slot);
    void loadState(int slot);

    // The window's tab (runtime/app/amp_settings_ui.cpp, the window build only).
    void registerSettingsTab();
}
