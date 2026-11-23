// [Assets] window: assets/ folder tree and files, drag models into the scene view, rename/move (references fixed too), broken reference list.
// Assets are referenced by paths relative to assets ("models/tree.glb") (docs/CODING_MODEL.md "Asset references and loading").
#include "mealkit/editor/Editor.h"
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/EditorColors.h"

#include "mealkit/render/ModelDraw.h" // asset::ModelCache + its drawing
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"

#include <imgui.h>
#include <imgui_internal.h>

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

// Kinds for the filter combo; 0 matches everything
constexpr int kAssetKindCount = 7;
const char* assetKindName(int kind) { // already translated
    switch (kind) {
    case 0: return tr("All kinds");
    case 1: return tr("Models");
    case 2: return tr("Scenes and prefabs");
    case 3: return tr("Tables and text");
    case 4: return tr("Sounds");
    case 5: return tr("Images");
    default: return tr("Other");
    }
}

int assetKindOf(const std::string& name) {
    const std::string ext = lowerExtension(name);
    auto in = [&](std::initializer_list<const char*> list) {
        for (const char* e : list)
            if (ext == e) return true;
        return false;
    };
    if (in({".glb", ".gltf"})) return 1;
    if (in({".scene", ".prefab"})) return 2;
    if (in({".table", ".tuning", ".events", ".bindings", ".fx", ".ui", ".txt", ".csv"})) return 3;
    if (in({".wav", ".ogg", ".mp3", ".flac"})) return 4;
    if (in({".png", ".jpg", ".jpeg", ".tga", ".hdr", ".dds", ".ktx", ".ktx2", ".cube"})) return 5;
    return 6;
}

bool containsNoCase(const std::string& text, const char* needle) {
    const std::string n(needle);
    if (n.empty()) return true;
    auto it = std::search(text.begin(), text.end(), n.begin(), n.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != text.end();
}

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> out;
    std::string text;
    if (!readTextFile(path, text)) return out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

// Text files that can reference paths (scenes, tuning, UI, tables, effects, sound events, input)
bool referenceFile(const std::string& name) {
    const std::string ext = lowerExtension(name);
    for (const char* e : {".scene", ".prefab", ".tuning", ".ui", ".table", ".fx", ".events", ".bindings", ".txt"})
        if (ext == e) return true;
    return false;
}

void collectFiles(const std::string& root, const std::string& relative, std::vector<std::string>& out) {
    const std::string folder = relative.empty() ? root : root + MEALKIT_SEP + relative;
    for (const std::string& f : listFiles(folder)) out.push_back(relative.empty() ? f : relative + "/" + f);
    for (const std::string& d : listDirectories(folder))
        if (d[0] != '.') collectFiles(root, relative.empty() ? d : relative + "/" + d, out);
}

std::string toNative(std::string path) {
    path = nativePath(path);
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
    m_assetView = AssetView::Folder;
    m_assetSearch[0] = 0;
    m_assetKind = 0;
    m_assetsOpen = true;
    m_assetsFront = 3;
    m_assetScanTime = -1000.0;
}

void Editor::loadAssetLists() {
    m_assetListsLoaded = true;
    m_assetFavorites.clear();
    m_assetRecent.clear();
    if (m_project.directory.empty()) return;
    const std::string dir = m_project.directory + MEALKIT_SEP "build" MEALKIT_SEP "editor" MEALKIT_SEP;
    m_assetFavorites = readLines(dir + "asset_favorites.txt");
    m_assetRecent = readLines(dir + "asset_recent.txt");
}

void Editor::saveAssetLists() const {
    if (m_project.directory.empty()) return;
    const std::string dir = m_project.directory + MEALKIT_SEP "build" MEALKIT_SEP "editor" MEALKIT_SEP;
    std::string favorites, recent;
    for (const std::string& p : m_assetFavorites) favorites += p + "\n";
    for (const std::string& p : m_assetRecent) recent += p + "\n";
    createDirectories(m_project.directory + MEALKIT_SEP "build" MEALKIT_SEP "editor");
    writeTextFile(dir + "asset_favorites.txt", favorites);
    writeTextFile(dir + "asset_recent.txt", recent);
}

// Used = opened with a double-click or dragged out of the browser. Newest first, 12 kept
void Editor::rememberRecentAsset(const std::string& relative) {
    if (relative.empty()) return;
    std::erase(m_assetRecent, relative);
    m_assetRecent.insert(m_assetRecent.begin(), relative);
    if (m_assetRecent.size() > 12) m_assetRecent.resize(12);
    saveAssetLists();
}

void Editor::toggleAssetFavorite(const std::string& relative) {
    if (std::erase(m_assetFavorites, relative) == 0) m_assetFavorites.push_back(relative);
    saveAssetLists();
}

void Editor::drawAssetFolder(const std::string& root, const std::string& relative) {
    const std::string folder = relative.empty() ? root : root + MEALKIT_SEP + toNative(relative);
    auto directories = [this](const std::string& path) -> const std::vector<std::string>& {
        auto it = m_assetDirs.find(path);
        return it != m_assetDirs.end() ? it->second : m_assetDirs.emplace(path, listDirectories(path)).first->second;
    };
    const std::vector<std::string> folders = directories(folder); // copy: the map grows while walking children
    for (const std::string& d : folders) {
        if (d[0] == '.') continue;
        const std::string child = relative.empty() ? d : relative + "/" + d;
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (directories(root + MEALKIT_SEP + toNative(child)).empty()) flags |= ImGuiTreeNodeFlags_Leaf;
        if (m_assetFolder == child) flags |= ImGuiTreeNodeFlags_Selected;
        const bool open = ImGui::TreeNodeEx(child.c_str(), flags, "%s  %s", icon::kFolder, d.c_str());
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            m_assetFolder = child;
            m_assetView = AssetView::Folder;
        }
        if (open) {
            drawAssetFolder(root, child);
            ImGui::TreePop();
        }
    }
}

void Editor::drawAssetBrowser() {
    if (!m_assetsOpen) return;
    if (m_assetsFront > 0) {
        --m_assetsFront;
        ImGui::SetNextWindowFocus();
    }
    if (!ImGui::Begin(iconTitle(icon::kFolder, "Assets###Assets").c_str(), &m_assetsOpen)) {
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
        m_assetAll.clear(); // read again below only while searching
    }
    if (!m_assetListsLoaded) loadAssetLists();
    // Left: favorites, recent, then folders
    if (beginListPane("##folders", ImGui::GetFontSize() * 13.0f)) {
        if (ImGui::Selectable((std::string(icon::kStarFilled) + "  " + tr("Favorites")).c_str(), m_assetView == AssetView::Favorites)) m_assetView = AssetView::Favorites;
        if (ImGui::Selectable((std::string(icon::kLog) + "  " + tr("Recent")).c_str(), m_assetView == AssetView::Recent)) m_assetView = AssetView::Recent;
        ImGui::Separator();
        if (ImGui::Selectable((std::string(icon::kFolder) + "  assets").c_str(), m_assetView == AssetView::Folder && m_assetFolder.empty())) {
            m_assetFolder.clear();
            m_assetView = AssetView::Folder;
        }
        drawAssetFolder(root, {});
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // Right: files (drag models/prefabs into the scene view, double-click scenes to open)
    if (ImGui::BeginChild("##files")) {
        const std::string folder = m_assetFolder.empty() ? root : root + MEALKIT_SEP + toNative(m_assetFolder);
        // Path bar: every part is a button that goes up to that folder
        if (m_assetView == AssetView::Folder) {
            if (ImGui::SmallButton("assets")) m_assetFolder.clear();
            std::string walked;
            std::string rest = m_assetFolder;
            std::string goTo;
            bool jump = false;
            while (!rest.empty()) {
                const size_t slash = rest.find('/');
                const std::string part = rest.substr(0, slash);
                rest = slash == std::string::npos ? std::string() : rest.substr(slash + 1);
                walked += (walked.empty() ? "" : "/") + part;
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::TextDisabled("/");
                ImGui::SameLine(0.0f, 2.0f);
                ImGui::PushID(walked.c_str());
                if (ImGui::SmallButton(part.c_str())) {
                    goTo = walked;
                    jump = true;
                }
                ImGui::PopID();
            }
            if (jump) m_assetFolder = goTo;
        } else {
            ImGui::TextDisabled("%s", m_assetView == AssetView::Favorites ? tr("Favorites") : tr("Recent"));
        }
        // Search (all folders when not empty) and the kind filter
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        ImGui::InputTextWithHint("##assetSearch", tr("Search all assets"), m_assetSearch, sizeof(m_assetSearch));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
        if (beginCombo("##assetKind", assetKindName(m_assetKind))) {
            for (int k = 0; k < kAssetKindCount; ++k)
                if (ImGui::Selectable(assetKindName(k), m_assetKind == k)) m_assetKind = k;
            ImGui::EndCombo();
        }
        const bool searching = m_assetSearch[0] != 0;
        // Slider at the far left = list, further right = grid with bigger pictures
        const f32 sliderWidth = ImGui::GetFontSize() * 8.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - sliderWidth));
        ImGui::SetNextItemWidth(sliderWidth);
        ImGui::SliderFloat("##thumbSize", &m_assetThumb, 0.0f, 160.0f, m_assetThumb < 48.0f ? tr("List") : "%.0f");
        const f32 cell = m_assetThumb < 48.0f ? 0.0f : m_assetThumb;
        // Broken model references in the current scene (paths gone after renaming in Explorer)
        if (rescan) {
            std::set<std::string> missing;
            for (const scene::Entity& e : m_scene.entities)
                if (!e.model.empty() && !fileExists(root + MEALKIT_SEP + toNative(e.model)) && !fileExists(e.model)) missing.insert(e.model);
            m_assetMissing.assign(missing.begin(), missing.end());
        }
        if (!m_assetMissing.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, color::kDanger);
            for (const std::string& m : m_assetMissing) ImGui::TextWrapped("%s", trf("Missing in this scene: %s", m.c_str()).c_str());
            ImGui::PopStyleColor();
        }
        // The file list scrolls on its own, with the preview pane on its right (the window is wide and short)
        const f32 previewWidth = m_assetSelected.empty() ? 0.0f : ImGui::GetFontSize() * 22.0f;
        ImGui::BeginChild("##assetList", ImVec2(-previewWidth, 0.0f));
        // What is listed: the folder's files, or every match across assets while searching, or the favorites / recent lists
        std::vector<std::string> entries;
        if (searching || m_assetView != AssetView::Folder) {
            if (searching && m_assetAll.empty() && directoryExists(root)) collectFiles(root, {}, m_assetAll);
            const std::vector<std::string>& source = searching ? m_assetAll : (m_assetView == AssetView::Favorites ? m_assetFavorites : m_assetRecent);
            for (const std::string& r : source)
                if (!searching || containsNoCase(r, m_assetSearch)) entries.push_back(r);
        } else {
            auto listed = m_assetFiles.find(folder);
            if (listed == m_assetFiles.end()) listed = m_assetFiles.emplace(folder, listFiles(folder)).first;
            for (const std::string& name : listed->second) entries.push_back(m_assetFolder.empty() ? name : m_assetFolder + "/" + name);
        }
        if (entries.empty()) ImGui::TextDisabled("%s", tr("Nothing to show"));
        size_t shown = 0;
        for (const std::string& relative : entries) {
            const size_t slashAt = relative.rfind('/');
            const std::string name = slashAt == std::string::npos ? relative : relative.substr(slashAt + 1);
            if (name.size() > 9 && name.compare(name.size() - 9, 9, ".autosave") == 0) continue;
            if (m_assetKind != 0 && assetKindOf(name) != m_assetKind) continue;
            if (m_assetView != AssetView::Folder && !searching && !fileExists(root + MEALKIT_SEP + toNative(relative))) continue; // deleted since
            ++shown;
            const std::string itemFolder = slashAt == std::string::npos ? root : root + MEALKIT_SEP + toNative(relative.substr(0, slashAt));
            const bool favorite = std::find(m_assetFavorites.begin(), m_assetFavorites.end(), relative) != m_assetFavorites.end();
            ImGui::PushID(relative.c_str());
            const bool plainFolder = !searching && m_assetView == AssetView::Folder;
            const std::string label = std::string(favorite ? icon::kStarFilled : assetIcon(name)) + "  " + (plainFolder ? name : relative);
            const f32 textHeight = ImGui::GetTextLineHeight();
            const ImVec2 cellPos = ImGui::GetCursorScreenPos(); // the Selectable's rectangle reaches past this by half the item spacing
            const std::string shownLabel = cell > 0.0f ? "##cell" : label;
            if (ImGui::Selectable(shownLabel.c_str(), m_assetSelected == relative, ImGuiSelectableFlags_AllowDoubleClick,
                                  cell > 0.0f ? ImVec2(cell, cell + textHeight) : ImVec2(0.0f, 0.0f))) {
                m_assetSelected = relative;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    rememberRecentAsset(relative);
                    if (lowerExtension(name) == ".scene") { // edited in MkRender when installed, else opened here to look at and play
                        if (!openInSceneTool(relative)) openSceneFile(itemFolder + MEALKIT_SEP + name);
                    }
                }
            }
            if (ImGui::BeginDragDropSource()) { // to pick= fields (objects are placed in MkRender)
                ImGui::SetDragDropPayload(kAssetPayload, relative.c_str(), relative.size() + 1);
                if (m_assetRecent.empty() || m_assetRecent.front() != relative) rememberRecentAsset(relative);
                ImGui::Text("%s  %s", assetIcon(name), relative.c_str());
                ImGui::EndDragDropSource();
            }
            if (cell > 0.0f) { // picture for models, icon for the rest, then the name cut to the cell
                const ImVec2 min = cellPos;
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const unsigned long long picture = isModel(name) ? m_thumbnails.texture(relative, m_renderer) : 0;
                if (picture) {
                    draw->AddImage(ImTextureRef(static_cast<ImTextureID>(picture)), min, ImVec2(min.x + cell, min.y + cell));
                } else {
                    const char* glyph = assetIcon(name);
                    const ImVec2 size = ImGui::CalcTextSize(glyph);
                    draw->AddText(ImVec2(min.x + (cell - size.x) * 0.5f, min.y + (cell - size.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_TextDisabled), glyph);
                }
                draw->PushClipRect(ImVec2(min.x, min.y + cell), ImVec2(min.x + cell, min.y + cell + textHeight), true);
                draw->AddText(ImVec2(min.x, min.y + cell), ImGui::GetColorU32(ImGuiCol_Text), name.c_str());
                draw->PopClipRect();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", relative.c_str());
            }
            if (ImGui::BeginPopupContextItem("##assetMenu")) {
                if (ImGui::MenuItem(tr("Rename or Move..."), nullptr, false, m_mode == Mode::Edit)) {
                    m_assetRenameFrom = relative;
                    snprintf(m_assetRenameBuffer, sizeof(m_assetRenameBuffer), "%s", relative.c_str());
                }
                if (ImGui::MenuItem(favorite ? tr("Remove from Favorites") : tr("Add to Favorites"))) toggleAssetFavorite(relative);
                if (ImGui::MenuItem(tr("Show in Explorer"))) openInExplorer(itemFolder);
                if (!plainFolder && ImGui::MenuItem(tr("Show in Folder"))) {
                    m_assetView = AssetView::Folder;
                    m_assetSearch[0] = 0;
                    revealAsset(relative);
                }
                if (ImGui::MenuItem(tr("Find Uses"))) findAssetUses(relative);
                if (lowerExtension(name) == ".scene" && hasSceneTool() && ImGui::MenuItem(tr("Open in MkRender"))) openInSceneTool(relative);
                ImGui::EndPopup();
            }
            ImGui::PopID();
            if (cell > 0.0f && ImGui::GetItemRectMax().x + ImGui::GetStyle().ItemSpacing.x + cell < ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x)
                ImGui::SameLine();
        }
        ImGui::EndChild(); // ##assetList
        if (previewWidth > 0.0f) {
            ImGui::SameLine();
            drawAssetPreview(root);
        }
    }
    ImGui::EndChild();

    if (m_assetUsesOpen) {
        ImGui::OpenPopup("##assetUses");
        m_assetUsesOpen = false;
    }
    if (ImGui::BeginPopup("##assetUses")) {
        ImGui::TextDisabled("%s", m_assetUsesOf.c_str());
        if (m_assetUses.empty()) ImGui::TextUnformatted(tr("Not used by any scene, table or other text file"));
        for (const std::string& f : m_assetUses)
            if (ImGui::Selectable(f.c_str())) {
                if (lowerExtension(f) == ".scene" && m_mode == Mode::Edit) openSceneFile(root + MEALKIT_SEP + toNative(f));
                else revealAsset(f);
            }
        ImGui::EndPopup();
    }
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

// Bar of used / limit, red over the limit and yellow from 80 percent. A limit of 0 means none (only the number is shown)
static void budgetRow(const char* label, u32 used, u32 limit) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 7.0f);
    if (limit == 0) {
        ImGui::Text("%u", used);
        return;
    }
    const bool over = used > limit;
    ImVec4 color = over ? color::kDanger : (used * 5 >= limit * 4 ? color::kWarning : color::kGood);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
    char text[48];
    snprintf(text, sizeof(text), "%u / %u", used, limit);
    ImGui::ProgressBar(std::min(static_cast<f32>(used) / static_cast<f32>(limit), 1.0f), ImVec2(ImGui::GetFontSize() * 14.0f, ImGui::GetTextLineHeight()), text);
    ImGui::PopStyleColor();
}

// Right of the file list: a model's triangle, material, bone and texture counts against the budget of its kind folder (the same limits the load warns
// with), a sound's waveform with a play button, and the size of any file
void Editor::drawAssetPreview(const std::string& root) {
    ImGui::BeginChild("##assetPreview", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    const std::string& relative = m_assetSelected;
    const std::string full = root + MEALKIT_SEP + toNative(relative);
    const size_t slash = relative.rfind('/');
    const std::string name = slash == std::string::npos ? relative : relative.substr(slash + 1);
    ImGui::TextUnformatted(relative.c_str());
    if (FILE* f = openFile(full, "rb")) {
        fseek(f, 0, SEEK_END);
        const long bytes = ftell(f);
        fclose(f);
        ImGui::SameLine();
        if (bytes >= 1024 * 1024) ImGui::TextDisabled("%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        else ImGui::TextDisabled("%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else {
        ImGui::SameLine();
        ImGui::TextColored(color::kDanger, "%s", tr("File not found"));
    }
    if (isModel(name) && m_models) {
        const asset::Model* model = m_models->get(relative);
        if (!model || !model->valid) {
            ImGui::TextColored(color::kDanger, "%s", tr("The model could not be loaded"));
        } else {
            const asset::ModelBudget& budget = m_models->budget(model->kind);
            ImGui::TextDisabled("%s", trf("Budget kind: %s (from the folder)", asset::modelKindFolder(model->kind)).c_str());
            budgetRow(tr("Triangles"), model->triangles[0], budget.triangles);
            budgetRow(tr("Materials"), model->materials, budget.materials);
            if (model->bones > 0 || budget.bones > 0) budgetRow(tr("Bones"), model->bones, budget.bones);
            budgetRow(tr("Texture size"), model->textureSize, budget.textureSize);
            ImGui::TextDisabled("%s", trf("%u LODs, %zu clips, %u textures", model->lodCount, model->clips.size(), model->textures).c_str());
        }
    } else if (assetKindOf(name) == 4 && m_audio) {
        if (m_assetPreviewPath != relative) { // read once per selection
            m_assetPreviewPath = relative;
            m_assetPreviewPeaks.clear();
            m_assetPreviewSeconds = 0.0;
            if (m_audio->playing(m_assetPreviewVoice)) m_audio->stop(m_assetPreviewVoice);
            m_audio->peaks(full, 160, m_assetPreviewPeaks, m_assetPreviewSeconds);
        }
        const bool playing = m_audio->playing(m_assetPreviewVoice);
        if (iconButton(playing ? icon::kStop : icon::kPlay, playing ? tr("Stop") : tr("Play"))) {
            if (playing) m_audio->stop(m_assetPreviewVoice);
            else m_assetPreviewVoice = m_audio->playFile(full);
        }
        ImGui::SameLine();
        if (m_assetPreviewPeaks.empty()) {
            ImGui::TextColored(color::kDanger, "%s", tr("The sound could not be read"));
        } else {
            ImGui::TextDisabled("%.2f s", m_assetPreviewSeconds);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 size(std::max(ImGui::GetContentRegionAvail().x, 10.0f), std::max(ImGui::GetContentRegionAvail().y, 10.0f));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const f32 middle = min.y + size.y * 0.5f;
            const f32 step = size.x / static_cast<f32>(m_assetPreviewPeaks.size());
            for (size_t i = 0; i < m_assetPreviewPeaks.size(); ++i) {
                const f32 h = std::max(m_assetPreviewPeaks[i] * size.y * 0.5f, 1.0f);
                const f32 x = min.x + step * static_cast<f32>(i);
                draw->AddRectFilled(ImVec2(x, middle - h), ImVec2(x + std::max(step - 1.0f, 1.0f), middle + h), ImGui::GetColorU32(ImGuiCol_PlotHistogram));
            }
            ImGui::Dummy(size);
        }
    }
    ImGui::EndChild();
}

// Text files that name this asset, found the same way a rename finds what to fix. Effects and sound events named without their folder aren't found
void Editor::findAssetUses(const std::string& relative) {
    m_assetUses.clear();
    m_assetUsesOf = relative;
    m_assetUsesOpen = true;
    const std::string root = asset::ModelCache::assetsRoot();
    for (const std::string& f : allAssetFiles()) {
        if (f == relative || !referenceFile(f)) continue;
        std::string text;
        if (readTextFile(root + MEALKIT_SEP + toNative(f), text) && replacePath(text, relative, relative) > 0) m_assetUses.push_back(f);
    }
}

// Moves the file and fixes path references in text files under assets and in mealkit.project. The open scene is fixed in memory too
void Editor::renameAsset(const std::string& from, const std::string& toRaw) {
    std::string to = toRaw;
    std::replace(to.begin(), to.end(), '\\', '/');
    while (!to.empty() && to.front() == '/') to.erase(0, 1);
    if (to.empty() || to == from) return;
    const std::string root = asset::ModelCache::assetsRoot();
    const std::string fromPath = root + MEALKIT_SEP + toNative(from), toPath = root + MEALKIT_SEP + toNative(to);
    if (fileExists(toPath)) {
        setStatus(trf("Already exists: %s", to.c_str()), true);
        return;
    }
    const size_t slash = toPath.rfind(kPathSeparator);
    createDirectories(toPath.substr(0, slash));
    if (!moveFile(fromPath, toPath)) {
        setStatus(trf("Could not move %s", from.c_str()), true);
        return;
    }
    std::vector<std::string> files;
    collectFiles(root, {}, files);
    size_t changedFiles = 0, changedRefs = 0;
    std::vector<std::string> paths;
    for (const std::string& f : files)
        if (referenceFile(f)) paths.push_back(root + MEALKIT_SEP + toNative(f));
    if (!m_project.directory.empty()) paths.push_back(m_project.filePath());
    for (const std::string& path : paths) {
        std::string text;
        if (!readTextFile(path, text)) continue;
        const size_t n = replacePath(text, from, to);
        if (n == 0) continue;
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
