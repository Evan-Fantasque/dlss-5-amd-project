#pragma once
#include "ui_localization.h"
namespace NativeMenuHints
{
inline void Draw()
{
    MenuI18nUI::TextWrapped("FFXIV: select DLSS in the game. Choose the actual FSR backend under Upscalers, then press Change Upscaler. Game input and plugin output are separate settings.");
    MenuI18nUI::TextWrapped("AMD DLSS5 requires both Enable Neural Rendering and AMD: apply before Super Resolution. The game must be actively upscaling.");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,.78f,.35f,1.f));
    MenuI18nUI::TextWrapped("FFXIV ratio changes: enable Override all and both DRS overrides, save settings, then restart the game. This menu does not change the game's own graphics options.");
    ImGui::PopStyleColor();
    ImGui::Separator();
}
}
