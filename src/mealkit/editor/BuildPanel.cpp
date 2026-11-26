#include "mealkit/editor/BuildPanel.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/CodeLink.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <vector>

namespace mealkit::editor {

bool parseBuildDiagnostic(const std::string& line, BuildDiagnostic* out) {
    FileLine at;
    if (!parseFileLine(line, &at)) return false;
    // The keyword follows the location: ": error", ": warning", ": fatal error" (Korean MSVC: 오류/경고)
    struct Key { const char* text; bool error; };
    static const Key keys[] = {{": fatal error", true}, {": error", true}, {": warning", false},
                               {": 오류", true}, {": 경고", false}}; // i18n-ignore (Korean MSVC output)
    for (const Key& key : keys) {
        const size_t pos = line.find(key.text);
        if (pos == std::string::npos) continue;
        const size_t colon = line.find(": ", pos + 2); // after "error C2065" / "error"
        out->error = key.error;
        out->file = at.file;
        out->line = at.line;
        out->message = colon == std::string::npos ? line.substr(pos + 2) : line.substr(colon + 2);
        out->count = 1;
        return true;
    }
    return false;
}

void BuildPanel::addOutputLine(const std::string& line) {
    m_log += line;
    BuildDiagnostic d;
    std::string text = line;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    if (!parseBuildDiagnostic(text, &d)) return;
    for (BuildDiagnostic& other : m_diagnostics) {
        if (other.error == d.error && other.line == d.line && other.file == d.file && other.message == d.message) {
            ++other.count;
            return;
        }
    }
    (d.error ? m_errors : m_warnings) += 1;
    m_diagnostics.push_back(std::move(d));
}

void BuildPanel::cancel() {
    if (!m_process.running()) return;
    m_cancelled = true;
    m_process.terminate();
}

namespace {

#ifdef _WIN32
constexpr const char* kExeSuffix = ".exe";
#else
constexpr const char* kExeSuffix = "";
#endif

const ImVec4 kBusyColor(0.95f, 0.80f, 0.35f, 1.0f);
const ImVec4 kGoodColor(0.45f, 0.85f, 0.5f, 1.0f);
const ImVec4 kBadColor(1.0f, 0.45f, 0.4f, 1.0f);

std::string clock(f64 seconds) {
    const int total = static_cast<int>(seconds);
    char text[24];
    std::snprintf(text, sizeof(text), "%d:%02d", total / 60, total % 60);
    return text;
}

std::string sizeText(unsigned long long bytes) {
    char text[32];
    if (bytes >= 1024ull * 1024ull) std::snprintf(text, sizeof(text), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    else std::snprintf(text, sizeof(text), "%.0f KB", static_cast<double>(bytes) / 1024.0);
    return text;
}

// "11-24 21:03" of a file's last write
std::string fileTimeText(const std::filesystem::path& path) {
    std::error_code ec;
    const auto written = std::filesystem::last_write_time(path, ec);
    if (ec) return {};
    const auto system = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        written - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t t = std::chrono::system_clock::to_time_t(system);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%m-%d %H:%M", &local);
    return text;
}

// The build script's configurations a profile can use, with what they make
struct ConfigChoice { const char* config; const char* what; };
const ConfigChoice kConfigs[] = {
    {"Export", "Shipping build: exe + packed assets in the output folder"},
    {"VulkanExport", "Shipping build with the Vulkan renderer (Windows)"},
    {"Release", "Development build (build/<os>/Release)"},
    {"Profile", "Release with Tracy profiling"},
    {"Debug", "Debug build"},
    {"Vulkan", "Development build with the Vulkan renderer"},
    {"WebGPUExport", "Shipping build with the WebGPU renderer (Linux)"},
    {"WebGPU", "Development build with the WebGPU renderer (Linux)"},
    {"WebExport", "Browser build: a page with WebGPU, built on Linux with Emscripten"},
};

} // namespace

// Runs the project's build.bat without a window.
//   GameCode: build.bat Editor → build/<os>/Editor/bin/<name>_game.dll (the engine mealkit.dll isn't touched if already built)
//   Export:   build.bat Export → rebuilds only the shipping exe and gathers it in export/<os>/<name>
//   Profile:  build.bat profile:<id> → the profile's configuration (and export folder)
bool BuildPanel::start(BuildKind kind, const ProjectInfo& project, const std::string& engineDirectory, f64 now, bool run) {
    if (m_process.running() || project.directory.empty()) return false;
    if (!m_profilesLoaded) loadProfiles(project);
    std::string command;
    const BuildProfile* profile = nullptr;
    if (kind == BuildKind::Profile) {
        profile = findProfile(m_requestId.empty() ? m_activeId : m_requestId);
        m_requestId.clear();
        if (!profile && !m_profiles.empty()) {
            m_status = tr("Pick a build profile first");
            return false;
        }
        if (!profile) kind = BuildKind::Export; // no profiles: the plain release build
        else if (!profile->buildsHere()) {
            m_status = trf("%s builds on %s only (this PC: %s)", profile->title().c_str(), profile->platform.c_str(), buildOsName());
            return false;
        }
    }
    if (kind == BuildKind::Upgrade) {
        const std::string script = engineDirectory + MEALKIT_SEP "tools" MEALKIT_SEP "upgrade_project.ps1";
        if (!fileExists(script)) {
            m_status = trf("Upgrade script not found: %s", script.c_str());
            return false;
        }
        command = powershellCommand(script) + " -Project \"" + project.directory + "\"";
        m_log = "> upgrade_project.ps1\n";
    } else {
        const std::string script = buildScriptPath(project.buildRoot());
        if (!fileExists(script)) {
            m_status = trf("build.bat not found: %s", script.c_str());
            return false;
        }
        std::string arg = kind == BuildKind::GameCode ? editorBuildName() : std::string("Export");
        if (profile) {
            arg = "profile:" + profile->id;
            m_runningProfile = profile->title();
        }
        command = buildCommand(project.buildRoot(), arg);
        m_log = "> " + fileName(script) + " " + arg + "\n";
    }
    m_kind = kind;
    m_buildingId = profile ? profile->id : std::string();
    m_runAfter = run && (kind == BuildKind::Profile || kind == BuildKind::Export);
    m_webOutput = profile && profile->isWeb() ? outputFolder(project, profile) : std::string();
    m_runPath = m_runAfter ? runnablePath(project, profile) : std::string();
    m_projectDirectory = project.directory;
    m_cancelled = false;
    m_diagnostics.clear();
    m_errors = m_warnings = 0;
    m_view = 2; // the full log while it runs; the error list once it fails
    m_pickView = true;
    m_title = kind == BuildKind::GameCode  ? tr("Game code build")
              : kind == BuildKind::Upgrade ? tr("Project upgrade")
              : kind == BuildKind::Profile ? m_runningProfile
                                           : tr("Release build");
    m_step = m_steps = 0;
    m_scroll = true;
    if (kind != BuildKind::GameCode) m_open = true; // game code builds open only on failure
    m_profilesLoaded = false; // a build may be started right after a profile file was edited by hand
    if (!m_process.start(command, project.buildRoot())) {
        m_log += std::string(tr("Could not start the build.")) + "\n";
        m_open = true;
        return false;
    }
    m_start = now;
    MEALKIT_INFO("Build started: %s", command.c_str());
    return true;
}

bool BuildPanel::poll(const ProjectInfo& project, f64 now) {
    if (!m_process.running()) return false;
    // For a failed command ninja shows the /showIncludes lines as is. Those lines are dropped to make errors easier to find (English/Korean MSVC).
    std::string fresh;
    const bool running = m_process.poll(fresh);
    size_t start = 0;
    for (size_t nl = fresh.find('\n'); nl != std::string::npos; nl = fresh.find('\n', start)) {
        const std::string line = fresh.substr(start, nl + 1 - start);
        start = nl + 1;
        if (line.find("Note: including file:") != std::string::npos ||
            line.find("참고: 포함 파일:") != std::string::npos) // i18n-ignore (Korean MSVC output)
            continue;
        // ninja's progress "[n/N] ..." (the engine editor build first, then the game: N restarts with each)
        int n = 0, total = 0;
        if (line.size() > 4 && line[0] == '[' && std::sscanf(line.c_str(), "[%d/%d]", &n, &total) == 2 && total > 0) {
            m_step = n;
            m_steps = total;
        }
        addOutputLine(line);
        m_scroll = true;
    }
    if (running) return false;
    m_scroll = true;
    m_succeeded = m_process.exitCode() == 0 && !m_cancelled;
    m_elapsed = now - m_start;
    m_outputInfo.checked = -100.0;
    if (m_kind == BuildKind::Profile || m_kind == BuildKind::Export) { // the [Build Profiles] window shows it under the profile
        LastBuild last{m_buildingId, m_succeeded, m_cancelled, m_elapsed, m_errors};
        bool found = false;
        for (LastBuild& other : m_lastBuilds)
            if (other.id == last.id) other = last, found = true;
        if (!found) m_lastBuilds.push_back(last);
    }
    const std::string seconds = std::to_string(static_cast<int>(now - m_start));
    if (m_cancelled) {
        m_log += "\n" + std::string(tr("Cancelled")) + "\n";
        m_status = tr("Build cancelled");
        MEALKIT_INFO("Build cancelled");
    } else if (!m_succeeded) {
        m_view = m_errors > 0 ? 0 : 2;
        m_log += "\n" + trf("Failed (exit code %d)", m_process.exitCode()) + "\n";
        m_open = true;
        m_status = m_kind == BuildKind::GameCode  ? tr("Game code build failed - see the build log")
                   : m_kind == BuildKind::Upgrade ? tr("Upgrade failed - see the build log")
                   : m_kind == BuildKind::Profile ? trf("Build profile %s failed - see the build log", m_runningProfile.c_str())
                                                  : tr("Release build failed - see the build log");
        MEALKIT_ERROR("Build failed with exit code %d", m_process.exitCode());
    } else if (m_kind == BuildKind::Upgrade) {
        m_log += std::string("\n") + tr("Upgrade done - rebuilding the game code") + "\n";
    } else if (m_kind == BuildKind::Profile) {
        m_log += "\n" + trf("Done (%ss): %s", seconds.c_str(), m_runningProfile.c_str()) + "\n";
        m_status = trf("Build profile done: %s", m_runningProfile.c_str());
        MEALKIT_INFO("Build profile finished: %s", m_runningProfile.c_str());
        m_open = m_runAfter ? false : m_open;
    } else if (m_kind == BuildKind::GameCode) {
        m_log += "\n" + trf("Done (%ss): game code", seconds.c_str()) + "\n";
        MEALKIT_INFO("Game code build finished");
    } else {
        m_log += "\n" + trf("Done (%ss): %s", seconds.c_str(), project.exportDirectory().c_str()) + "\n";
        m_status = trf("Release build done: %s", ("export\\" + project.name()).c_str());
        MEALKIT_INFO("Export finished: %s", project.exportDirectory().c_str());
    }
    // A browser build has no exe: the page is played from a web server (WebGPU needs https or localhost)
    if (m_succeeded && !m_webOutput.empty()) {
        m_status = trf("Built a web page in %s: serve that folder (https or http://localhost) and open it in a WebGPU browser",
                       m_webOutput.c_str());
        m_runAfter = false;
    }
    // Build and Run: the exe the build made, started from its folder
    if (m_succeeded && m_runAfter) {
        const std::string folder = m_runPath.substr(0, m_runPath.find_last_of('/'));
        if (!fileExists(m_runPath)) m_status = trf("Built, but there is no %s to run", m_runPath.c_str());
        else if (!launchDetached(m_runPath, "", folder)) m_status = trf("Could not start %s", m_runPath.c_str());
        else m_status = trf("Started %s", fileName(m_runPath).c_str());
    }
    m_runAfter = false;
    return true;
}

void BuildPanel::loadProfiles(const ProjectInfo& project) {
    m_profiles = project.directory.empty() ? std::vector<BuildProfile>() : loadBuildProfiles(project.directory);
    m_profilesLoaded = true;
    // An active id that is gone (or never set) falls to the first profile that builds on this OS
    if (!findProfile(m_activeId)) {
        m_activeId.clear();
        for (const BuildProfile& profile : m_profiles)
            if (m_activeId.empty() && profile.buildsHere()) m_activeId = profile.id;
        if (m_activeId.empty() && !m_profiles.empty()) m_activeId = m_profiles.front().id;
    }
    if (!findProfile(m_viewId)) m_viewId = m_activeId;
    m_outputInfo.checked = -100.0;
}

const BuildProfile* BuildPanel::findProfile(const std::string& id) const {
    for (const BuildProfile& profile : m_profiles) {
        if (profile.id == id) return &profile;
    }
    return nullptr;
}

const BuildProfile* BuildPanel::activeProfile() const { return findProfile(m_activeId); }

std::string BuildPanel::activeTitle() const {
    const BuildProfile* profile = activeProfile();
    return profile ? profile->title() : std::string(tr("Release"));
}

std::string BuildPanel::outputFolder(const ProjectInfo& project, const BuildProfile* profile) const {
    if (!profile) return project.exportDirectory();
    if (profile->isExport()) return buildProfileOutputDirectory(project.directory, project.name(), *profile);
    return buildTreeDirectory(project.buildRoot(), profile->config) + "/bin"; // a development configuration runs from its build tree
}

std::string BuildPanel::runnablePath(const ProjectInfo& project, const BuildProfile* profile) const {
    return outputFolder(project, profile) + "/" + project.name() + kExeSuffix;
}

std::string BuildPanel::releaseFolder(const ProjectInfo& project) const { return outputFolder(project, activeProfile()); }

bool BuildPanel::drawStatus(f64 now) const {
    if (!m_process.running()) return false;
    ImGui::TextColored(kBusyColor,
                       m_kind == BuildKind::GameCode  ? tr("Building game code... %.0fs")
                       : m_kind == BuildKind::Upgrade ? tr("Upgrading project... %.0fs")
                       : m_kind == BuildKind::Profile ? tr("Profile build... %.0fs")
                                                      : tr("Release build... %.0fs"),
                       now - m_start);
    if (m_steps > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%d/%d", m_step, m_steps);
    }
    return true;
}

// The game's shipping settings: one set in mealkit.project for every profile (Unity Player Settings, Godot's application settings)
void BuildPanel::drawGameSection(bool canEdit, const ProjectInfo& project, BuildWindowRequest* out) {
    game::ProjectSettings edit = project.settings;
    bool changed = false;
    ImGui::SeparatorText(tr("Game"));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Project Settings..."))) out->openProjectSettings = true;
    if (ImGui::BeginTable("##game", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7.0f);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        const auto label = [](const char* text, const char* help) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            if (help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", help);
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
        };
        char buffer[256];
        const auto text = [&](const char* id, std::string& value, const char* hint) {
            std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
            ImGui::InputTextWithHint(id, hint, buffer, sizeof(buffer));
            if (ImGui::IsItemDeactivatedAfterEdit() && value != buffer) {
                value = buffer;
                changed = true;
            }
        };
        label(tr("Name"), tr("The exe and folder name. Set when the project was made"));
        ImGui::BeginDisabled();
        std::snprintf(buffer, sizeof(buffer), "%s", edit.name.c_str());
        ImGui::InputText("##name", buffer, sizeof(buffer), ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!canEdit);
        label(tr("Version"), tr("Game version (shown to players, compared when joining)"));
        text("##version", edit.version, "0.1.0");
        label(tr("Icon"), tr("Game icon (PNG, project folder)"));
        const std::string iconPath = project.directory + "/" + (edit.icon.empty() ? std::string("icon.png") : edit.icon);
        const bool iconFound = fileExists(iconPath);
        ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 1.8f);
        text("##icon", edit.icon, "icon.png");
        ImGui::SameLine();
        ImGui::TextColored(iconFound ? kGoodColor : kBusyColor, "%s", iconFound ? icon::kAccept : icon::kWarning);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", iconFound ? iconPath.c_str() : trf("%s not found: the exe gets the MealKit icon", iconPath.c_str()).c_str());
        // Scenes the cook starts from: the start scene always, the checked ones besides
        label(tr("Scenes"), tr("The cook packs these scenes and what they reference. The start scene is always in"));
        if (ImGui::BeginChild("##scenes", ImVec2(-FLT_MIN, ImGui::GetFrameHeightWithSpacing() * std::min<f32>(3.5f, static_cast<f32>(std::max<size_t>(m_scenes.size(), 1)) + 0.5f)),
                              ImGuiChildFlags_Borders)) {
            std::vector<std::string> shipped;
            for (size_t start = 0; start < edit.scenes.size();) {
                size_t end = edit.scenes.find(' ', start);
                if (end == std::string::npos) end = edit.scenes.size();
                if (end > start) shipped.push_back(edit.scenes.substr(start, end - start));
                start = end + 1;
            }
            if (m_scenes.empty()) ImGui::TextDisabled("%s", tr("No scenes under assets"));
            for (const std::string& scene : m_scenes) {
                const bool isStart = scene == edit.scene;
                bool on = isStart || std::find(shipped.begin(), shipped.end(), scene) != shipped.end();
                ImGui::BeginDisabled(isStart);
                if (ImGui::Checkbox(scene.c_str(), &on)) {
                    if (on) shipped.push_back(scene);
                    else shipped.erase(std::remove(shipped.begin(), shipped.end(), scene), shipped.end());
                    edit.scenes.clear();
                    for (const std::string& s : shipped) edit.scenes += (edit.scenes.empty() ? "" : " ") + s;
                    changed = true;
                }
                ImGui::EndDisabled();
                if (isStart) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", tr("start scene"));
                }
            }
        }
        ImGui::EndChild();
        label(tr("Folder name"), nullptr);
        if (ImGui::Checkbox(tr("Add the version (export/<os>/<name>-<version>)"), &edit.exportVersionFolder)) changed = true;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Only for profiles without an output folder of their own"));
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
    if (changed) {
        out->projectChanged = true;
        out->settings = edit;
    }
}

// Under the profile: running, how its last build in this session ended, or what its output folder holds
void BuildPanel::drawBuildState(const ProjectInfo& project, const BuildProfile* profile, f64 now) {
    const std::string id = profile ? profile->id : std::string();
    const bool thisOne = (m_kind == BuildKind::Profile || m_kind == BuildKind::Export) && m_buildingId == id && !m_title.empty();
    if (thisOne && m_process.running()) {
        const f32 fraction = m_steps > 0 ? static_cast<f32>(m_step) / static_cast<f32>(m_steps) : -static_cast<f32>(now);
        const std::string overlay = (m_steps > 0 ? std::to_string(m_step) + "/" + std::to_string(m_steps) + "  " : std::string()) + clock(now - m_start);
        ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.0f), overlay.c_str());
        return;
    }
    for (const LastBuild& last : m_lastBuilds) {
        if (last.id != id) continue;
        if (last.cancelled) ImGui::TextDisabled("%s", tr("Last build: cancelled"));
        else if (last.succeeded) ImGui::TextColored(kGoodColor, "%s", trf("Last build: succeeded, %.0fs", last.elapsed).c_str());
        else ImGui::TextColored(kBadColor, "%s", trf("Last build: failed, %.0fs, %d errors", last.elapsed, last.errors).c_str());
        break;
    }
    // Output folder: refreshed every 3 s while shown (a build from a shell changes it too)
    const std::string folder = outputFolder(project, profile);
    if (m_outputInfo.folder != folder || now - m_outputInfo.checked > 3.0) {
        m_outputInfo = OutputInfo();
        m_outputInfo.folder = folder;
        m_outputInfo.checked = now;
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path exe = fs::path(runnablePath(project, profile));
        if (fs::exists(exe, ec)) {
            m_outputInfo.when = fileTimeText(exe);
            if (!profile || profile->isExport()) { // a shipping folder is small: its whole size is the download size
                for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
                    if (it->is_regular_file(ec)) m_outputInfo.bytes += it->file_size(ec);
            } else {
                m_outputInfo.bytes = fs::file_size(exe, ec);
            }
        }
    }
    const std::string shown = folder.rfind(project.directory + "/", 0) == 0 ? folder.substr(project.directory.size() + 1) : std::string();
    if (m_outputInfo.when.empty()) {
        ImGui::TextDisabled("%s", trf("Not built yet: %s", (shown.empty() ? folder : shown).c_str()).c_str());
    } else {
        ImGui::TextDisabled("%s", trf("%s  ·  %s  ·  %s", (shown.empty() ? folder : shown).c_str(), m_outputInfo.when.c_str(), sizeText(m_outputInfo.bytes).c_str()).c_str());
    }
}

bool BuildPanel::drawSettings(bool* open, bool editing, const ProjectInfo& project, f64 now, BuildWindowRequest* out) {
    const f32 font = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(font * 46.0f, font * 30.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin((std::string(icon::kBuild) + "  " + tr("Build Profiles") + "###BuildProfiles").c_str(), open)) {
        ImGui::End();
        return false;
    }
    if (!m_profilesLoaded && !project.directory.empty()) loadProfiles(project);
    if (now - m_scenesChecked > 5.0 && !project.directory.empty()) { // scenes for the shipped list
        m_scenesChecked = now;
        m_scenes.clear();
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path assets = fs::path(project.directory) / "assets";
        for (fs::recursive_directory_iterator it(assets, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".scene") m_scenes.push_back(it->path().lexically_relative(assets).generic_string());
        std::sort(m_scenes.begin(), m_scenes.end());
    }
    const bool canEdit = editing && !project.directory.empty();
    const std::string dir = buildProfileDirectory(project.directory);

    // Left: the profiles. The star is the active one (Ctrl+B); a profile for another OS is dimmed
    if (beginListPane("##profiles", font * 13.0f)) {
        ImGui::TextDisabled("%s", tr("Profiles"));
        if (m_profiles.empty()) { // the plain release build stands in until there are profiles
            ImGui::Selectable((std::string(icon::kStarFilled) + "  " + tr("Release") + "###release").c_str(), true);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("No build profiles: Ctrl+B builds Export into export/<os>/<name>"));
        }
        for (const BuildProfile& profile : m_profiles) {
            const bool active = profile.id == m_activeId;
            if (!profile.buildsHere()) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            const std::string label = std::string(active ? icon::kStarFilled : "   ") + "  " + profile.title() + "###" + profile.id;
            if (ImGui::Selectable(label.c_str(), profile.id == m_viewId, ImGuiSelectableFlags_AllowDoubleClick)) {
                m_viewId = profile.id;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && profile.buildsHere()) m_activeId = profile.id;
            }
            if (!profile.buildsHere()) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", trf("Builds on %s only (this PC: %s)", profile.platform.c_str(), buildOsName()).c_str());
            } else if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", active ? tr("Active profile: Ctrl+B and Build and Run build it") : tr("Double-click to make it the active profile"));
            }
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(!canEdit);
        if (iconButton(icon::kAdd, tr("New profile"))) {
            BuildProfile fresh;
            fresh.id = "profile";
            for (int n = 2; fileExists(dir + "/" + fresh.id + ".profile"); ++n) fresh.id = "profile" + std::to_string(n);
            fresh.config = "Export";
            fresh.platform = buildOsName();
            if (saveBuildProfile(project.directory, fresh)) {
                loadProfiles(project);
                m_viewId = fresh.id;
            }
        }
        ImGui::SameLine();
        const BuildProfile* viewed = findProfile(m_viewId);
        ImGui::BeginDisabled(!viewed);
        if (iconButton(icon::kCopy, tr("Duplicate profile")) && viewed) {
            BuildProfile copy = *viewed;
            copy.id = viewed->id + "_copy";
            for (int n = 2; fileExists(dir + "/" + copy.id + ".profile"); ++n) copy.id = viewed->id + "_copy" + std::to_string(n);
            if (!copy.label.empty()) copy.label += " (copy)";
            if (saveBuildProfile(project.directory, copy)) {
                loadProfiles(project);
                m_viewId = copy.id;
            }
        }
        ImGui::SameLine();
        if (iconButton(icon::kTrash, tr("Delete profile")) && viewed) {
            removeFile(dir + "/" + viewed->id + ".profile");
            m_viewId.clear();
            loadProfiles(project);
        }
        ImGui::EndDisabled();
        if (m_profiles.empty()) {
            ImGui::Spacing();
            if (ImGui::Button(tr("Add Default Profiles"))) {
                for (const BuildProfile& profile : defaultBuildProfiles()) {
                    if (!fileExists(dir + "/" + profile.id + ".profile")) saveBuildProfile(project.directory, profile);
                }
                loadProfiles(project);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", tr("Writes profiles/*.profile: Windows DX12, Windows Vulkan, Linux Vulkan, Linux WebGPU, Web and Debug"));
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Right: game settings, the shown profile, its build state and the buttons (kept at the bottom)
    const f32 footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    ImGui::BeginGroup();
    const BuildProfile* current = findProfile(m_viewId);
    if (ImGui::BeginChild("##profileBody", ImVec2(0.0f, -footer))) {
        if (current) {
            ImGui::SeparatorText(trf("Profile: %s", current->title().c_str()).c_str());
            BuildProfile edited = *current;
            bool changed = false;
            ImGui::BeginDisabled(!canEdit);
            if (ImGui::BeginTable("##profile", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed, font * 7.0f);
                ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
                const auto label = [](const char* text) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(text);
                    ImGui::TableNextColumn();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                };
                char buffer[160];
                const auto textRow = [&](const char* name, std::string& value, const char* hint) {
                    label(name);
                    std::snprintf(buffer, sizeof(buffer), "%s", value.c_str());
                    ImGui::PushID(name);
                    ImGui::InputTextWithHint("##v", hint, buffer, sizeof(buffer));
                    if (ImGui::IsItemDeactivatedAfterEdit() && value != buffer) {
                        value = buffer;
                        changed = true;
                    }
                    ImGui::PopID();
                };
                textRow(tr("Name"), edited.label, current->id.c_str());
                label(tr("Platform"));
                const char* platformName = edited.platform == "windows" ? "Windows" : edited.platform == "linux" ? "Linux" : tr("Any");
                if (beginCombo("##platform", platformName)) {
                    const std::pair<const char*, const char*> platforms[] = {{"", tr("Any")}, {"windows", "Windows"}, {"linux", "Linux"}};
                    for (const auto& [value, name] : platforms) {
                        if (ImGui::Selectable(name, edited.platform == value) && edited.platform != value) {
                            edited.platform = value;
                            changed = true;
                        }
                    }
                    ImGui::EndCombo();
                }
                if (!edited.buildsHere()) {
                    ImGui::SameLine();
                    ImGui::TextColored(kBusyColor, "%s", icon::kWarning);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", trf("Builds on %s only (this PC: %s)", edited.platform.c_str(), buildOsName()).c_str());
                }
                label(tr("Configuration"));
                if (beginCombo("##config", edited.config.c_str())) {
                    for (const ConfigChoice& choice : kConfigs) {
                        if (ImGui::Selectable(choice.config, edited.config == choice.config) && edited.config != choice.config) {
                            edited.config = choice.config;
                            changed = true;
                        }
                        ImGui::SameLine(font * 8.0f);
                        ImGui::TextDisabled("%s", tr(choice.what));
                    }
                    ImGui::EndCombo();
                }
                if (edited.isExport()) textRow(tr("Output folder"), edited.output, tr("export/<os>/<name>"));
                ImGui::EndTable();
            }
            ImGui::EndDisabled();
            ImGui::TextDisabled("profiles/%s.profile", current->id.c_str());
            if (changed) {
                const std::string id = current->id;
                if (saveBuildProfile(project.directory, edited)) {
                    loadProfiles(project);
                    m_viewId = id;
                }
                current = findProfile(id);
            }
        } else if (m_profiles.empty()) {
            ImGui::SeparatorText(tr("Release"));
            ImGui::TextWrapped("%s", tr("Without profiles, a build is Export into export/<os>/<name>. Add profiles for other platforms, renderers or folders."));
        }
        drawGameSection(canEdit, project, out);
    }
    ImGui::EndChild();

    // Footer: build state line, then [Open folder] ... [Set Active] [Build] [Build and Run]
    ImGui::Separator();
    drawBuildState(project, current, now);
    const std::string folder = outputFolder(project, current);
    ImGui::BeginDisabled(!directoryExists(folder));
    if (iconButton(icon::kFolderOpen, tr("Open Release Folder"))) openInExplorer(folder);
    ImGui::EndDisabled();
    const bool buildsHere = !current || current->buildsHere();
    const char* runLabel = tr("Build and Run");
    // Buttons at the right edge: their width is measured on the previous frame (styles pad buttons differently)
    ImGui::SameLine();
    const f32 lineStart = ImGui::GetCursorPosX();
    const f32 right = lineStart + ImGui::GetContentRegionAvail().x;
    if (m_footerWidth > 0.0f && right - m_footerWidth > lineStart) ImGui::SetCursorPosX(right - m_footerWidth);
    const f32 buttonsStart = ImGui::GetCursorScreenPos().x;
    if (current) {
        ImGui::BeginDisabled(current->id == m_activeId || !buildsHere);
        if (ImGui::Button(tr("Set Active"))) m_activeId = current->id;
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (m_process.running()) {
        if (ImGui::Button(tr("Cancel Build"))) cancel();
    } else {
        ImGui::BeginDisabled(!canEdit || !buildsHere);
        if (ImGui::Button(tr("Build"))) {
            m_requestId = current ? current->id : std::string();
            out->build = true;
        }
        ImGui::SameLine();
        if (ImGui::Button(runLabel)) {
            m_requestId = current ? current->id : std::string();
            out->build = out->run = true;
        }
        ImGui::EndDisabled();
        if (!buildsHere && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("%s", trf("Builds on %s only (this PC: %s)", current->platform.c_str(), buildOsName()).c_str());
    }
    m_footerWidth = ImGui::GetItemRectMax().x - buttonsStart;
    ImGui::EndGroup();
    ImGui::End();
    return out->build || out->projectChanged || out->openProjectSettings;
}

bool BuildPanel::drawContent(bool editing, const ProjectInfo& project, f64 now, BuildKind* request) {
    bool requested = false;
    if (m_title.empty()) { // nothing built in this session yet
        ImGui::TextDisabled("%s", tr("Nothing built yet."));
        ImGui::BeginDisabled(m_process.running() || project.directory.empty() || !editing);
        if (iconButton(icon::kBuild, tr("Build Game Code"))) {
            *request = BuildKind::GameCode;
            requested = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Build Profiles..."))) m_wantSettings = true;
        return requested;
    }

    // Result row: what was built, how it ended, error/warning counts, [Retry] / [Cancel]
    {
        const bool running = m_process.running();
        const ImVec4 stateColor = running ? kBusyColor : m_cancelled ? ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled)
                                  : m_succeeded ? kGoodColor : kBadColor;
        const char* state = running ? tr("Building") : m_cancelled ? tr("Cancelled") : m_succeeded ? tr("Succeeded") : tr("Failed");
        ImGui::TextUnformatted(m_title.c_str());
        ImGui::SameLine();
        ImGui::TextColored(stateColor, "%s", state);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", clock(running ? now - m_start : m_elapsed).c_str());
        if (running && m_steps > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%d/%d", m_step, m_steps);
        }
        ImGui::SameLine();
        ImGui::TextColored(m_errors > 0 ? kBadColor : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s %d", icon::kError, m_errors);
        ImGui::SameLine();
        ImGui::TextColored(m_warnings > 0 ? kBusyColor : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled), "%s %d", icon::kWarning, m_warnings);
        ImGui::SameLine();
        if (running) {
            if (ImGui::Button(tr("Cancel"))) cancel();
        } else {
            ImGui::BeginDisabled(!editing || project.directory.empty() || m_kind == BuildKind::Upgrade);
            if (ImGui::Button(tr("Retry"))) {
                if (m_kind == BuildKind::Profile) m_requestId = m_buildingId; // the same profile, even if another became active
                *request = m_kind;
                requested = true;
            }
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        if (iconButton(icon::kSettings, tr("Build Profiles..."))) m_wantSettings = true;
        if (m_kind == BuildKind::Profile || m_kind == BuildKind::Export) {
            const std::string folder = outputFolder(project, findProfile(m_buildingId));
            ImGui::SameLine();
            ImGui::BeginDisabled(running || !directoryExists(folder));
            if (iconButton(icon::kFolderOpen, tr("Open Release Folder"))) openInExplorer(folder);
            ImGui::EndDisabled();
        }
        // View: error list (default after a failure), warnings, full log
        if (m_pickView) m_pickView = false;
        char label[48];
        std::snprintf(label, sizeof(label), tr("Errors (%d)"), m_errors);
        if (radioButton(label, m_view == 0)) m_view = 0;
        ImGui::SameLine();
        std::snprintf(label, sizeof(label), tr("Warnings (%d)"), m_warnings);
        if (radioButton(label, m_view == 1)) m_view = 1;
        ImGui::SameLine();
        if (radioButton(tr("Full Log"), m_view == 2)) m_view = 2;
    }

    if (ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
        const auto openAt = [&](const std::string& file, int line) {
            const bool absolute = isAbsolutePath(file) || (!file.empty() && file[0] == '\\');
            openInCodeEditor(m_codeEditor, absolute ? file : project.directory + MEALKIT_SEP + file, line);
        };
        if (m_view != 2 && !m_title.empty()) {
            const bool wantErrors = m_view == 0;
            int shown = 0;
            for (size_t i = 0; i < m_diagnostics.size(); ++i) {
                const BuildDiagnostic& d = m_diagnostics[i];
                if (d.error != wantErrors) continue;
                ++shown;
                ImGui::PushID(static_cast<int>(i));
                ImGui::PushStyleColor(ImGuiCol_Text, d.error ? ImVec4(1.0f, 0.5f, 0.45f, 1.0f) : ImVec4(0.95f, 0.80f, 0.35f, 1.0f));
                std::string text = std::string(d.error ? icon::kError : icon::kWarning) + "  " + fileName(d.file) + "(" + std::to_string(d.line) + ")  " + d.message;
                if (d.count > 1) text += "  x" + std::to_string(d.count);
                if (ImGui::Selectable(text.c_str())) openAt(d.file, d.line);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s:%d", d.file.c_str(), d.line);
                ImGui::PopID();
            }
            if (shown == 0) ImGui::TextDisabled("%s", m_process.running() ? tr("Building...") : wantErrors ? tr("No errors") : tr("No warnings"));
        } else {
            // Per line: if there's a "file(line)", make it clickable (opens in the code editor; relative paths are relative to the project folder)
            std::vector<std::pair<size_t, size_t>> lines;
            for (size_t pos = 0; pos < m_log.size();) {
                size_t end = m_log.find('\n', pos);
                if (end == std::string::npos) end = m_log.size();
                lines.emplace_back(pos, end);
                pos = end + 1;
            }
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(lines.size()));
            while (clipper.Step()) {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                    const std::string line = m_log.substr(lines[i].first, lines[i].second - lines[i].first);
                    FileLine at;
                    if (!parseFileLine(line, &at)) {
                        ImGui::TextUnformatted(line.c_str());
                        continue;
                    }
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.75f, 1.0f, 1.0f));
                    ImGui::TextUnformatted(line.c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                        ImGui::SetTooltip("%s", tr("Click to open in the code editor"));
                    }
                    if (ImGui::IsItemClicked()) openAt(at.file, at.line);
                }
            }
            if (m_scroll) {
                ImGui::SetScrollHereY(1.0f);
                m_scroll = false;
            }
        }
    }
    ImGui::EndChild();
    return requested;
}

} // namespace mealkit::editor
