#pragma once

#include <string>
#include <vector>

// Build profiles (docs/DECISIONS.md 2026-11-23 "Build profiles"): one text file per profile in <project>/profiles/<id>.profile.
//   @format profile 1
//   label Windows Vulkan
//   config VulkanExport        the build script's configuration (Release, Debug, Profile, Export, VulkanExport, Vulkan ...)
//   output export/win-vulkan   folder an export is gathered in (Export and VulkanExport only; default export/<name>)
// The build script reads the same file for the configuration "profile:<id>", so a profile builds the same from a shell or the editor.
namespace mealkit::editor {

struct BuildProfile {
    std::string id;     // file name without ".profile": letters, digits, '_', '-', '.'
    std::string label;  // shown in the editor (id when empty)
    std::string config; // build script configuration
    std::string output; // export folder relative to the project (or absolute); empty = export/<name>

    const std::string& title() const { return label.empty() ? id : label; }
    bool isExport() const { return config == "Export" || config == "VulkanExport"; }
};

std::string buildProfileDirectory(const std::string& projectDirectory); // <project>/profiles
std::vector<BuildProfile> loadBuildProfiles(const std::string& projectDirectory); // sorted by id; bad files are skipped with a warning
bool saveBuildProfile(const std::string& projectDirectory, const BuildProfile& profile);
// The starting set written by [Add Default Profiles] (a file that exists is never overwritten)
std::vector<BuildProfile> defaultBuildProfiles();
bool validProfileId(const std::string& id);
// Where an export profile gathers its files
std::string buildProfileOutputDirectory(const std::string& projectDirectory, const std::string& projectName, const BuildProfile& profile);

} // namespace mealkit::editor
