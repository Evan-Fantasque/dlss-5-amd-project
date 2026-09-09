#pragma once
#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif
#include <imgui/imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

// Presentation only. The owner applies actions through the existing Config/State paths.
namespace SimpleSettings
{
struct Model
{
    bool chinese = false, inputActive = false, fsrSelected = false, canSelectFsr = false;
    const char* backend = "";
    bool ratioOverride = false, drsMin = false, drsMax = false, qualityOverride = false;
    bool upscaleRestart = false;
    float ratio = 1.3f;
    bool fgSupported = false, fgRouteActive = false, fgRouteConfigured = false;
    bool externalFg = false, fgEnabled = false, fgRestart = false;
    bool nrAvailable = false, nrEnabled = false;
    float nrPercent = 100.f, tone = 1.f, structure = 1.f, fpsLimit = 0.f;
    bool showFps = false;
};
struct Actions
{
    bool ffxivPreset = false, selectFsr = false, ratio = false, prepareFg = false, fg = false;
    bool nr = false, nrScale = false, tone = false, structure = false, limit = false, showFps = false;
    bool full = false, save = false, close = false;
};
struct EditState
{
    float nr = -1.f, tone = -1.f, structure = -1.f, limit = -1.f;
};
inline const char* Text(bool chinese, const char* zh, const char* en) { return chinese ? zh : en; }
#ifdef SIMPLE_SETTINGS_TEST
void RecordItem(const char* id);
#else
inline void RecordItem(const char*) {}
#endif
inline bool FfxivReady(const Model& m)
{
    return m.ratioOverride && m.drsMin && m.drsMax && !m.qualityOverride;
}
inline void ApplyFfxivPreset(Model& m)
{
    m.ratioOverride = m.drsMin = m.drsMax = true;
    m.qualityOverride = false;
    if (!std::isfinite(m.ratio) || m.ratio <= 0.f) m.ratio = 1.3f;
    m.upscaleRestart = true;
}
inline Actions Draw(Model& m, EditState& edit)
{
    Actions a;
    auto tr = [&m](const char* zh, const char* en) { return Text(m.chinese, zh, en); };
    ImGui::TextDisabled("FFXIV AMD | test10");
    ImGui::TextWrapped("%s", tr("常用选项在这里，更多调整请打开完整设置。", "Everyday controls. Open full settings for advanced options."));

    ImGui::Spacing();
    ImGui::SeparatorText(tr("FSR 超分辨率", "FSR Super Resolution"));
    ImGui::TextWrapped("%s: %s", tr("当前超分", "Current upscaler"), m.backend);
    if (!m.fsrSelected)
    {
        ImGui::BeginDisabled(!m.canSelectFsr);
        a.selectFsr = ImGui::Button(tr("切换到 FSR##useFsr", "Use FSR##useFsr")); RecordItem("useFsr");
        ImGui::EndDisabled();
    }
    if (!m.inputActive)
        ImGui::TextWrapped("%s", tr("等待游戏启用超分；FF14 中请选择 DLSS。", "Waiting for game upscaling. Select DLSS in FFXIV."));
    ImGui::BeginDisabled(!m.fsrSelected);
    char ratioLabel[64];
    if (m.ratioOverride) std::snprintf(ratioLabel, sizeof(ratioLabel), "%.2fx", m.ratio);
    else std::snprintf(ratioLabel, sizeof(ratioLabel), "%s", tr("尚未设置", "Not configured"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .53f);
    if (ImGui::BeginCombo(tr("超分倍率##ratio", "Upscale ratio##ratio"), ratioLabel))
    {
        const float values[] = {1.f, 1.3f, 1.5f, 1.7f, 2.f, 3.f};
        for (float v : values)
        {
            std::snprintf(ratioLabel, sizeof(ratioLabel), "%.1fx%s", v, v == 1.f ? tr("（原生分辨率抗锯齿）", " (native AA)") : "");
            if (ImGui::Selectable(ratioLabel, m.ratioOverride && std::abs(m.ratio - v) < .001f))
            { m.ratio = v; ApplyFfxivPreset(m); a.ratio = true; }
            if (v == 1.3f) RecordItem("ratio13");
        }
        ImGui::EndCombo();
    }
    RecordItem("ratio");
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", tr("倍率越大，渲染分辨率越低、负担越小。", "Higher ratios lower the render resolution and workload."));

    ImGui::TextWrapped("%s", FfxivReady(m) ? tr("FFXIV 必需设置：已配置", "FFXIV required settings: configured") : tr("FFXIV 必需设置：未配齐", "FFXIV required settings: incomplete"));
    ImGui::BeginDisabled(FfxivReady(m));
    a.ffxivPreset = ImGui::Button(tr("一键应用 FFXIV 必需设置##ffxivPreset", "Apply FFXIV required settings##ffxivPreset")); RecordItem("ffxivPreset");
    ImGui::EndDisabled();
    if (a.ffxivPreset) ApplyFfxivPreset(m);
    ImGui::TextWrapped("%s", tr("统一倍率并覆盖动态分辨率上下限，让游戏采用所选倍率。选择倍率也会自动补齐。", "Overrides all ratios and both DRS limits. Choosing a ratio also applies these settings."));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,.78f,.35f,1.f));
    ImGui::TextWrapped("%s", m.upscaleRestart ? tr("超分设置已修改：必须保存并重启游戏才能生效。", "Upscaling settings changed: save and restart the game to apply.") : tr("注意：修改超分倍率后，必须保存并重启游戏才能生效。", "After changing the upscale ratio, save and restart the game to apply."));
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::SeparatorText("FSR FG");
    if (m.externalFg)
        ImGui::TextWrapped("%s", tr("补帧由其他组件管理，请在完整设置中调整。", "External frame generation is active. Use full settings."));
    else if (m.fgRouteActive)
    {
        ImGui::BeginDisabled(!m.fgSupported && !m.fgEnabled);
        a.fg = ImGui::Checkbox(tr("启用 FSR 补帧##fg", "Enable FSR frame generation##fg"), &m.fgEnabled); RecordItem("fg");
        ImGui::EndDisabled();
        if (!m.inputActive) ImGui::TextDisabled("%s", tr("进入游戏并启用超分后运行。", "Runs when game upscaling is active."));
    }
    else
    {
        ImGui::BeginDisabled(!m.fgSupported || m.fgRouteConfigured);
        a.prepareFg = ImGui::Button(tr("配置 FSR 补帧##prepareFg", "Set up FSR frame generation##prepareFg")); RecordItem("prepareFg");
        ImGui::EndDisabled();
        ImGui::TextWrapped("%s", tr("保存并重启游戏后，可在这里开关补帧。", "Save and restart to enable the frame generation toggle."));
    }
    if (!m.fgSupported && !m.externalFg)
        ImGui::TextWrapped("%s", tr("尚未检测到 FSR FG 依赖。", "FSR FG dependency is not available yet."));
    if (m.fgRestart)
        ImGui::TextWrapped("%s", tr("补帧配置已更改：请保存并重启游戏。", "Frame generation setup changed: save and restart."));

    ImGui::Spacing();
    ImGui::SeparatorText(tr("DLSS5 神经渲染", "DLSS5 Neural Rendering"));
    ImGui::BeginDisabled(!m.nrAvailable && !m.nrEnabled);
    a.nr = ImGui::Checkbox(tr("启用 DLSS5##nr", "Enable DLSS5##nr"), &m.nrEnabled); RecordItem("nr");
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!m.nrAvailable || !m.nrEnabled);
    if (!m.nrAvailable || !m.nrEnabled) edit.nr = -1.f;
    float nr = edit.nr >= 0 ? edit.nr : m.nrPercent;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .53f);
    if (ImGui::SliderFloat(tr("处理分辨率##nrScale", "NR resolution##nrScale"), &nr, 25.f, 100.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) edit.nr = nr;
    RecordItem("nrScale");
    if (ImGui::IsItemDeactivatedAfterEdit() && edit.nr >= 0) { m.nrPercent = std::clamp(edit.nr,25.f,100.f); edit.nr = -1; a.nrScale = true; }
    if (!m.nrAvailable || !m.nrEnabled) { edit.tone = -1.f; edit.structure = -1.f; }
    float tone = edit.tone >= 0 ? edit.tone : m.tone;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .53f);
    if (ImGui::SliderFloat(tr("色调强度##tone", "Tone strength##tone"), &tone, 0.f, 2.f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) edit.tone = tone;
    RecordItem("tone");
    if (ImGui::IsItemDeactivatedAfterEdit() && edit.tone >= 0) { m.tone = std::clamp(edit.tone,0.f,2.f); edit.tone = -1.f; a.tone = true; }
    float structure = edit.structure >= 0 ? edit.structure : m.structure;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .53f);
    if (ImGui::SliderFloat(tr("结构强度##structure", "Structure strength##structure"), &structure, 0.f, 2.f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) edit.structure = structure;
    RecordItem("structure");
    if (ImGui::IsItemDeactivatedAfterEdit() && edit.structure >= 0) { m.structure = std::clamp(edit.structure,0.f,2.f); edit.structure = -1.f; a.structure = true; }
    ImGui::EndDisabled();
    ImGui::TextWrapped("%s", tr("分辨率可从 75% 起步。色调调整首轮风格，结构调整细节。松开滑块后应用。", "Try 75% NR resolution. Tone affects the first pass; structure adjusts detail. Applies on release."));
    if (!m.nrAvailable)
        ImGui::TextWrapped("%s", tr("未找到 AMD 神经渲染运行库，请检查安装。", "AMD NR runtime is missing. Check installation."));

    ImGui::Spacing();
    ImGui::SeparatorText(tr("帧率", "Frame rate"));
    float limit = edit.limit >= 0 ? edit.limit : m.fpsLimit;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * .53f);
    if (ImGui::SliderFloat(tr("帧率上限##limit", "FPS limit##limit"), &limit, 0.f, 240.f, "%.0f", ImGuiSliderFlags_AlwaysClamp)) edit.limit = limit;
    RecordItem("limit");
    if (ImGui::IsItemDeactivatedAfterEdit() && edit.limit >= 0) { m.fpsLimit = std::clamp(std::round(edit.limit),0.f,240.f); edit.limit = -1; a.limit = true; }
    ImGui::TextDisabled("%s", tr("0 表示不限制，沿用原生限帧方式。", "0 = unlimited. Uses OptiScaler's existing limiter."));
    a.showFps = ImGui::Checkbox(tr("显示帧率信息##showFps", "Show performance overlay##showFps"), &m.showFps); RecordItem("showFps");

    ImGui::Spacing();
    ImGui::Separator();
    a.full = ImGui::Button(tr("打开原生完整设置##full", "Open original full settings##full"), ImVec2(-1,0)); RecordItem("full");
    float width = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * .5f;
    a.save = ImGui::Button(tr("保存设置##save", "Save settings##save"), ImVec2(width,0)); RecordItem("save");
    ImGui::SameLine();
    a.close = ImGui::Button(tr("关闭##close", "Close##close"), ImVec2(-1,0)); RecordItem("close");
    return a;
}
}
