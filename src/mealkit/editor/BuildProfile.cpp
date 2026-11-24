#include "mealkit/editor/BuildProfile.h"

#include "mealkit/editor/Project.h"

#include "mealkit/core/Log.h"
#include "mealkit/core/TextFormat.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>

namespace mealkit::editor {

namespace {

constexpr text::Format kProfileFormat{"profile", 1};

std::string trimmed(const std::string& s) {
    size_t begin = 0, end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r')) ++begin;
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r')) --end;
    return s.substr(begin, end - begin);
}

} // namespace

bool validProfileId(const std::string& id) {
    if (id.empty()) return false;
    return std::all_of(id.begin(), id.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

std::string buildProfileDirectory(const std::string& projectDirectory) { return projectDirectory + "/profiles"; }

std::vector<BuildProfile> loadBuildProfiles(const std::string& projectDirectory) {
    std::vector<BuildProfile> profiles;
    const std::string directory = buildProfileDirectory(projectDirectory);
    for (const std::string& file : listFiles(directory, ".profile")) {
        BuildProfile profile;
        profile.id = file.substr(0, file.size() - 8);
        if (!validProfileId(profile.id)) {
            MEALKIT_WARN("Build profile %s skipped: the file name may only use letters, digits, '_', '-' and '.'", file.c_str());
            continue;
        }
        std::string content;
        const std::string path = directory + "/" + file;
        if (!readTextFile(path, content)) continue;
        size_t start = 0;
        while (start <= content.size()) {
            size_t end = content.find('\n', start);
            if (end == std::string::npos) end = content.size();
            const std::string line = trimmed(content.substr(start, end - start));
            start = end + 1;
            if (line.empty() || line[0] == '#') continue;
            if (line[0] == '@') {
                u32 version = 0;
                text::readFormatLine(line, kProfileFormat, path, version);
                continue;
            }
            const size_t split = line.find_first_of(" \t=");
            const std::string key = line.substr(0, split);
            std::string value = split == std::string::npos ? std::string() : trimmed(line.substr(split + 1));
            if (!value.empty() && value[0] == '=') value = trimmed(value.substr(1));
            if (key == "label") profile.label = value;
            else if (key == "config") profile.config = value;
            else if (key == "output") profile.output = value;
        }
        if (profile.config.empty()) {
            MEALKIT_WARN("Build profile %s skipped: no config line", file.c_str());
            continue;
        }
        profiles.push_back(std::move(profile));
    }
    return profiles; // listFiles is sorted by name
}

bool saveBuildProfile(const std::string& projectDirectory, const BuildProfile& profile) {
    if (!validProfileId(profile.id) || profile.config.empty()) return false;
    const std::string directory = buildProfileDirectory(projectDirectory);
    if (!createDirectories(directory)) return false;
    std::string out = "# MealKit build profile. Build it from the editor's [Build Log] window or with the configuration profile:" + profile.id + "\n";
    out += text::formatLine(kProfileFormat) + "\n";
    if (!profile.label.empty()) out += "label " + profile.label + "\n";
    out += "config " + profile.config + "\n";
    if (!profile.output.empty()) out += "output " + profile.output + "\n";
    return writeTextFile(directory + "/" + profile.id + ".profile", out);
}

std::vector<BuildProfile> defaultBuildProfiles() {
    return {
        {"windows-dx12", "Windows DX12", "Export", "export/windows-dx12"},
        {"windows-vulkan", "Windows Vulkan", "VulkanExport", "export/windows-vulkan"},
        {"linux-vulkan", "Linux Vulkan", "Export", "export/linux-vulkan"},
        {"debug", "Debug", "Debug", ""},
    };
}

std::string buildProfileOutputDirectory(const std::string& projectDirectory, const std::string& projectName, const BuildProfile& profile) {
    if (profile.output.empty()) return projectDirectory + "/export/" + buildOsName() + "/" + projectName;
    return isAbsolutePath(profile.output) ? profile.output : projectDirectory + "/" + profile.output;
}

} // namespace mealkit::editor
