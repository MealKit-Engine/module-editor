// [에셋] 창: assets/ 폴더 트리와 파일, 모델을 씬 뷰로 끌어 놓기, 이름 바꾸기·옮기기(참조 같이 고침), 끊긴 참조 목록.
// 에셋은 assets 기준 경로("models/tree.glb")로 참조한다 (docs/CODING_MODEL.md "에셋 참조와 적재").
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

const char* assetIcon(const std::string& name) {
    const std::string ext = lowerExtension(name);
    if (ext == ".glb" || ext == ".gltf") return icon::kModel;
    if (ext == ".scene") return icon::kGlobe;
    return icon::kFlag;
}

// 경로를 참조할 수 있는 텍스트 파일 (씬·튜닝·UI·표·효과·소리 이벤트·입력)
bool referenceFile(const std::string& name) {
    const std::string ext = lowerExtension(name);
    for (const char* e : {".scene", ".tuning", ".ui", ".table", ".fx", ".events", ".bindings", ".txt"})
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

// text 안의 from을 to로 (앞뒤가 경로 글자가 아닌 곳만). 바꾼 수
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

void Editor::drawAssetFolder(const std::string& root, const std::string& relative) {
    const std::string folder = relative.empty() ? root : root + "\\" + toNative(relative);
    for (const std::string& d : listDirectories(folder)) {
        if (d[0] == '.') continue;
        const std::string child = relative.empty() ? d : relative + "/" + d;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (listDirectories(root + "\\" + toNative(child)).empty()) flags |= ImGuiTreeNodeFlags_Leaf;
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
    if (!ImGui::Begin(trl("Assets###Assets"), &m_assetsOpen)) {
        ImGui::End();
        return;
    }
    const std::string root = asset::ModelCache::assetsRoot();
    // 왼쪽: 폴더
    if (ImGui::BeginChild("##folders", ImVec2(ImGui::GetFontSize() * 13.0f, 0), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders)) {
        if (ImGui::Selectable((std::string(icon::kFolder) + "  assets").c_str(), m_assetFolder.empty())) m_assetFolder.clear();
        drawAssetFolder(root, {});
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // 오른쪽: 파일 (모델은 씬 뷰로 끌어 놓는다, 씬은 두 번 눌러 연다)
    if (ImGui::BeginChild("##files")) {
        const std::string folder = m_assetFolder.empty() ? root : root + "\\" + toNative(m_assetFolder);
        ImGui::TextDisabled("assets/%s", m_assetFolder.c_str());
        // 지금 씬의 끊긴 모델 참조 (탐색기에서 바꿔 경로가 없어진 것)
        std::set<std::string> missing;
        for (const scene::Entity& e : m_scene.entities)
            if (!e.model.empty() && !fileExists(root + "\\" + toNative(e.model)) && !fileExists(e.model)) missing.insert(e.model);
        if (!missing.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.45f, 1.0f));
            for (const std::string& m : missing) ImGui::TextWrapped("%s", trf("Missing in this scene: %s", m.c_str()).c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        for (const std::string& name : listFiles(folder)) {
            if (name.size() > 9 && name.compare(name.size() - 9, 9, ".autosave") == 0) continue;
            const std::string relative = m_assetFolder.empty() ? name : m_assetFolder + "/" + name;
            ImGui::PushID(relative.c_str());
            const std::string label = std::string(assetIcon(name)) + "  " + name;
            if (ImGui::Selectable(label.c_str(), m_assetSelected == relative, ImGuiSelectableFlags_AllowDoubleClick)) {
                m_assetSelected = relative;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    if (lowerExtension(name) == ".scene") openSceneFile(folder + "\\" + name);
                    else if (isModel(name) && m_mode == Mode::Edit) addModelEntity(relative, nullptr);
                }
            }
            if (isModel(name) && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload(kAssetPayload, relative.c_str(), relative.size() + 1);
                ImGui::Text("%s  %s", icon::kModel, relative.c_str());
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

// 모델을 씬 뷰에 놓으면 마우스가 가리키는 곳(맞는 물체 위, 없으면 y = 0 바닥)에 모델 물체를 더한다
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
    addModelEntity(static_cast<const char*>(payload->Data), &at);
}

// 파일을 옮기고, assets 아래 텍스트 파일과 mealkit.project의 경로 참조를 같이 고친다. 열린 씬도 메모리에서 고친다
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
    // 참조 고치기
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
        if (path == m_scenePath && m_dirty) continue; // 저장 안 된 열린 씬은 아래에서 메모리를 고치고 저장할 때 쓴다
        writeTextFile(path, text);
        ++changedFiles;
        changedRefs += n;
    }
    // 열린 씬 (메모리)
    for (scene::Entity& e : m_scene.entities) {
        if (e.model == from) {
            e.model = to;
            ++changedRefs;
        }
        for (auto& [path, value] : e.fields) changedRefs += replacePath(value, from, to);
    }
    if (!m_scenePath.empty()) m_sceneTime = fileWriteTime(m_scenePath); // 위에서 고쳐 쓴 씬을 "바깥에서 바뀜"으로 보지 않게
    if (m_assetSelected == from) m_assetSelected = to;
    setStatus(trf("Moved %s -> %s (%zu references in %zu files)", from.c_str(), to.c_str(), changedRefs, changedFiles));
    MEALKIT_INFO("Asset moved: %s -> %s (%zu references in %zu files)", from.c_str(), to.c_str(), changedRefs, changedFiles);
}

} // namespace mealkit::editor
