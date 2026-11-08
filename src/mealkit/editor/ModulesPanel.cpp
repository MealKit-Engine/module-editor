#include "mealkit/editor/ModulesPanel.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/EditorColors.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace mealkit::editor {
namespace {

bool containsNoCase(const std::string& text, const char* needle) {
    if (!needle[0]) return true;
    const std::string n(needle);
    return std::search(text.begin(), text.end(), n.begin(), n.end(), [](char a, char b) {
               return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           }) != text.end();
}

// Every word of the search appears somewhere in the text
bool matchesWords(const std::string& text, const char* search) {
    std::string word;
    for (const char* c = search;; ++c) {
        if (*c == ' ' || *c == 0) {
            if (!word.empty() && !containsNoCase(text, word.c_str())) return false;
            word.clear();
            if (*c == 0) return true;
        } else word += *c;
    }
}

std::vector<std::string> splitWords(const std::string& text) {
    std::vector<std::string> words;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find_first_of(" \t", start);
        if (end == std::string::npos) end = text.size();
        if (end > start) words.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return words;
}

std::string quotePath(const std::string& s) { return "\"" + s + "\""; }

std::filesystem::path toPath(const std::string& utf8) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string fromPath(const std::filesystem::path& path) {
    const std::u8string s = path.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

// module.txt "requires": module names (the version numbers after them are dropped)
std::vector<std::string> readRequires(const std::string& moduleDir) {
    std::vector<std::string> names;
    std::string text;
    if (!readTextFile(moduleDir + "\\module.txt", text)) return names;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::vector<std::string> words = splitWords(text.substr(start, end - start));
        if (!words.empty() && words[0] == "requires")
            for (size_t i = 1; i < words.size(); ++i)
                if (!std::isdigit(static_cast<unsigned char>(words[i][0]))) names.push_back(words[i]);
        start = end + 1;
    }
    return names;
}

// Icon keywords (mkcontract kModuleIconKeywords) as Segoe Fluent Icons / MDL2 Assets code points, like icon:: in ImGuiLayer.h
struct IconGlyph {
    const char* keyword;
    const char* glyph;
};
constexpr IconGlyph kIconGlyphs[] = {
    {"gameplay", "\xEE\x9F\xBC"}, // U+E7FC game controller
    {"game", "\xEE\x9F\xBC"},
    {"timer", "\xEE\xA4\x96"},    // U+E916 stopwatch
    {"combat", "\xEE\xA8\x98"},   // U+EA18 shield
    {"health", "\xEE\xAD\x91"},   // U+EB51 heart
    {"ai", "\xEE\xA6\x9A"},       // U+E99A robot
    {"map", "\xEE\x9C\x87"},      // U+E707 map pin
    {"save", "\xEE\x9D\x8E"},     // U+E74E save
    {"ui", "\xEE\x9F\xB4"},       // U+E7F4 screen
    {"audio", "\xEE\x9D\xA7"},    // U+E767 volume
    {"render", "\xEE\x9E\x90"},   // U+E790 color
    {"rhi", "\xEE\x9E\x90"},
    {"camera", "\xEE\x9C\xA2"},   // U+E722 camera
    {"physics", "\xEF\x85\x98"},  // U+F158 3D cube
    {"net", "\xEE\xA5\xA8"},      // U+E968 network
    {"online", "\xEE\x9D\xB4"},   // U+E774 globe
    {"input", "\xEE\x9D\xA5"},    // U+E765 keyboard
    {"data", "\xEE\xA3\xAF"},     // U+E8EF (icon::kData)
    {"asset", "\xEE\xA2\xB7"},    // U+E8B7 folder
    {"scene", "\xEE\xA3\xBD"},    // U+E8FD (icon::kTree)
    {"debug", "\xEE\xAF\xA8"},    // U+EBE8 bug
    {"tool", "\xEE\xA4\x8F"},     // U+E90F wrench
    {"editor", "\xEE\xA4\x8F"},
    {"settings", "\xEE\x9C\x93"}, // U+E713 gear
    {"loc", "\xEE\xA3\x81"},      // U+E8C1 characters
    {"feedback", "\xEE\xA5\x85"}, // U+E945 lightning bolt
    {"live", "\xEE\xA2\x95"},     // U+E895 sync
    {"session", "\xEE\x9C\x96"},  // U+E716 people
    {"sim", "\xEE\xA7\xB5"},      // U+E9F5 (icon::kState)
};
constexpr const char* kPuzzle = "\xEE\xAA\x86"; // U+EA86 puzzle piece

ImU32 textColor(bool disabled) { return ImGui::GetColorU32(disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text); }

std::string joined(const std::vector<std::string>& words) {
    std::string out;
    for (const std::string& w : words) out += (out.empty() ? "" : ", ") + w;
    return out;
}

} // namespace

const char* moduleGlyph(const std::string& icon, const std::string& tags, const std::string& name) {
    const std::string keyword = mkcontract::moduleIconKeyword(icon, tags, name);
    for (const IconGlyph& g : kIconGlyphs)
        if (keyword == g.keyword) return g.glyph;
    return kPuzzle;
}

void ModulesPanel::run(const std::string& label, const std::string& arguments, Job job) {
    if (m_process.running()) return;
    const std::string script = m_engineDir + "\\tools\\module.ps1";
    if (!fileExists(script)) {
        m_status = trf("Module tool not found: %s", script.c_str());
        return;
    }
    const std::string command = "powershell -NoProfile -ExecutionPolicy Bypass -File " + quotePath(script) + " " + arguments +
                                " -Project " + quotePath(m_projectDir);
    m_output.clear();
    m_label = label;
    m_job = job;
    if (!m_process.start(command, m_projectDir)) {
        m_status = trf("Could not start: %s", label.c_str());
        return;
    }
    MEALKIT_INFO("Module tool: %s", arguments.c_str());
}

void ModulesPanel::poll() {
    if (!m_process.running()) return;
    if (m_process.poll(m_output)) return;
    const bool ok = m_process.exitCode() == 0;
    switch (m_job) {
    case Job::List:
        m_list = parseModuleList(m_output);
        if (!ok) m_list.error = m_output;
        rebuildItems();
        return;
    case Job::Versions:
        m_versions.clear();
        m_pickedVersion = 0;
        for (const std::string& line : [&] {
                 std::vector<std::string> lines;
                 size_t start = 0;
                 while (start < m_output.size()) {
                     size_t end = m_output.find('\n', start);
                     if (end == std::string::npos) end = m_output.size();
                     lines.push_back(m_output.substr(start, end - start));
                     start = end + 1;
                 }
                 return lines;
             }()) {
            std::vector<std::string> f;
            size_t from = 0;
            for (size_t tab = line.find('\t'); tab != std::string::npos; tab = line.find('\t', from)) {
                f.push_back(line.substr(from, tab - from));
                from = tab + 1;
            }
            f.push_back(line.substr(from));
            if (!f.back().empty() && f.back().back() == '\r') f.back().pop_back();
            if (f.size() >= 4 && f[0] == "version") m_versions.push_back({f[1], f[2], f[3] != "no"});
        }
        if (m_versions.empty()) m_status = ok ? trf("%s: no versions found", m_label.c_str()) : trf("%s: failed - see the Modules window", m_label.c_str());
        if (!ok) {
            m_log = m_output;
            m_logFailed = true;
        }
        return;
    case Job::Preview:
        m_log = m_output;
        m_logFailed = !ok;
        return;
    case Job::Change:
        m_log = m_output;
        m_logFailed = !ok;
        m_status = ok ? trf("%s: done", m_label.c_str()) : trf("%s: failed - see the Modules window", m_label.c_str());
        if (ok) m_changed = true;
        m_needList = true; // recheck the current state whether it succeeded or not
        m_versionsFor.clear();
        return;
    }
}

bool ModulesPanel::anyEngineInstalled() const {
    return std::any_of(m_items.begin(), m_items.end(), [](const Item& i) { return i.installed && i.engine; });
}

void ModulesPanel::rebuildItems() {
    m_items.clear();
    const auto findRegistry = [this](const std::string& name) -> const RegistryModule* {
        for (const RegistryModule& r : m_list.registry)
            if (r.name == name) return &r;
        return nullptr;
    };
    for (const InstalledModule& m : m_list.installed) {
        Item item;
        item.name = m.name;
        item.installed = &m;
        item.registry = findRegistry(m.name);
        item.title = m.title.empty() ? m.name : m.title;
        item.summary = m.summary;
        item.tags = m.tags;
        item.icon = m.icon;
        item.engine = m.type == "engine" || m.source.rfind("engine ", 0) == 0;
        if (item.registry) {
            if (item.summary.empty()) item.summary = item.registry->summary;
            if (item.tags.empty()) item.tags = item.registry->tags;
            if (item.icon.empty()) item.icon = item.registry->icon;
            item.author = item.registry->author;
            item.repo = item.registry->repo;
            const bool fromRegistry = m.source.rfind("registry ", 0) == 0;
            item.update = (fromRegistry || item.engine) && !item.registry->version.empty() && item.registry->version != m.version;
        }
        m_items.push_back(std::move(item));
    }
    for (const RegistryModule& r : m_list.registry) {
        if (std::any_of(m_items.begin(), m_items.end(), [&](const Item& i) { return i.name == r.name; })) continue;
        Item item;
        item.name = r.name;
        item.registry = &r;
        item.title = r.title.empty() ? r.name : r.title;
        item.summary = r.summary;
        item.tags = r.tags;
        item.icon = r.icon;
        item.author = r.author;
        item.repo = r.repo;
        item.engine = mkcontract::hasTag(r.tags, "engine");
        m_items.push_back(std::move(item));
    }
    // No engine modules in modules/: the game builds with every engine module of its engine
    if (!anyEngineInstalled())
        for (Item& item : m_items) item.inUse = item.engine && !item.installed;
    if (m_selected.empty() && !m_items.empty()) m_selected = m_items.front().name;
    m_details.name.clear(); // reread for the new state
}

void ModulesPanel::readDetails(const std::string& name) {
    m_details = {};
    m_details.name = name;
    const auto found = std::find_if(m_items.begin(), m_items.end(), [&](const Item& i) { return i.name == name; });
    if (found == m_items.end()) return;
    const std::string projectModules = m_projectDir + "\\modules\\";
    const std::string dir = found->installed ? projectModules + name : found->engine ? m_engineDir + "\\modules\\" + name : std::string();
    if (!dir.empty()) m_details.needs = readRequires(dir);
    const bool engineAll = !anyEngineInstalled(); // engine modules are all in the build
    for (const std::string& need : m_details.needs) {
        const auto have = std::find_if(m_items.begin(), m_items.end(), [&](const Item& i) { return i.name == need; });
        const bool inBuild = have != m_items.end() && (have->installed || (have->engine && engineAll));
        if (!inBuild) m_details.missing.push_back(need);
    }
    for (const Item& other : m_items)
        if (other.installed && other.name != name) {
            const std::vector<std::string> needs = readRequires(projectModules + other.name);
            if (std::find(needs.begin(), needs.end(), name) != needs.end()) m_details.neededBy.push_back(other.name);
        }
    if (found->installed) {
        std::error_code ec;
        const std::filesystem::path root = toPath(projectModules + name);
        for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".new")
                m_details.newFiles.push_back(fromPath(std::filesystem::relative(it->path(), root, ec).replace_extension()));
        std::sort(m_details.newFiles.begin(), m_details.newFiles.end());
    }
}

bool ModulesPanel::visible(const Item& item) const {
    switch (m_filter) {
    case Filter::All: break;
    case Filter::Installed:
        if (!item.installed && !item.inUse) return false;
        break;
    case Filter::Engine:
        if (!item.engine) return false;
        break;
    case Filter::Community:
        if (item.engine) return false;
        break;
    case Filter::Updates:
        if (!item.update) return false;
        break;
    }
    if (!m_tag.empty() && !mkcontract::hasTag(item.tags, m_tag)) return false;
    return matchesWords(item.name + " " + item.title + " " + item.summary + " " + item.tags + " " + item.author, m_search);
}

int ModulesPanel::updateCount() const {
    return static_cast<int>(std::count_if(m_items.begin(), m_items.end(), [](const Item& i) { return i.update; }));
}

bool ModulesPanel::tagChips(const std::string& tags) {
    bool clicked = false;
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, ImGui::GetFontSize());
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetFontSize() * 0.5f, 0.0f));
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    bool first = true;
    for (const std::string& tag : splitWords(tags)) {
        const float width = ImGui::CalcTextSize(tag.c_str()).x + ImGui::GetFontSize();
        if (!first) {
            ImGui::SameLine(0.0f, style.ItemSpacing.x * 0.5f);
            if (ImGui::GetCursorScreenPos().x + width > right) ImGui::NewLine();
        }
        first = false;
        const bool active = tag == m_tag;
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(active ? ImGuiCol_ButtonActive : ImGuiCol_FrameBg));
        ImGui::PushID(tag.c_str());
        if (ImGui::SmallButton(tag.c_str())) {
            m_tag = active ? std::string() : tag;
            clicked = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", active ? tr("Show every tag") : trf("Show only modules tagged %s", tag.c_str()).c_str());
        ImGui::PopID();
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleVar(2);
    return clicked;
}

void ModulesPanel::drawToolbar(bool busy, bool editing) {
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(tr("Refresh"))) m_needList = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13.0f);
    ImGui::InputTextWithHint("##search", (std::string(icon::kSearch) + "  " + tr("Search")).c_str(), m_search, sizeof(m_search));
    ImGui::SameLine();
    const char* filters[] = {tr("All"), tr("In this project"), tr("Engine"), tr("Community"), tr("Updates")};
    int filter = static_cast<int>(m_filter);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
    if (ImGui::Combo("##filter", &filter, filters, IM_ARRAYSIZE(filters))) m_filter = static_cast<Filter>(filter);
    if (!m_tag.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton((trf("Tag: %s", m_tag.c_str()) + "  x").c_str())) m_tag.clear();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Show every tag"));
    }
    ImGui::SameLine();
    ImGui::Checkbox(tr("Compact"), &m_compact);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("One line per module"));
    if (const int updates = updateCount(); updates > 0) {
        ImGui::SameLine();
        ImGui::BeginDisabled(busy || !editing);
        if (ImGui::Button(trf("Update All (%d)", updates).c_str())) m_confirmUpdateAll = true;
        ImGui::EndDisabled();
    }
    if (busy) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s...", m_label.c_str());
    } else if (!editing) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", tr("Stop playing to change modules."));
    }
}

void ModulesPanel::drawRow(const Item& item) {
    const float line = ImGui::GetTextLineHeight();
    const float height = m_compact ? line : line * 2.0f + ImGui::GetStyle().ItemSpacing.y * 0.5f;
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImGui::PushID(item.name.c_str());
    if (ImGui::Selectable("##row", m_selected == item.name, ImGuiSelectableFlags_None, ImVec2(0.0f, height))) m_selected = item.name;
    if (ImGui::IsItemHovered() && !item.summary.empty() && m_compact) ImGui::SetTooltip("%s", item.summary.c_str());
    ImGui::PopID();
    const ImVec2 end(ImGui::GetItemRectMax().x, start.y + height);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float iconBox = height;
    drawGlyph(draw, moduleGlyph(item.icon, item.tags, item.name), start, ImVec2(start.x + iconBox, end.y),
              m_compact ? ImGui::GetFontSize() : ImGui::GetFontSize() * 1.5f, textColor(false));

    // Version on the right: installed one (-> newer), or what would be added
    std::string version;
    ImU32 versionColor = textColor(true);
    if (item.installed) {
        version = item.installed->version;
        if (item.update) {
            version += " -> " + item.registry->version;
            versionColor = ImGui::GetColorU32(color::kWarning);
        }
    } else if (item.registry) {
        if (item.registry->version.empty()) {
            version = trf("%s (engine %s)", item.registry->latest.c_str(), item.registry->latestEngine.c_str());
            versionColor = ImGui::GetColorU32(color::kWarning);
        } else version = item.registry->version;
        if (item.inUse) {
            version = std::string(tr("in use")) + "  " + version;
            versionColor = ImGui::GetColorU32(color::kGood);
        }
    }
    const float pad = ImGui::GetFontSize() * 0.4f;
    const float versionWidth = ImGui::CalcTextSize(version.c_str()).x;
    const float textLeft = start.x + iconBox + pad;
    const float versionLeft = std::max(textLeft, end.x - versionWidth - pad);

    draw->PushClipRect(ImVec2(textLeft, start.y), ImVec2(versionLeft - pad, end.y), true);
    draw->AddText(ImVec2(textLeft, start.y), textColor(false), item.title.c_str());
    if (item.title != item.name) {
        const float titleWidth = ImGui::CalcTextSize(item.title.c_str()).x;
        draw->AddText(ImVec2(textLeft + titleWidth + pad, start.y), textColor(true), item.name.c_str());
    }
    draw->PopClipRect();
    draw->AddText(ImVec2(versionLeft, start.y), versionColor, version.c_str());
    if (!m_compact) {
        draw->PushClipRect(ImVec2(textLeft, start.y), ImVec2(end.x - pad, end.y), true);
        const std::string second = item.summary.empty() ? item.tags : item.summary;
        draw->AddText(ImVec2(textLeft, start.y + line + ImGui::GetStyle().ItemSpacing.y * 0.5f), textColor(true), second.c_str());
        draw->PopClipRect();
    }
}

void ModulesPanel::drawList() {
    int installedCount = 0, availableCount = 0;
    for (const Item& item : m_items)
        if (visible(item)) ++(item.installed ? installedCount : availableCount);

    if (ImGui::CollapsingHeader((trf("In this project (%d)", installedCount) + "###installed").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_list.installed.empty()) ImGui::TextDisabled("%s", tr("No modules. Add one from the list below."));
        for (const Item& item : m_items)
            if (item.installed && visible(item)) drawRow(item);
        if (!anyEngineInstalled()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", tr("No engine modules here: the game builds with every engine module of its engine (marked in use below). Adding one makes it build with only those in modules/."));
            ImGui::PopTextWrapPos();
        }
    }
    if (ImGui::CollapsingHeader((trf("Available (%d)", availableCount) + "###available").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!m_list.error.empty()) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(color::kDanger, "%s", m_list.error.c_str());
            ImGui::PopTextWrapPos();
        }
        for (const Item& item : m_items)
            if (!item.installed && visible(item)) drawRow(item);
    }
    if (installedCount + availableCount == 0 && !m_items.empty()) ImGui::TextDisabled("%s", tr("Nothing matches the search."));
}

void ModulesPanel::add(const Item& item, const std::string& spec, bool withRequires) {
    // The first engine module changes the game from every engine module to only those in modules/: ask first
    if (item.engine && !anyEngineInstalled()) {
        m_confirmEngine = spec;
        m_confirmEngineRequires = withRequires;
        return;
    }
    run(trf("Add %s", spec.c_str()), "add " + spec + (withRequires ? " -WithRequires" : ""), Job::Change);
}

void ModulesPanel::drawDetails(bool canChange) {
    const auto found = std::find_if(m_items.begin(), m_items.end(), [this](const Item& i) { return i.name == m_selected; });
    if (found == m_items.end()) {
        ImGui::TextDisabled("%s", tr("Pick a module on the left."));
        return;
    }
    const Item& item = *found;
    if (m_details.name != item.name) readDetails(item.name);

    // Picture, title, name and kind
    const float big = ImGui::GetFontSize() * 3.0f;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, ImVec2(at.x + big, at.y + big), ImGui::GetColorU32(ImGuiCol_FrameBg), ImGui::GetFontSize() * 0.4f);
    drawGlyph(draw, moduleGlyph(item.icon, item.tags, item.name), at, ImVec2(at.x + big, at.y + big), ImGui::GetFontSize() * 1.8f, textColor(false));
    ImGui::Dummy(ImVec2(big, big));
    ImGui::SameLine();
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 1.3f);
    ImGui::TextUnformatted(item.title.c_str());
    ImGui::PopFont();
    const char* kind = item.engine ? tr("engine module") : tr("source module");
    if (item.author.empty()) ImGui::TextDisabled("%s  -  %s", item.name.c_str(), kind);
    else ImGui::TextDisabled("%s  -  %s  -  %s", item.name.c_str(), kind, trf("by %s", item.author.c_str()).c_str());
    if (item.installed) {
        if (item.update) ImGui::TextColored(color::kWarning, "%s", trf("Version %s, %s available", item.installed->version.c_str(), item.registry->version.c_str()).c_str());
        else ImGui::TextColored(color::kGood, "%s", trf("Version %s, in this project", item.installed->version.c_str()).c_str());
    } else if (item.inUse) {
        ImGui::TextColored(color::kGood, "%s", trf("Version %s, in use from the engine", item.registry->version.c_str()).c_str());
    } else if (item.registry) {
        if (item.registry->version.empty())
            ImGui::TextColored(color::kWarning, "%s", trf("Newest %s needs engine %s; none for this engine", item.registry->latest.c_str(), item.registry->latestEngine.c_str()).c_str());
        else ImGui::TextUnformatted(trf("Version %s", item.registry->version.c_str()).c_str());
    }
    ImGui::EndGroup();

    if (!item.tags.empty()) {
        ImGui::Spacing();
        tagChips(item.tags);
    }
    if (!item.summary.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", item.summary.c_str());
    }

    // Dependencies
    if (!m_details.needs.empty() || !m_details.neededBy.empty()) ImGui::Spacing();
    if (!m_details.needs.empty()) ImGui::TextUnformatted(trf("Requires: %s", joined(m_details.needs).c_str()).c_str());
    if (!m_details.missing.empty())
        ImGui::TextColored(color::kWarning, "%s", trf("Not in the build: %s", joined(m_details.missing).c_str()).c_str());
    if (!m_details.neededBy.empty()) ImGui::TextDisabled("%s", trf("Required by: %s", joined(m_details.neededBy).c_str()).c_str());

    // State of the copy in the project
    if (item.installed) {
        ImGui::Spacing();
        const InstalledModule& m = *item.installed;
        if (m.modified > 0) ImGui::TextUnformatted(trf("%d file(s) changed here", m.modified).c_str());
        else if (m_details.newFiles.empty()) ImGui::TextDisabled("%s", tr("No files changed here"));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", trf("From %s", m.source.c_str()).c_str());
        ImGui::PopTextWrapPos();
    }

    // .new files left by an update: compare, take the module's version or keep yours
    if (!m_details.newFiles.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(color::kWarning, "%s", trf("%d .new file(s) to merge", static_cast<int>(m_details.newFiles.size())).c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("You changed these files, so the update wrote its version next to them as .new"));
        const std::string dir = m_projectDir + "\\modules\\" + item.name + "\\";
        for (const std::string& rel : m_details.newFiles) {
            std::string native = rel;
            std::replace(native.begin(), native.end(), '/', '\\');
            ImGui::PushID(rel.c_str());
            ImGui::BulletText("%s", rel.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Open Both"))) {
                openInExplorer(dir + native);
                openInExplorer(dir + native + ".new");
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!canChange);
            if (ImGui::SmallButton(tr("Use New")))
                run(trf("Merge %s", rel.c_str()), "resolve " + item.name + " -File " + quotePath(rel) + " -Use new", Job::Change);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Replace your file with the module's version"));
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Keep Mine")))
                run(trf("Merge %s", rel.c_str()), "resolve " + item.name + " -File " + quotePath(rel) + " -Use mine", Job::Change);
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Keep your file (merge by hand first) and drop the .new"));
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }

    // Actions
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::BeginDisabled(!canChange);
    bool any = false;
    const auto next = [&any] {
        if (any) ImGui::SameLine();
        any = true;
    };
    if (item.installed) {
        next();
        if (ImGui::Button(tr("Update")))
            run(trf("Update %s", item.name.c_str()), "update " + item.name, Job::Change);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr("Files you changed are kept; the new version is written next to them as .new"));
        next();
        if (ImGui::Button(tr("Remove"))) m_confirmRemove = item.name;
    } else if (item.registry && !item.registry->version.empty()) {
        next();
        if (ImGui::Button((std::string(icon::kAdd) + "  " + tr("Add")).c_str())) add(item, item.name, false);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", item.engine ? tr("Copies the engine's module into modules/") : tr("Module code runs inside the editor and the game. The registry checks that it builds, not what it does - read what you add."));
        next();
        if (ImGui::Button(tr("Add with Required"))) add(item, item.name, true);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Also adds the modules its requires line lists"));
    }
    ImGui::EndDisabled();
    if (item.installed) {
        const std::string docs = m_projectDir + "\\modules\\" + item.name + "\\docs";
        next();
        ImGui::BeginDisabled(!fileExists(docs + "\\index.md"));
        if (ImGui::Button(tr("Docs"))) openInExplorer(docs + "\\index.md");
        ImGui::EndDisabled();
        next();
        if (ImGui::Button(tr("Open Folder"))) openInExplorer(m_projectDir + "\\modules\\" + item.name);
    }
    if (item.repo.rfind("https://", 0) == 0) {
        next();
        if (ImGui::Button(tr("Repository"))) openInExplorer(item.repo);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", item.repo.c_str());
    }

    // Other versions: fetched on request (registry list or the engine module repo's tags)
    if (item.installed || item.registry) {
        ImGui::Spacing();
        if (m_versionsFor != item.name) {
            ImGui::BeginDisabled(m_process.running());
            if (ImGui::SmallButton(tr("Other Versions..."))) {
                m_versionsFor = item.name;
                m_versions.clear();
                run(trf("Versions of %s", item.name.c_str()), "versions " + item.name + " -Tsv", Job::Versions);
            }
            ImGui::EndDisabled();
        } else if (m_versions.empty()) {
            ImGui::TextDisabled("%s", m_process.running() ? tr("Reading versions...") : tr("No other versions"));
        } else {
            m_pickedVersion = std::clamp(m_pickedVersion, 0, static_cast<int>(m_versions.size()) - 1);
            const Version& picked = m_versions[static_cast<size_t>(m_pickedVersion)];
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
            if (ImGui::BeginCombo("##version", picked.version.c_str())) {
                for (int i = 0; i < static_cast<int>(m_versions.size()); ++i) {
                    const Version& v = m_versions[static_cast<size_t>(i)];
                    const std::string label = v.engine == "-" ? v.version : trf("%s (engine %s)", v.version.c_str(), v.engine.c_str());
                    if (!v.fits) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::Selectable(label.c_str(), i == m_pickedVersion)) m_pickedVersion = i;
                    if (!v.fits) ImGui::PopStyleColor();
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            const bool current = item.installed && item.installed->version == picked.version;
            ImGui::BeginDisabled(!canChange || current);
            if (ImGui::SmallButton(item.installed ? tr("Change to This Version") : tr("Add This Version"))) {
                if (item.installed) run(trf("Update %s", item.name.c_str()), "update " + item.name + " -To " + picked.version, Job::Change);
                else add(item, item.name + "@" + picked.version, false);
            }
            ImGui::EndDisabled();
            if (!picked.fits) {
                ImGui::SameLine();
                ImGui::TextColored(color::kWarning, "%s", tr("made for another engine version"));
            }
        }
    }
}

void ModulesPanel::drawPopups() {
    if (!m_confirmRemove.empty()) ImGui::OpenPopup("##remove");
    if (ImGui::BeginPopupModal("##remove", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(trf("Remove modules/%s from the project?", m_confirmRemove.c_str()).c_str());
        ImGui::TextDisabled("%s", tr("It is refused while game code includes it. Undo with version control."));
        if (ImGui::Button(tr("Remove"))) {
            run(trf("Remove %s", m_confirmRemove.c_str()), "remove " + m_confirmRemove, Job::Change);
            m_confirmRemove.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) {
            m_confirmRemove.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (!m_confirmEngine.empty()) ImGui::OpenPopup("##engineFirst");
    if (ImGui::BeginPopupModal("##engineFirst", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(trf("Add %s, the first engine module in modules/?", m_confirmEngine.c_str()).c_str());
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextDisabled("%s", tr("Now the game builds with every engine module of its engine. From the first one in modules/, it builds with only those, so add the others it uses too."));
        ImGui::PopTextWrapPos();
        if (ImGui::Button(tr("Add the Standard Set"))) {
            run(tr("Add the standard engine modules"), "add -Preset standard", Job::Change);
            m_confirmEngine.clear();
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Every engine module but the editor: the game keeps building as now, and you can remove what it doesn't use"));
        ImGui::SameLine();
        if (ImGui::Button(trf("Only %s", m_confirmEngine.c_str()).c_str())) {
            run(trf("Add %s", m_confirmEngine.c_str()), "add " + m_confirmEngine + (m_confirmEngineRequires ? " -WithRequires" : ""), Job::Change);
            m_confirmEngine.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) {
            m_confirmEngine.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (m_confirmUpdateAll) ImGui::OpenPopup("##updateAll");
    if (ImGui::BeginPopupModal("##updateAll", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(trf("Update %d module(s) to the newest version for this engine?", updateCount()).c_str());
        for (const Item& item : m_items)
            if (item.update) ImGui::BulletText("%s  %s -> %s", item.name.c_str(), item.installed->version.c_str(), item.registry->version.c_str());
        ImGui::TextDisabled("%s", tr("Files you changed are kept; the new version is written next to them as .new"));
        const bool busy = m_process.running();
        ImGui::BeginDisabled(busy);
        if (ImGui::Button(tr("Preview"))) run(tr("Preview the updates"), "update -All -DryRun", Job::Preview);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Lists what would change in the log below, without changing anything"));
        ImGui::SameLine();
        if (ImGui::Button(tr("Update All"))) {
            run(tr("Update all modules"), "update -All", Job::Change);
            m_confirmUpdateAll = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"))) {
            m_confirmUpdateAll = false;
            ImGui::CloseCurrentPopup();
        }
        if (busy) ImGui::TextDisabled("%s...", m_label.c_str());
        else if (!m_log.empty() && m_job == Job::Preview) {
            ImGui::InputTextMultiline("##preview", m_log.data(), m_log.size() + 1,
                                      ImVec2(ImGui::GetFontSize() * 36.0f, ImGui::GetTextLineHeightWithSpacing() * 12.0f), ImGuiInputTextFlags_ReadOnly);
        }
        ImGui::EndPopup();
    }
}

void ModulesPanel::draw(const ProjectInfo& project, const std::string& engineDirectory, bool editing, bool* open) {
    if (!*open) return;
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 52.0f, ImGui::GetFontSize() * 32.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(iconTitle(kPuzzle, "Modules###Modules").c_str(), open)) {
        ImGui::End();
        return;
    }
    if (project.directory.empty()) {
        ImGui::TextDisabled("%s", tr("Open a project to see its modules."));
        ImGui::End();
        return;
    }
    m_engineDir = engineDirectory;
    if (m_projectDir != project.directory) {
        m_projectDir = project.directory;
        m_list = {};
        m_items.clear();
        m_details = {};
        m_versionsFor.clear();
        m_log.clear();
        m_selected.clear();
        m_needList = true;
    }
    if (m_needList && !m_process.running()) {
        m_needList = false;
        run(tr("Module list"), "search -Tsv", Job::List);
    }

    const bool busy = m_process.running();
    drawToolbar(busy, editing);

    // List and details; the last change's output under them (its height can be dragged)
    const float logHeight = ImGui::GetTextLineHeightWithSpacing() * 8.0f;
    const bool showLog = !m_log.empty() && !m_confirmUpdateAll;
    const ImVec2 bodySize(0.0f, showLog ? std::max(ImGui::GetContentRegionAvail().y - logHeight, logHeight) : 0.0f);
    if (ImGui::BeginChild(showLog ? "##bodyLog" : "##body", bodySize, showLog ? ImGuiChildFlags_ResizeY : ImGuiChildFlags_None)) {
        if (beginListPane("##moduleList", ImGui::GetFontSize() * 20.0f)) drawList();
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##moduleDetails", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding)) drawDetails(editing && !busy);
        ImGui::EndChild();
    }
    ImGui::EndChild();
    if (showLog) {
        if (m_logFailed) ImGui::TextColored(color::kDanger, "%s", tr("The last change failed:"));
        else ImGui::TextDisabled("%s", tr("Last change"));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Clear"))) m_log.clear();
        if (!m_log.empty())
            ImGui::InputTextMultiline("##log", m_log.data(), m_log.size() + 1, ImVec2(-FLT_MIN, -FLT_MIN), ImGuiInputTextFlags_ReadOnly);
    }
    drawPopups();
    ImGui::End();
}

} // namespace mealkit::editor
