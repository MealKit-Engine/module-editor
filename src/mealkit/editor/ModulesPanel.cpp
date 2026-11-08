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
        size_t end = text.find(' ', start);
        if (end == std::string::npos) end = text.size();
        if (end > start) words.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return words;
}

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

// Icon keywords (docs/MODULES.md "module.txt"): Segoe Fluent Icons / MDL2 Assets code points, like icon:: in ImGuiLayer.h
struct IconKeyword {
    const char* keyword;
    const char* glyph;
};
constexpr IconKeyword kIconKeywords[] = {
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

const char* keywordGlyph(const std::string& word) {
    for (const IconKeyword& k : kIconKeywords)
        if (word == k.keyword) return k.glyph;
    return nullptr;
}

ImU32 textColor(bool disabled) { return ImGui::GetColorU32(disabled ? ImGuiCol_TextDisabled : ImGuiCol_Text); }

} // namespace

const char* moduleGlyph(const std::string& icon, const std::string& tags, const std::string& name) {
    if (const char* g = keywordGlyph(icon)) return g;
    for (const std::string& tag : splitWords(tags))
        if (tag != "engine")
            if (const char* g = keywordGlyph(tag)) return g;
    if (const char* g = keywordGlyph(name)) return g;
    const size_t dash = name.find('-'); // backends: online-steam takes online's
    if (dash != std::string::npos)
        if (const char* g = keywordGlyph(name.substr(0, dash))) return g;
    return kPuzzle;
}

void ModulesPanel::run(const std::string& engineDirectory, const ProjectInfo& project, const std::string& label,
                       const std::string& arguments, bool changes) {
    if (m_process.running()) return;
    const std::string script = engineDirectory + "\\tools\\module.ps1";
    if (!fileExists(script)) {
        m_status = trf("Module tool not found: %s", script.c_str());
        return;
    }
    const std::string command = "powershell -NoProfile -ExecutionPolicy Bypass -File " + quoted(script) + " " + arguments +
                                " -Project " + quoted(project.directory);
    m_output.clear();
    m_label = label;
    m_listing = !changes;
    m_changes = changes;
    if (!m_process.start(command, project.directory)) {
        m_status = trf("Could not start: %s", label.c_str());
        return;
    }
    MEALKIT_INFO("Module tool: %s", arguments.c_str());
}

void ModulesPanel::poll() {
    if (!m_process.running()) return;
    if (m_process.poll(m_output)) return;
    const bool ok = m_process.exitCode() == 0;
    if (m_listing) {
        m_list = parseModuleList(m_output);
        if (!ok) m_list.error = m_output;
        rebuildItems();
        return;
    }
    m_log = m_output;
    m_logFailed = !ok;
    m_status = ok ? trf("%s: done", m_label.c_str()) : trf("%s: failed - see the Modules window", m_label.c_str());
    if (ok && m_changes) m_changed = true;
    m_needList = true; // recheck the current state whether it succeeded or not
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
    if (m_selected.empty() && !m_items.empty()) m_selected = m_items.front().name;
}

bool ModulesPanel::visible(const Item& item) const {
    switch (m_filter) {
    case Filter::All: break;
    case Filter::Installed:
        if (!item.installed) return false;
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
    bool anyEngineInstalled = false;
    for (const Item& item : m_items) anyEngineInstalled |= item.installed && item.engine;

    if (ImGui::CollapsingHeader((trf("In this project (%d)", installedCount) + "###installed").c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
        if (m_list.installed.empty()) ImGui::TextDisabled("%s", tr("No modules. Add one from the list below."));
        for (const Item& item : m_items)
            if (item.installed && visible(item)) drawRow(item);
        if (!anyEngineInstalled) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", tr("No engine modules here: the game builds with every engine module of its engine. Adding one makes it build with only those in modules/."));
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

void ModulesPanel::drawDetails(const ProjectInfo& project, const std::string& engineDirectory, bool canChange) {
    const auto found = std::find_if(m_items.begin(), m_items.end(), [this](const Item& i) { return i.name == m_selected; });
    if (found == m_items.end()) {
        ImGui::TextDisabled("%s", tr("Pick a module on the left."));
        return;
    }
    const Item& item = *found;

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

    // State of the copy in the project
    if (item.installed) {
        ImGui::Spacing();
        const InstalledModule& m = *item.installed;
        if (m.pending > 0) ImGui::TextColored(color::kWarning, "%s", trf("%d .new file(s) to merge", m.pending).c_str());
        else if (m.modified > 0) ImGui::TextUnformatted(trf("%d file(s) changed here", m.modified).c_str());
        else ImGui::TextDisabled("%s", tr("No files changed here"));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", trf("From %s", m.source.c_str()).c_str());
        ImGui::PopTextWrapPos();
    }

    // Actions
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::BeginDisabled(!canChange);
    if (item.installed) {
        if (ImGui::Button(tr("Update")))
            run(engineDirectory, project, trf("Update %s", item.name.c_str()), "update " + item.name, true);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", tr("Files you changed are kept; the new version is written next to them as .new"));
        ImGui::SameLine();
        if (ImGui::Button(tr("Remove"))) m_confirmRemove = item.name;
    } else if (item.registry && !item.registry->version.empty()) {
        if (ImGui::Button((std::string(icon::kAdd) + "  " + tr("Add")).c_str()))
            run(engineDirectory, project, trf("Add %s", item.name.c_str()), "add " + item.name, true);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", item.engine ? tr("Copies the engine's module into modules/") : tr("Module code runs inside the editor and the game. The registry checks that it builds, not what it does - read what you add."));
        ImGui::SameLine();
        if (ImGui::Button(tr("Add with Required")))
            run(engineDirectory, project, trf("Add %s", item.name.c_str()), "add " + item.name + " -WithRequires", true);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", tr("Also adds the modules its requires line lists"));
    }
    ImGui::EndDisabled();
    if (item.installed) {
        const std::string docs = project.directory + "\\modules\\" + item.name + "\\docs";
        const bool haveDocs = fileExists(docs + "\\index.md");
        ImGui::SameLine();
        ImGui::BeginDisabled(!haveDocs);
        if (ImGui::Button(tr("Docs"))) openInExplorer(docs + "\\index.md");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Open Folder"))) openInExplorer(project.directory + "\\modules\\" + item.name);
    }
    if (item.repo.rfind("https://", 0) == 0) {
        if (item.installed || (item.registry && !item.registry->version.empty())) ImGui::SameLine();
        if (ImGui::Button(tr("Repository"))) openInExplorer(item.repo);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", item.repo.c_str());
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
    if (m_projectDir != project.directory) {
        m_projectDir = project.directory;
        m_list = {};
        m_items.clear();
        m_log.clear();
        m_selected.clear();
        m_needList = true;
    }
    if (m_needList && !m_process.running()) {
        m_needList = false;
        run(engineDirectory, project, tr("Module list"), "search -Tsv", false);
    }

    const bool busy = m_process.running();
    drawToolbar(busy, editing);

    // List and details; the last change's output under them (its height can be dragged)
    const float logHeight = ImGui::GetTextLineHeightWithSpacing() * 8.0f;
    const bool showLog = !m_log.empty();
    const ImVec2 bodySize(0.0f, showLog ? std::max(ImGui::GetContentRegionAvail().y - logHeight, logHeight) : 0.0f);
    if (ImGui::BeginChild(showLog ? "##bodyLog" : "##body", bodySize, showLog ? ImGuiChildFlags_ResizeY : ImGuiChildFlags_None)) {
        if (beginListPane("##moduleList", ImGui::GetFontSize() * 20.0f)) drawList();
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##moduleDetails", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding))
            drawDetails(project, engineDirectory, editing && !busy);
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

    if (!m_confirmRemove.empty()) ImGui::OpenPopup("##remove");
    if (ImGui::BeginPopupModal("##remove", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(trf("Remove modules/%s from the project?", m_confirmRemove.c_str()).c_str());
        ImGui::TextDisabled("%s", tr("It is refused while game code includes it. Undo with version control."));
        if (ImGui::Button(tr("Remove"))) {
            run(engineDirectory, project, trf("Remove %s", m_confirmRemove.c_str()), "remove " + m_confirmRemove, true);
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
    ImGui::End();
}

} // namespace mealkit::editor
