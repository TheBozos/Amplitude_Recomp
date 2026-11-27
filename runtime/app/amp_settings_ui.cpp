// The window's Amplitude tab (F2, beside Controls and Video; a gamepad's middle button opens it too): the graphics
// options, the cheats, save states and quitting (runtime/amp_options.h); the game's PC SETTINGS entry opens it
// (runtime/amp_menu.cpp). Only in the window build.
#include "amp_options.h"

#include "bozo/app/panel.h"
#include "bozo/app/settings_tabs.h"
#include "bozo/app/window.h"
#include "imgui.h"

namespace amp::options
{
    void registerSettingsTab()
    {
        bozo::app::ui::registerTab("Amplitude", [] {
            ImGui::SeparatorText("Graphics");
            bool wide = widescreen();
            if (ImGui::Checkbox("Widescreen (16:9)", &wide))
            {
                setWidescreen(wide);
                bozo::app::setPictureAspect(wide ? 16.0f / 9.0f : 0.0f);
                save();
            }
            ImGui::TextDisabled("The 3D seen through a wider camera; the HUD and the menus stay 4:3 in the middle.");
            bool sharp = imageScale() > 1;
            if (ImGui::Checkbox("Sharper effects", &sharp))
            {
                setImageScale(sharp ? 2 : 1);
                save();
            }
            ImGui::TextDisabled("The pictures the game draws into textures (the track's surface, the monitors) at twice\n"
                                "their console size. The 3D's own resolution is the Video tab's render scale.");

            ImGui::SeparatorText("Sound");
            int offset = audioOffsetMs();
            ImGui::SetNextItemWidth(260);
            if (ImGui::SliderInt("Audio offset (ms)", &offset, 0, 200))
                setAudioOffsetMs(offset);
            if (ImGui::IsItemDeactivatedAfterEdit())
                save();
            ImGui::TextDisabled("How late the sound reaches you. The gems and the hits follow the music you hear:\n"
                                "raise it if you must hit early to score, lower it if you must hit late.");

            ImGui::SeparatorText("Cheats");
            bool autopilot = cheat(Cheat::Autopilot);
            if (ImGui::Checkbox("Autopilot", &autopilot))
                setCheat(Cheat::Autopilot, autopilot);
            ImGui::TextDisabled("The game's own autopilot: it plays the gems for you.");
            bool energy = cheat(Cheat::InfiniteEnergy);
            if (ImGui::Checkbox("Infinite energy", &energy))
                setCheat(Cheat::InfiniteEnergy, energy);
            ImGui::TextDisabled("The energy meter stays full: a song cannot be lost.");
            bool unlock = cheat(Cheat::UnlockAll);
            if (ImGui::Checkbox("Unlock all arenas and songs", &unlock))
                setCheat(Cheat::UnlockAll, unlock);
            ImGui::TextDisabled("The game's own unlock-all flag; turned off, the progress on the memory card counts again.");
            ImGui::TextDisabled("Cheats last for this session only.");

            if (hasStates())
            {
                // The slot is the window's (F5 / F7's, the Save states tab's, where each slot's picture is).
                ImGui::SeparatorText("Save states");
                int slot = bozo::app::stateSlot();
                ImGui::SetNextItemWidth(160);
                if (ImGui::SliderInt("Slot", &slot, 0, bozo::app::kStateSlots - 1))
                    bozo::app::setStateSlot(slot);
                ImGui::TextUnformatted(bozo::app::stateSlotSummary(bozo::app::stateSlot()).c_str());
                if (ImGui::Button("Save here"))
                    saveState(bozo::app::stateSlot());
                ImGui::SameLine();
                if (ImGui::Button("Load this slot"))
                    loadState(bozo::app::stateSlot());
                ImGui::TextDisabled("The Save states tab shows every slot with its picture; F5 saves, F7 loads.");
            }

            ImGui::SeparatorText("Game");
            static bool confirm = false;
            if (!confirm)
            {
                if (ImGui::Button("Quit the game"))
                    confirm = true;
            }
            else
            {
                ImGui::TextUnformatted("Quit? Anything not saved to the memory card is lost.");
                if (ImGui::Button("Yes, quit"))
                {
                    confirm = false;
                    quit();
                }
                ImGui::SameLine();
                if (ImGui::Button("No"))
                    confirm = false;
            }
        });
    }
} // namespace amp::options
