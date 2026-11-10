#include "mealkit/editor/ProjectManager.h"
#include "mealkit/editor/FieldEditor.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Project.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/FileDialog.h"
#include "mealkit/platform/Image.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"
#include "mealkit/platform/Window.h"
#include "mealkit/render/Renderer.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <vector>

// Project manager (follows the Godot editor's project manager):
//   top: [Create] [Import] [Search] ............ [Language]
//   list: star (favorite) | icon (click to change) | name (click to edit = exe name) / folder (click to edit = move folder) | created/modified dates
// Renaming runs tools\rename_project.ps1, new projects run tools\new_project.ps1, without a window.
namespace mealkit::editor {
namespace {
namespace fs = std::filesystem;

constexpr int kDefaultIconResource = 2; // RCDATA in MealKitEditor.rc (MealKit icon)
constexpr u32 kIconPixels = 128;
constexpr u32 kPreviewPixels = 640; // template preview images (longest side)

// New project templates (tools\new_project.ps1 -Template): script name, label, description. Previews: templates\previews\<script>.png
struct ProjectTemplate {
    const char* script;
    const char* label;
    const char* description;
};
constexpr ProjectTemplate kTemplates[] = {
    {"empty", "3D empty project", "One character per player on a floor, with the title, lobby and results screens. Start your own 3D game here."},
    {"empty2d", "2D empty project",
     "A side view on the XY plane with an orthographic camera: characters run and jump on platforms. The editor opens it in 2D."},
    {"arena", "Arena versus (sample game)", "A copy of the sample game: physics party versus with AI, boxes, jump pads and cutscenes."},
    {"coop", "Co-op adventure (sample game)", "A copy of the co-op sample: heroes cross the hills to the goal, chasers knock them down and teammates revive them."},
};
constexpr int kTemplateCount = static_cast<int>(sizeof(kTemplates) / sizeof(kTemplates[0]));
constexpr FileFilter kImageFilter{"Image", "*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.ico;*.tif;*.tiff"};

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool validGameName(const char* name) {
    if (!std::isalpha(static_cast<unsigned char>(name[0]))) return false;
    for (const char* c = name; *c; ++c)
        if (!std::isalnum(static_cast<unsigned char>(*c)) && *c != '_') return false;
    return true;
}

bool containsNoCase(const std::string& text, const char* needle) {
    if (!needle[0]) return true;
    auto it = std::search(text.begin(), text.end(), needle, needle + std::strlen(needle), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != text.end();
}

// Time of the most recently modified file in the project. Build output, version control, and third-party folders aren't checked
u64 lastModifiedTime(const std::string& directory) {
    static const char* const kSkip[] = {"build", "export", "third_party", "node_modules"};
    u64 newest = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(fromUtf8(directory), fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::u8string name = it->path().filename().u8string();
        if (it->is_directory(ec)) {
            const std::string n(name.begin(), name.end());
            bool skip = !n.empty() && n[0] == '.';
            for (const char* s : kSkip) skip |= _stricmp(n.c_str(), s) == 0;
            if (skip) it.disable_recursion_pending();
            continue;
        }
        const std::u8string full = it->path().u8string();
        newest = std::max<u64>(newest, fileWriteTime(std::string(full.begin(), full.end())));
    }
    return newest;
}

struct Entry {
    std::string directory;
    ProjectInfo info;
    bool valid = false;
    bool favorite = false;
    bool engineSample = false; // the engine repo itself (name/folder can't be changed)
    std::string created;
    std::string modified;
    u64 modifiedTime = 0;
    ImTextureData* icon = nullptr; // MealKit icon if none
};

std::string iconPath(const Entry& e) {
    std::string icon = e.info.settings.icon.empty() ? std::string("icon.png") : e.info.settings.icon;
    std::replace(icon.begin(), icon.end(), '/', '\\');
    return e.directory + "\\" + icon;
}

// Move a folder. Rename if on the same drive, otherwise copy then delete. build\ contains absolute paths, so it's deleted first
bool moveProjectFolder(const std::string& from, const std::string& to, std::string& error) {
    std::error_code ec;
    const fs::path src = fromUtf8(from), dst = fromUtf8(to);
    if (fs::exists(dst, ec) && !fs::is_empty(dst, ec)) {
        error = trf("%s already exists and is not empty", to.c_str());
        return false;
    }
    fs::remove(dst, ec); // if it's an empty folder, delete it and move into its place
    fs::create_directories(dst.parent_path(), ec);
    fs::remove_all(src / "build", ec);
    if (ec) {
        error = trf("Could not remove the build folder (is the project open?): %s", ec.message().c_str());
        return false;
    }
    fs::rename(src, dst, ec);
    if (ec) {
        // Different drive: copy then delete the original
        std::error_code copyError;
        fs::copy(src, dst, fs::copy_options::recursive, copyError);
        if (copyError) {
            fs::remove_all(dst, ec);
            error = trf("Could not move the folder: %s", copyError.message().c_str());
            return false;
        }
        fs::remove_all(src, ec);
    }
    return true;
}

// Text that starts editing on click. Looks like an input field on hover. true if clicked
bool clickableText(const char* id, const char* text, ImU32 color, bool enabled, const char* disabledTip) {
    const ImVec2 size = ImGui::CalcTextSize(text);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 pad(4.0f * ImGui::GetStyle().FontScaleDpi, 1.0f);
    ImGui::SetNextItemAllowOverlap();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(std::max(size.x, 1.0f), size.y));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered && enabled) {
        draw->AddRectFilled(ImVec2(pos.x - pad.x, pos.y - pad.y), ImVec2(pos.x + size.x + pad.x, pos.y + size.y + pad.y),
                            ImGui::GetColorU32(ImGuiCol_FrameBgHovered), ImGui::GetStyle().FrameRounding);
        ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    }
    draw->AddText(pos, color, text);
    if (hovered && !enabled && disabledTip) ImGui::SetTooltip("%s", disabledTip);
    return pressed && enabled;
}

// Name/path input field (Enter = confirm, Esc/click elsewhere = cancel). 0 = continue, 1 = confirm, -1 = cancel
int inlineEdit(const char* id, char* buffer, size_t size, f32 width, bool& focus) {
    if (focus) {
        ImGui::SetKeyboardFocusHere();
        focus = false;
    }
    ImGui::SetNextItemWidth(width);
    const bool enter = ImGui::InputText(id, buffer, size, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (enter) return 1;
    if (ImGui::IsItemDeactivated() || ImGui::IsKeyPressed(ImGuiKey_Escape)) return -1;
    return 0;
}

} // namespace

std::string runProjectManager(Window& window, render::Renderer& renderer, const std::string& engineDirectory,
                              unsigned maxFrames, const std::string& capturePath) {
    std::string engineDir = engineDirectory;
    std::replace(engineDir.begin(), engineDir.end(), '/', '\\'); // CMake path → Windows notation
    ImGuiLayer imgui;
    if (!imgui.init(window, renderer, executableDirectory() + "\\mealkit_projects.ini")) return {};
    window.setTitle(tr("MealKit Engine - Project Manager"));

    ImTextureData* defaultIcon = nullptr;
    // Template previews: <engine>\templates\previews\<script>.png (missing = blank card)
    ImTextureData* previews[kTemplateCount] = {};
    for (int i = 0; i < kTemplateCount; ++i) {
        Image image;
        if (loadImage(engineDir + "\\templates\\previews\\" + kTemplates[i].script + ".png", kPreviewPixels, image))
            previews[i] = imgui.createTexture(image.rgba.data(), image.width, image.height);
    }
    {
        Image image;
        if (loadImageResource(kDefaultIconResource, kIconPixels, image))
            defaultIcon = imgui.createTexture(image.rgba.data(), image.width, image.height);
    }

    std::vector<Entry> entries;
    std::string selected;    // selected project folder
    bool scrollToSelected = false;
    auto reload = [&] {
        for (Entry& e : entries) imgui.releaseTexture(e.icon);
        entries.clear();
        const std::vector<std::string> favorites = loadFavoriteProjects();
        for (const std::string& dir : loadRecentProjects()) {
            Entry e;
            e.directory = dir;
            e.valid = loadProject(dir, e.info);
            if (e.valid) e.directory = e.info.directory;
            for (const std::string& f : favorites) e.favorite |= sameProjectDirectory(f, dir);
            e.engineSample = sameProjectDirectory(dir, engineDir);
            if (e.valid) {
                e.created = !e.info.settings.created.empty() ? e.info.settings.created
                                                             : formatFileTime(fileCreationTime(e.directory), false);
                e.modifiedTime = lastModifiedTime(e.directory);
                e.modified = formatFileTime(e.modifiedTime, true);
                Image image;
                if (loadImage(iconPath(e), kIconPixels, image))
                    e.icon = imgui.createTexture(image.rgba.data(), image.width, image.height);
            }
            entries.push_back(std::move(e));
        }
        // Favorites first, then by most recently modified
        std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
            if (a.favorite != b.favorite) return a.favorite;
            return a.modifiedTime > b.modifiedTime;
        });
    };
    reload();

    std::string chosen;
    std::string message;
    bool messageIsError = false;
    auto showError = [&](std::string text) {
        message = std::move(text);
        messageIsError = true;
    };

    // New project window
    char newName[64] = "MyGame";
    int newTemplate = 0; // index into kTemplates
    char newLocation[512] = {};
    {
        // Default location: folder of the most recent project, or Documents\MealKitProjects if none
        std::string location;
        if (!entries.empty()) location = parentDirectory(loadRecentProjects().front());
        if (location.empty()) {
            const std::string documents = documentsDirectory();
            if (!documents.empty()) location = documents + "\\MealKitProjects";
        }
        strncpy_s(newLocation, location.c_str(), _TRUNCATE);
    }
    bool openCreate = false;

    // Tools run without a window (new project / rename). One at a time
    enum class Task { None, Create, Rename };
    ChildProcess process;
    Task task = Task::None;
    std::string taskLog;
    std::string taskDirectory; // project folder being created or renamed
    bool showLog = false;

    // Cell being edited in the list
    enum class Edit { None, Name, Path };
    Edit editing = Edit::None;
    std::string editDirectory;
    char editBuffer[512] = {};
    bool editFocus = false;
    // Values to pass to the confirmation window
    std::string confirmDirectory, confirmValue;
    Edit confirmKind = Edit::None;

    auto openProject = [&](const std::string& dir) {
        if (!isProjectDirectory(dir)) {
            showError(trf("Not a project folder (no mealkit.project): %s", dir.c_str()));
            return;
        }
        chosen = dir;
    };
    auto changeIcon = [&](Entry& e) {
        const std::string file =
            openFileDialog(window.nativeHandle(), "", e.directory, &kImageFilter, 1);
        if (file.empty()) return;
        Image image;
        if (!loadImage(file, 256, image)) {
            showError(trf("Could not read the image: %s", file.c_str()));
            return;
        }
        if (e.info.settings.icon.empty()) {
            e.info.settings.icon = "icon.png";
            game::saveProjectFile(e.info.filePath(), e.info);
        }
        if (!savePng(iconPath(e), image)) {
            showError(trf("Could not write %s", iconPath(e).c_str()));
            return;
        }
        imgui.releaseTexture(e.icon);
        Image shown;
        e.icon = loadImage(iconPath(e), kIconPixels, shown) ? imgui.createTexture(shown.rgba.data(), shown.width, shown.height)
                                                            : nullptr;
        message = trf("Icon changed: %s", iconPath(e).c_str());
        messageIsError = false;
    };
    auto startTask = [&](Task kind, const std::string& script, const std::string& arguments, const std::string& dir) {
        taskLog.clear();
        message.clear();
        taskDirectory = dir;
        const std::string command = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + engineDir +
                                    "\\tools\\" + script + "\" " + arguments;
        if (!process.start(command, engineDir)) {
            showError(trf("Could not run %s", script.c_str()));
            return;
        }
        task = kind;
    };

    unsigned frameCount = 0;
    while (chosen.empty() && window.pumpMessages()) {
        if (window.consumeResize()) renderer.resize(window.width(), window.height());

        if (task != Task::None && !process.poll(taskLog)) {
            const bool ok = process.exitCode() == 0;
            if (task == Task::Create) {
                if (ok && isProjectDirectory(taskDirectory)) {
                    addRecentProject(taskDirectory);
                    chosen = taskDirectory;
                } else {
                    showError(tr("Could not create the project (see the log)"));
                }
            } else if (task == Task::Rename) {
                if (ok) {
                    message = tr("Renamed. The game code is rebuilt when the project opens.");
                    messageIsError = false;
                } else {
                    showError(tr("Could not rename the project (see the log)"));
                    showLog = true;
                }
                reload();
            }
            task = Task::None;
        }
        const bool busy = task != Task::None;

        imgui.newFrame();
        const ImGuiStyle& style = ImGui::GetStyle();
        const f32 dpi = style.FontScaleDpi;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##projects", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        // ---- Top bar: create, import, search ... language
        static char filter[128] = {};
        ImGui::BeginDisabled(busy);
        if (ImGui::Button((std::string(icon::kAdd) + "  " + tr("Create")).c_str())) openCreate = true;
        ImGui::SameLine();
        if (ImGui::Button((std::string(icon::kFolder) + "  " + tr("Import")).c_str())) {
            const std::string dir = pickFolderDialog(window.nativeHandle(), newLocation);
            if (!dir.empty()) {
                if (isProjectDirectory(dir)) {
                    addRecentProject(dir);
                    reload();
                    ProjectInfo info;
                    if (loadProject(dir, info)) selected = info.directory;
                    scrollToSelected = true;
                } else {
                    showError(trf("Not a project folder (no mealkit.project): %s", dir.c_str()));
                }
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();

        const char* langName = kLanguages[0].name;
        for (const Language& lang : kLanguages)
            if (std::strcmp(lang.code, currentLanguage()) == 0) langName = lang.name;
        const std::string langLabel = std::string(icon::kGlobe) + "  " + langName;
        const f32 langWidth = ImGui::CalcTextSize(langLabel.c_str()).x + ImGui::GetFrameHeight() + style.FramePadding.x * 2 + 8 * dpi;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - langWidth - style.ItemSpacing.x);
        ImGui::InputTextWithHint("##filter", (std::string(icon::kSearch) + "  " + tr("Filter projects")).c_str(), filter,
                                 sizeof(filter));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(langWidth);
        if (beginCombo("##language", langLabel.c_str())) {
            for (const Language& lang : kLanguages) {
                if (ImGui::Selectable(lang.name, std::strcmp(lang.code, currentLanguage()) == 0)) {
                    setLanguage(lang.code);
                    window.setTitle(tr("MealKit Engine - Project Manager"));
                }
            }
            ImGui::EndCombo();
        }

        // ---- Project list
        const f32 bottom = (message.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing()) +
                           (showLog && !taskLog.empty() ? 160.0f * dpi + style.ItemSpacing.y : 0.0f);
        ImGui::BeginChild("##list", ImVec2(0, -bottom), ImGuiChildFlags_Borders);
        const f32 iconSize = ImGui::GetFontSize() * 3.0f;
        const f32 rowHeight = iconSize + style.FramePadding.y * 4;
        const f32 nameSize = style.FontSizeBase * 1.3f;
        Entry* iconToChange = nullptr;
        std::string removeDir;
        int shown = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            Entry& e = entries[i];
            const std::string displayName = e.valid ? e.info.name() : tr("(missing)");
            if (!containsNoCase(displayName, filter) && !containsNoCase(e.directory, filter)) continue;
            ++shown;
            ImGui::PushID(e.directory.c_str());
            const ImVec2 rowStart = ImGui::GetCursorPos();
            const bool isSelected = selected == e.directory;

            // Whole row: select, double-click to open, right-click menu
            ImGui::BeginDisabled(busy);
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::Selectable("##row", isSelected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, rowHeight))) {
                selected = e.directory;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && e.valid) openProject(e.directory);
            }
            if (isSelected && scrollToSelected) {
                ImGui::SetScrollHereY();
                scrollToSelected = false;
            }
            if (ImGui::BeginPopupContextItem("##menu")) {
                selected = e.directory;
                if (ImGui::MenuItem(tr("Open"), nullptr, false, e.valid)) openProject(e.directory);
                if (ImGui::MenuItem(tr("Show in Explorer"), nullptr, false, e.valid)) openInExplorer(e.directory);
                if (ImGui::MenuItem(tr("Change icon..."), nullptr, false, e.valid)) iconToChange = &e;
                ImGui::Separator();
                if (ImGui::MenuItem(tr("Remove from list"))) removeDir = e.directory;
                ImGui::EndPopup();
            }
            const f32 rowRight = ImGui::GetItemRectMax().x;

            // Favorite star
            ImGui::SetCursorPos(ImVec2(rowStart.x + style.FramePadding.x, rowStart.y + (rowHeight - ImGui::GetFrameHeight()) * 0.5f));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, e.favorite ? ImVec4(1.0f, 0.78f, 0.25f, 1.0f) : style.Colors[ImGuiCol_TextDisabled]);
            ImGui::SetNextItemAllowOverlap();
            if (ImGui::Button(e.favorite ? icon::kStarFilled : icon::kStar)) {
                setFavoriteProject(e.directory, !e.favorite);
                e.favorite = !e.favorite;
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", e.favorite ? tr("Remove from favorites") : tr("Add to favorites"));

            // Icon (click to change)
            ImGui::SameLine();
            ImGui::SetCursorPosY(rowStart.y + (rowHeight - iconSize) * 0.5f);
            ImTextureData* tex = e.icon ? e.icon : defaultIcon;
            ImGui::SetNextItemAllowOverlap();
            if (tex) {
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                // If not square, show the center cropped
                const f32 aspect = static_cast<f32>(tex->Width) / static_cast<f32>(std::max(tex->Height, 1));
                const ImVec2 uv0 = aspect > 1.0f ? ImVec2(0.5f - 0.5f / aspect, 0.0f) : ImVec2(0.0f, 0.5f - 0.5f * aspect);
                const ImVec2 uv1 = ImVec2(1.0f - uv0.x, 1.0f - uv0.y);
                if (ImGui::ImageButton("##icon", tex->GetTexRef(), ImVec2(iconSize, iconSize), uv0, uv1) && e.valid)
                    iconToChange = &e;
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            } else {
                ImGui::Dummy(ImVec2(iconSize, iconSize));
            }
            if (ImGui::IsItemHovered() && e.valid) ImGui::SetTooltip("%s", tr("Click to change the icon"));

            // Name (top) and folder (bottom)
            ImGui::SameLine(0, style.ItemSpacing.x * 2);
            const f32 textX = ImGui::GetCursorPosX();
            const f32 datesWidth = ImGui::CalcTextSize("0000-00-00 00:00").x + ImGui::CalcTextSize(tr("Modified")).x + 24 * dpi;
            const f32 editWidth = std::max(100.0f * dpi, rowRight - ImGui::GetWindowPos().x - textX - datesWidth);
            const char* lockedTip = e.engineSample ? tr("The engine's sample project cannot be renamed or moved") : nullptr;
            const bool canEdit = e.valid && !e.engineSample;

            ImGui::SetCursorPosY(rowStart.y + style.FramePadding.y * 1.5f);
            ImGui::PushFont(nullptr, nameSize);
            if (editing == Edit::Name && editDirectory == e.directory) {
                const int r = inlineEdit("##name", editBuffer, sizeof(editBuffer), editWidth, editFocus);
                if (r == 1) {
                    if (!validGameName(editBuffer)) {
                        showError(tr("The name must start with a letter and use only letters, digits and _"));
                    } else if (e.info.name() != editBuffer) {
                        confirmKind = Edit::Name;
                        confirmDirectory = e.directory;
                        confirmValue = editBuffer;
                    }
                }
                if (r != 0) editing = Edit::None;
            } else if (clickableText("##nameText", displayName.c_str(),
                                     ImGui::GetColorU32(e.valid ? ImGuiCol_Text : ImGuiCol_TextDisabled), canEdit && !busy,
                                     lockedTip)) {
                editing = Edit::Name;
                editDirectory = e.directory;
                strncpy_s(editBuffer, e.info.name().c_str(), _TRUNCATE);
                editFocus = true;
            }
            ImGui::PopFont();

            ImGui::SetCursorPos(ImVec2(textX, rowStart.y + rowHeight - ImGui::GetTextLineHeight() - style.FramePadding.y * 2.0f));
            if (editing == Edit::Path && editDirectory == e.directory) {
                const int r = inlineEdit("##path", editBuffer, sizeof(editBuffer), editWidth, editFocus);
                if (r == 1 && !sameProjectDirectory(editBuffer, e.directory) && editBuffer[0]) {
                    confirmKind = Edit::Path;
                    confirmDirectory = e.directory;
                    confirmValue = editBuffer;
                }
                if (r != 0) editing = Edit::None;
            } else if (clickableText("##pathText", e.directory.c_str(), ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                     canEdit && !busy, lockedTip)) {
                editing = Edit::Path;
                editDirectory = e.directory;
                strncpy_s(editBuffer, e.directory.c_str(), _TRUNCATE);
                editFocus = true;
            }

            // Right: development start date, last modified date (dimmed)
            if (e.valid) {
                const std::string createdLine = std::string(tr("Started")) + "  " + e.created;
                const std::string modifiedLine = std::string(tr("Modified")) + "  " + e.modified;
                const f32 right = rowRight - ImGui::GetWindowPos().x + ImGui::GetScrollX() - style.FramePadding.x * 2;
                const f32 lineGap = ImGui::GetTextLineHeightWithSpacing();
                const f32 top = rowStart.y + (rowHeight - lineGap - ImGui::GetTextLineHeight()) * 0.5f;
                ImGui::SetCursorPos(ImVec2(right - ImGui::CalcTextSize(createdLine.c_str()).x, top));
                ImGui::TextDisabled("%s", createdLine.c_str());
                ImGui::SetCursorPos(ImVec2(right - ImGui::CalcTextSize(modifiedLine.c_str()).x, top + lineGap));
                ImGui::TextDisabled("%s", modifiedLine.c_str());
            } else {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", tr("Folder not found"));
            }
            ImGui::EndDisabled();

            ImGui::SetCursorPos(ImVec2(rowStart.x, rowStart.y + rowHeight));
            ImGui::Dummy(ImVec2(0, 0)); // next line position (SetCursorPos alone doesn't grow the window)
            ImGui::PopID();
        }
        if (entries.empty()) ImGui::TextDisabled("%s", tr("No projects yet. Create one or import a project folder."));
        else if (shown == 0) ImGui::TextDisabled("%s", tr("No project matches the filter."));
        ImGui::EndChild();

        if (iconToChange) changeIcon(*iconToChange);
        if (!removeDir.empty()) {
            removeRecentProject(removeDir);
            reload();
        }

        if (!message.empty()) {
            if (messageIsError) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", message.c_str());
            else ImGui::TextUnformatted(message.c_str());
            if (!taskLog.empty()) {
                ImGui::SameLine();
                if (ImGui::SmallButton(showLog ? tr("Hide log") : tr("Show log"))) showLog = !showLog;
            }
        }
        if (showLog && !taskLog.empty()) {
            ImGui::BeginChild("##tasklog", ImVec2(0, 160.0f * dpi), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(taskLog.c_str());
            ImGui::EndChild();
        }

        // ---- Confirmation window: rename / move folder
        if (confirmKind != Edit::None && !ImGui::IsPopupOpen(trl("Confirm"))) ImGui::OpenPopup(trl("Confirm"));
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::BeginPopupModal(trl("Confirm"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (confirmKind == Edit::Name) {
                ImGui::TextUnformatted(trf("Rename the game to \"%s\"?", confirmValue.c_str()).c_str());
                ImGui::TextDisabled("%s", tr("The exe, CMake target and game DLL names change. The build folder is removed\n"
                                              "and the game code is rebuilt when the project opens."));
            } else {
                ImGui::TextUnformatted(trf("Move the project folder to \"%s\"?", confirmValue.c_str()).c_str());
                ImGui::TextDisabled("%s", tr("The build folder is removed (it holds absolute paths) and rebuilt when the project opens.\n"
                                              "Close any editor or program using this project first."));
            }
            ImGui::Spacing();
            if (ImGui::Button(tr("OK"), ImVec2(120 * dpi, 0))) {
                if (confirmKind == Edit::Name) {
                    startTask(Task::Rename, "rename_project.ps1",
                              "-Project \"" + confirmDirectory + "\" -Name " + confirmValue, confirmDirectory);
                    showLog = false;
                } else {
                    std::string error;
                    ProjectInfo info;
                    // Moving can change the folder creation time, so write the start date first if it isn't recorded yet
                    if (loadProject(confirmDirectory, info) && info.settings.created.empty()) {
                        info.settings.created = formatFileTime(fileCreationTime(confirmDirectory), false);
                        game::saveProjectFile(info.filePath(), info);
                    }
                    if (moveProjectFolder(confirmDirectory, confirmValue, error)) {
                        replaceProjectDirectory(confirmDirectory, confirmValue);
                        message = trf("Moved to %s", confirmValue.c_str());
                        messageIsError = false;
                        reload();
                        ProjectInfo moved;
                        if (loadProject(confirmValue, moved)) selected = moved.directory;
                        scrollToSelected = true;
                    } else {
                        showError(error);
                    }
                }
                confirmKind = Edit::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"), ImVec2(120 * dpi, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                confirmKind = Edit::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ---- New project window
        if (openCreate) {
            ImGui::OpenPopup(trl("Create New Project"));
            openCreate = false;
        }
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(880 * dpi, 0), ImGuiCond_Appearing);
        if (ImGui::BeginPopupModal(trl("Create New Project"), nullptr)) {
            ImGui::BeginDisabled(busy);
            // Template cards (preview + name) in a row, the chosen one's description below
            {
                const f32 gap = style.ItemSpacing.x;
                const f32 cardWidth = (ImGui::GetContentRegionAvail().x - gap * (kTemplateCount - 1)) / kTemplateCount;
                const f32 imageHeight = cardWidth * 0.5625f; // 16:9
                const f32 cardHeight = imageHeight + ImGui::GetTextLineHeight() * 2.0f;
                const f32 rounding = 6.0f * dpi;
                ImDrawList* draw = ImGui::GetWindowDrawList();
                for (int i = 0; i < kTemplateCount; ++i) {
                    if (i > 0) ImGui::SameLine(0, gap);
                    ImGui::PushID(i);
                    const ImVec2 at = ImGui::GetCursorScreenPos();
                    if (ImGui::InvisibleButton("##card", ImVec2(cardWidth, cardHeight))) newTemplate = i;
                    const bool hovered = ImGui::IsItemHovered();
                    const ImVec2 end(at.x + cardWidth, at.y + cardHeight);
                    const ImVec2 imageEnd(end.x, at.y + imageHeight);
                    draw->AddRectFilled(at, end, ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg), rounding);
                    if (ImTextureData* tex = previews[i]) { // fill 16:9, center cropped
                        const f32 aspect = static_cast<f32>(tex->Width) / static_cast<f32>(std::max(tex->Height, 1));
                        const f32 want = cardWidth / imageHeight;
                        ImVec2 uv0(0, 0), uv1(1, 1);
                        if (aspect > want) {
                            uv0.x = 0.5f - 0.5f * want / aspect;
                            uv1.x = 1.0f - uv0.x;
                        } else {
                            uv0.y = 0.5f - 0.5f * aspect / want;
                            uv1.y = 1.0f - uv0.y;
                        }
                        draw->AddImageRounded(tex->GetTexRef(), at, imageEnd, uv0, uv1, IM_COL32_WHITE, rounding,
                                              ImDrawFlags_RoundCornersTop);
                    } else {
                        draw->AddRectFilled(at, imageEnd, ImGui::GetColorU32(ImGuiCol_WindowBg), rounding, ImDrawFlags_RoundCornersTop);
                    }
                    const char* label = tr(kTemplates[i].label);
                    ImGui::PushClipRect(at, end, true);
                    draw->AddText(ImVec2(at.x + style.FramePadding.x, imageEnd.y + ImGui::GetTextLineHeight() * 0.5f),
                                  ImGui::GetColorU32(ImGuiCol_Text), label);
                    ImGui::PopClipRect();
                    if (newTemplate == i) draw->AddRect(at, end, ImGui::GetColorU32(ImGuiCol_CheckMark), rounding, 0, 2.0f * dpi);
                    if (hovered) ImGui::SetTooltip("%s", label);
                    ImGui::PopID();
                }
            }
            ImGui::Spacing();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("%s", tr(kTemplates[newTemplate].description));
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::TextUnformatted(tr("Name (exe name)"));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##name", newName, sizeof(newName));
            ImGui::TextUnformatted(tr("Location"));
            const char* browse = tr("Browse...");
            ImGui::SetNextItemWidth(-ImGui::CalcTextSize(browse).x - style.FramePadding.x * 2 - style.ItemSpacing.x);
            ImGui::InputText("##location", newLocation, sizeof(newLocation));
            ImGui::SameLine();
            if (ImGui::Button(browse)) {
                const std::string dir = pickFolderDialog(window.nativeHandle(), newLocation);
                if (!dir.empty()) strncpy_s(newLocation, dir.c_str(), _TRUNCATE);
            }
            const bool nameOk = validGameName(newName);
            if (!nameOk)
                ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s",
                                   tr("The name must start with a letter and use only letters, digits and _"));
            else ImGui::TextDisabled("%s\\%s", newLocation, newName);
            ImGui::Spacing();
            ImGui::BeginDisabled(!nameOk || newLocation[0] == '\0');
            if (ImGui::Button(tr("Create"), ImVec2(120 * dpi, 0))) {
                startTask(Task::Create, "new_project.ps1",
                          std::string("-Name ") + newName + " -Path \"" + newLocation + "\" -Template " + kTemplates[newTemplate].script,
                          std::string(newLocation) + "\\" + newName);
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button(tr("Cancel"), ImVec2(120 * dpi, 0))) ImGui::CloseCurrentPopup();
            ImGui::EndDisabled();
            if (task == Task::Create) ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.0f), "%s", tr("Creating..."));
            if (!taskLog.empty()) {
                ImGui::BeginChild("##log", ImVec2(0, 200 * dpi), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
                ImGui::TextUnformatted(taskLog.c_str());
                ImGui::EndChild();
            }
            ImGui::EndPopup();
        }
        if (task == Task::Rename) {
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 200 * dpi, ImGui::GetWindowHeight() - ImGui::GetFrameHeight()));
            ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.0f), "%s", tr("Renaming..."));
        }
        ImGui::End();

        render::FrameParams frame;
        frame.viewport = {0.0f, 0.0f, static_cast<f32>(window.width()), static_cast<f32>(window.height())};
        frame.fogColor = {0.08f, 0.09f, 0.10f};
        const bool last = maxFrames && ++frameCount >= maxFrames;
        if (last && !capturePath.empty()) renderer.requestCapture(capturePath.c_str());
        const bool active = !window.minimized() && renderer.beginFrame(frame);
        imgui.render(renderer, active);
        if (active) renderer.endFrame();
        if (last) break;
    }

    for (ImTextureData* tex : previews) imgui.releaseTexture(tex);
    imgui.shutdown();
    if (!chosen.empty()) addRecentProject(chosen);
    return chosen;
}

} // namespace mealkit::editor
