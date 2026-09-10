#pragma once
#include <imgui/imgui.h>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <cstring>
#include "ui_localization_data.h"

// Display-only translation. Never use translated strings as config keys or backend names.
namespace MenuI18n
{
inline bool enabled = false;
struct Entry { std::string text, label; };
inline const auto& Catalog()
{
    static const auto catalog = [] {
        std::unordered_map<std::string_view, Entry> result;
        for (const auto& pair : translations)
            result.emplace(pair.en, Entry {pair.zh, std::string(pair.zh) + "###" + pair.en});
        return result;
    }();
    return catalog;
}
inline const char* Text(const char* original)
{
    if (!enabled || !original) return original;
    const auto& catalog = Catalog();
    auto it = catalog.find(original);
    return it == catalog.end() ? original : it->second.text.c_str();
}
inline const char* Label(const char* original)
{
    if (!enabled || !original || original[0] == '#') return original;
    const auto& catalog = Catalog();
    auto it = catalog.find(original);
    if (it != catalog.end()) return it->second.label.c_str();
    // Preserve the complete original identifier, including ##/### suffixes and seeds.
    const char* suffix = std::strstr(original, "##");
    if (!suffix) return original;
    it = catalog.find(std::string_view(original, suffix - original));
    if (it == catalog.end()) return original;
    static thread_local std::unordered_map<std::string, std::string> labels;
    auto [entry, inserted] = labels.try_emplace(original);
    if (inserted) entry->second = it->second.text + "###" + original;
    return entry->second.c_str();
}
template<class T> inline T Arg(T value) { return value; }
inline const char* Arg(const char* value) { return Text(value); }
inline const char* Arg(char* value) { return Text(value); }
}

namespace MenuI18nUI
{
template<class... T> inline void Text(const char* fmt, T... args)
{
    const bool wrap = std::strchr(fmt, '\n') != nullptr;
    if constexpr (sizeof...(args) == 0)
    {
        if (wrap) ImGui::TextWrapped("%s", MenuI18n::Text(fmt));
        else ImGui::TextUnformatted(MenuI18n::Text(fmt));
    }
    else
    {
        if (wrap) ImGui::TextWrapped(MenuI18n::Text(fmt), MenuI18n::Arg(args)...);
        else ImGui::Text(MenuI18n::Text(fmt), MenuI18n::Arg(args)...);
    }
}
template<class... T> inline void TextWrapped(const char* fmt, T... args)
{
    if constexpr (sizeof...(args) == 0) ImGui::TextWrapped("%s", MenuI18n::Text(fmt));
    else ImGui::TextWrapped(MenuI18n::Text(fmt), MenuI18n::Arg(args)...);
}
template<class... T> inline void TextDisabled(const char* fmt, T... args)
{
    if constexpr (sizeof...(args) == 0) ImGui::TextDisabled("%s", MenuI18n::Text(fmt));
    else ImGui::TextDisabled(MenuI18n::Text(fmt), MenuI18n::Arg(args)...);
}
template<class... T> inline void TextColored(const ImVec4& colour, const char* fmt, T... args)
{
    if constexpr (sizeof...(args) == 0) ImGui::TextColored(colour, "%s", MenuI18n::Text(fmt));
    else ImGui::TextColored(colour, MenuI18n::Text(fmt), MenuI18n::Arg(args)...);
}
template<class... T> inline void SetTooltip(const char* fmt, T... args)
{
    if constexpr (sizeof...(args) == 0) ImGui::SetTooltip("%s", MenuI18n::Text(fmt));
    else ImGui::SetTooltip(MenuI18n::Text(fmt), MenuI18n::Arg(args)...);
}
inline void TextUnformatted(const char* text, const char* end = nullptr)
{
    if (!end) { ImGui::TextUnformatted(MenuI18n::Text(text)); return; }
    const std::string copy(text, end);
    ImGui::TextUnformatted(MenuI18n::Text(copy.c_str()));
}
inline bool Combo(const char* label, int* current, const char* const items[], int count, int height = -1)
{
    std::vector<const char*> translated;
    translated.reserve(count);
    for (int i=0;i<count;++i) translated.push_back(MenuI18n::Text(items[i]));
    return ImGui::Combo(MenuI18n::Label(label), current, translated.data(), count, height);
}
}
