// [Assets] window: assets/ folder tree and files, drag models into the scene view, rename/move (references fixed too), broken reference list.
// Assets are referenced by paths relative to assets ("models/tree.glb") (docs/CODING_MODEL.md "Asset references and loading").
#include "mealkit/editor/Editor.h"

#include "mealkit/asset/ModelCache.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

namespace mealkit::editor {
namespace {

constexpr const char* kAssetPayload = "MK_ASSET";

std::string lowerExtension(const std::string& name) {
    const size_t dot = name.rfind('.');
    std::string ext = dot == std::string::npos ? std::string() : name.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool isModel(const std::string& name) {
    const std::string ext = lowerExtension(name);
    return ext == ".glb" || ext == ".gltf";
}

bool isPrefab(const std::string& name) { return lowerExtension(name) == ".prefab"; }

const char* assetIcon(const std::string& name) {
    const std::string ext = lowerExtension(name);
    if (ext == ".glb" || ext == ".gltf") return icon::kModel;
    if (ext == ".scene") return icon::kGlobe;
    if (ext == ".prefab") return icon::kPrefab;
    return icon::kFlag;
}

// Text files that can reference paths (scenes, tuning, UI, tables, effects, sound events, input)
bool referenceFile(const std::string& name) {
    const std::string ext = lowerExtension(name);
    for (const char* e : {".scene", ".prefab", ".tuning", ".ui", ".table", ".fx", ".events", ".bindings", ".txt"})
        if (ext == e) return true;
    return false;
}

void collectFiles(const std::string& root, const std::string& relative, std::vector<std::string>& out) {
    const std::string folder = relative.empty() ? root : root + "\\" + relative;
    for (const std::string& f : listFiles(folder)) out.push_back(relative.empty() ? f : relative + "/" + f);
    for (const std::string& d : listDirectories(folder))
        if (d[0] != '.') collectFiles(root, relative.empty() ? d : relative + "/" + d, out);
}

std::string toNative(std::string path) {
    std::replace(path.begin(), path.end(), '/', '\\');
    return path;
}

bool pathChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == '/'; }

// Replaces from with to in text (only where the surrounding characters aren't path characters). Returns the count
size_t replacePath(std::string& text, const std::string& from, const std::string& to) {
    size_t count = 0;
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at)) {
        const bool before = at == 0 || !pathChar(text[at - 1]);
        const bool after = at + from.size() >= text.size() || !pathChar(text[at + from.size()]);
        if (before && after) {
            text.replace(at, from.size(), to);
            at += to.size();
            ++count;
        } else {
            at += from.size();
        }
    }
    return count;
}

} // namespace

std::vector<std::string> Editor::allAssetFiles() const {
    std::vector<std::string> files;
    const std::string root = asset::ModelCache::assetsRoot();
    if (directoryExists(root)) collectFiles(root, {}, files);
    return files;
}

void Editor::revealAsset(const std::string& relative) {
    const size_t slash = relative.rfind('/');
    m_assetFolder = slash == std::string::npos ? std::string() : relative.substr(0, slash);
    m_assetSelected = relative;
    m_assetsOpen = true;
    m_assetsFront = true;
    m_assetScanTime = -1000.0;
}

void Editor::drawAssetFolder(const std::string& root, const std::string& relative) {
    const std::string folder = relative.empty() ? root : root + "\\" + toNative(relative);
    auto directories = [this](const std::string& path) -> const std::vector<std::string>& {
        auto it = m_assetDirs.find(path);
        return it != m_assetDirs.end() ? it->second : m_assetDirs.emplace(path, listDirectories(path)).first->second;
    };
    const std::vector<std::string> folders = directories(folder); // copy: the map grows while walking children
    for (const std::string& d : folders) {
        if (d[0] == '.') continue;
        const std::string child = relative.empty() ? d : relative + "/" + d;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (directories(root + "\\" + toNative(child)).empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (m_assetFolder == child) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(child.c_str(), flags, "%s  %s", icon::kFolder, d.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) m_assetFolder = child;
        if (open) {
            drawAssetFolder(root, child);
            ImGui::TreePop();
        }
    }
}

void Editor::drawAssetBrowser() {
    if (!m_assetsOpen) return;
    if (m_assetsFront) {
        m_assetsFront = false;
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin(trl("Assets###Assets"), &m_assetsOpen)) {
        ImGui::End();
        return;
    }
    const std::string root = asset::ModelCache::assetsRoot();
    const double now = ImGui::GetTime();
    const bool rescan = now - m_assetScanTime >= 1.0;
    if (rescan) {
        m_assetScanTime = now;
        m_assetDirs.clear();
        m_assetFiles.clear();
    }
    // Left: folders
    if (ImGui::BeginChild("##folders", ImVec2(ImGui::GetFontSize() * 13.0f, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders)) {
        if (ImGui::Selectable((std::string(icon::kFolder) + "  assets").c_str(), m_assetFolder.empty())) m_assetFolder.clear();
        drawAssetFolder(root, {});
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // Right: files (drag models/prefabs into the scene view, double-click scenes to open)
    if (ImGui::BeginChild("##files")) {
        const std::string folder = m_assetFolder.empty() ? root : root + "\\" + toNative(m_assetFolder);
        ImGui::TextDisabled("assets/%s", m_assetFolder.c_str());
        // Broken model references in the current scene (paths gone after renaming in Explorer)
        if (rescan) {
            std::set<std::string> missing;
            for (const scene::Entity& e : m_scene.entities)
                if (!e.model.empty() && !fileExists(root + "\\" + toNative(e.model)) && !fileExists(e.model)) missing.insert(e.model);
            m_assetMissing.assign(missing.begin(), missing.end());
        }
        if (!m_assetMissing.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.45f, 1.0f));
            for (const std::string& m : m_assetMissing) ImGui::TextWrapped("%s", trf("Missing in this scene: %s", m.c_str()).c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        auto listed = m_assetFiles.find(folder);
        if (listed == m_assetFiles.end()) listed = m_assetFiles.emplace(folder, listFiles(folder)).first;
        const std::vector<std::string> files = listed->second; // copy: double-clicking a scene can change the lists
        for (const std::string& name : files) {
            if (name.size() > 9 && name.compare(name.size() - 9, 9, ".autosave") == 0) continue;
            const std::string relative = m_assetFolder.empty() ? name : m_assetFolder + "/" + name;
            ImGui::PushID(relative.c_str());
            const std::string label = std::string(assetIcon(name)) + "  " + name;
            if (ImGui::Selectable(label.c_str(), m_assetSelected == relative, ImGuiSelectableFlags_AllowDoubleClick)) {
                m_assetSelected = relative;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (lowerExtension(name) == ".scene") openSceneFile(folder + "\\" + name);
                    else if (isModel(name) && m_mode == Mode::Edit) addModelEntity(relative, nullptr);
                    else if (isPrefab(name) && m_mode == Mode::Edit) placePrefab(relative, nullptr);
                }
            }
            if ((isModel(name) || isPrefab(name)) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(kAssetPayload, relative.c_str(), relative.size() + 1);
                ImGui::Text("%s  %s", assetIcon(name), relative.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::BeginPopupContextItem("##assetMenu")) {
                if (ImGui::MenuItem(tr("Rename or Move..."), nullptr, false, m_mode == Mode::Edit)) {
                    m_assetRenameFrom = relative;
                    snprintf(m_assetRenameBuffer, sizeof(m_assetRenameBuffer), "%s", relative.c_str());
                }
                if (ImGui::MenuItem(tr("Show in Explorer"))) openInExplorer(folder);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

    if (!m_assetRenameFrom.empty()) ImGui::OpenPopup("##renameAsset");
    if (ImGui::BeginPopupModal("##renameAsset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(tr("New path under assets (folders are created). References are updated."));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 26.0f);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        const bool enter = ImGui::InputText("##to", m_assetRenameBuffer, sizeof(m_assetRenameBuffer), ImGuiInputTextFlags_EnterReturnsTrue);
        if (ImGui::Button(tr("OK")) || enter) {
            renameAsset(m_assetRenameFrom, m_assetRenameBuffer);
            m_assetScanTime = -1000.0; // show the move right away
            m_assetRenameFrom.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            m_assetRenameFrom.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

// Dropping a model/prefab on the scene view adds a model object/prefab copies where the mouse points (on a hit object, or the y = 0 floor if none)
void Editor::handleAssetDrop() {
    const ImGuiPayload* payload = ImGui::GetDragDropPayload();
    if (!payload || !payload->IsDataType(kAssetPayload) || !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    if (mouse.x < m_viewport.x || mouse.y < m_viewport.y || mouse.x >= m_viewport.x + m_viewport.width ||
        mouse.y >= m_viewport.y + m_viewport.height || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
        return;
    Vec3 origin, dir;
    mouseRay(mouse.x, mouse.y, &origin, &dir);
    f32 t = 0.0f;
    Vec3 at;
    if (raycast(origin, dir, &t)) at = origin + dir * t;
    else if (dir.y < -0.01f) at = origin + dir * (-origin.y / dir.y);
    else at = origin + dir * 12.0f;
    const std::string relative = static_cast<const char*>(payload->Data);
    if (isPrefab(relative)) placePrefab(relative, &at);
    else addModelEntity(relative, &at);
}

// Moves the file and fixes path references in text files under assets and in mealkit.project. The open scene is fixed in memory too
void Editor::renameAsset(const std::string& from, const std::string& toRaw) {
    std::string to = toRaw;
    std::replace(to.begin(), to.end(), '\\', '/');
    while (!to.empty() && to.front() == '/') to.erase(0, 1);
    if (to.empty() || to == from) return;
    const std::string root = asset::ModelCache::assetsRoot();
    const std::string fromPath = root + "\\" + toNative(from), toPath = root + "\\" + toNative(to);
    if (fileExists(toPath)) {
        setStatus(trf("Already exists: %s", to.c_str()), true);
        return;
    }
    const size_t slash = toPath.rfind('\\');
    createDirectories(toPath.substr(0, slash));
    if (!moveFile(fromPath, toPath)) {
        setStatus(trf("Could not move %s", from.c_str()), true);
        return;
    }
    // Fix references
    std::vector<std::string> files;
    collectFiles(root, {}, files);
    size_t changedFiles = 0, changedRefs = 0;
    std::vector<std::string> paths;
    for (const std::string& f : files)
        if (referenceFile(f)) paths.push_back(root + "\\" + toNative(f));
    if (!m_project.directory.empty()) paths.push_back(m_project.filePath());
    for (const std::string& path : paths) {
        std::string text;
        if (!readTextFile(path, text)) continue;
        const size_t n = replacePath(text, from, to);
        if (n == 0) continue;
        if (path == m_scenePath && m_dirty) continue; // the unsaved open scene is fixed in memory below and written on save
        writeTextFile(path, text);
        ++changedFiles;
        changedRefs += n;
    }
    // Open scene (memory)
    for (scene::Entity& e : m_scene.entities) {
        if (e.model == from) {
            e.model = to;
            ++changedRefs;
        }
        for (auto& [path, value] : e.fields) changedRefs += replacePath(value, from, to);
    }
    if (!m_scenePath.empty()) m_sceneTime = fileWriteTime(m_scenePath); // so the scene rewritten above isn't seen as "changed outside"
    if (m_assetSelected == from) m_assetSelected = to;
    setStatus(trf("Moved %s -> %s (%zu references in %zu files)", from.c_str(), to.c_str(), changedRefs, changedFiles));
    MEALKIT_INFO("Asset moved: %s -> %s (%zu references in %zu files)", from.c_str(), to.c_str(), changedRefs, changedFiles);
}

} // namespace mealkit::editor
