#include "mealkit/editor/ProjectManager.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Process.h"
#include "mealkit/platform/Window.h"
#include "mealkit/render/Renderer.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>
#include <shobjidl.h>

namespace mealkit::editor {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string narrow(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// 폴더 고르기 대화상자 (취소하면 빈 문자열)
std::string pickFolder(void* owner, const std::string& initial) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::string result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (!initial.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(widen(initial).c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder);
            folder->Release();
        }
    }
    if (SUCCEEDED(dialog->Show(static_cast<HWND>(owner)))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = narrow(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

std::string parentDirectory(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool validGameName(const char* name) {
    if (!std::isalpha(static_cast<unsigned char>(name[0]))) return false;
    for (const char* c = name; *c; ++c)
        if (!std::isalnum(static_cast<unsigned char>(*c)) && *c != '_') return false;
    return true;
}

struct RecentEntry {
    std::string directory;
    ProjectInfo info;
    bool valid = false;
};

std::vector<RecentEntry> readRecent() {
    std::vector<RecentEntry> list;
    for (const std::string& dir : loadRecentProjects()) {
        RecentEntry e;
        e.directory = dir;
        e.valid = loadProject(dir, e.info);
        list.push_back(std::move(e));
    }
    return list;
}

} // namespace

std::string runProjectManager(Window& window, render::Renderer& renderer, const std::string& engineDirectory,
                              unsigned maxFrames, const std::string& capturePath) {
    std::string engineDir = engineDirectory;
    std::replace(engineDir.begin(), engineDir.end(), '/', '\\'); // CMake 경로 → 윈도우 표기
    ImGuiLayer imgui;
    if (!imgui.init(window, renderer, executableDirectory() + "\\mealkit_projects.ini")) return {};
    window.setTitle("MealKit - 프로젝트");

    std::vector<RecentEntry> recent = readRecent();
    std::string chosen;
    std::string message;
    bool messageIsError = false;

    char newName[64] = "MyGame";
    char newLocation[512] = {};
    {
        // 새 프로젝트 기본 위치: 최근 프로젝트가 있는 폴더, 없으면 문서\MealKitProjects
        std::string location;
        if (!recent.empty()) location = parentDirectory(recent.front().directory);
        if (location.empty()) {
            wchar_t profile[MAX_PATH];
            const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH);
            if (n > 0 && n < MAX_PATH) location = narrow(profile) + "\\Documents\\MealKitProjects";
        }
        strncpy_s(newLocation, location.c_str(), _TRUNCATE);
    }
    ChildProcess creator;
    std::string creatorLog;
    std::string creating; // 만드는 중인 프로젝트 폴더

    auto openProject = [&](const std::string& dir) {
        if (!isProjectDirectory(dir)) {
            message = "프로젝트 폴더가 아닙니다 (mealkit.project 없음): " + dir;
            messageIsError = true;
            return;
        }
        chosen = dir;
    };

    unsigned frameCount = 0;
    while (chosen.empty() && window.pumpMessages()) {
        if (window.consumeResize()) renderer.resize(window.width(), window.height());

        if (creator.running() && !creator.poll(creatorLog)) {
            if (creator.exitCode() == 0 && isProjectDirectory(creating)) {
                addRecentProject(creating);
                chosen = creating;
            } else {
                message = "프로젝트를 만들지 못했습니다 (아래 로그)";
                messageIsError = true;
            }
        }

        imgui.newFrame();
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##projects", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::SetWindowFontScale(1.4f);
        ImGui::TextUnformatted("MealKit 프로젝트");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextDisabled("엔진: %s", engineDir.c_str());
        ImGui::Separator();

        const f32 rightWidth = std::min(420.0f * ImGui::GetStyle().FontScaleDpi, ImGui::GetContentRegionAvail().x * 0.45f);
        const f32 bottom = message.empty() ? 0.0f : ImGui::GetFrameHeightWithSpacing();

        // ---- 최근 프로젝트
        ImGui::BeginChild("##recent", ImVec2(-rightWidth - ImGui::GetStyle().ItemSpacing.x, -bottom), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted("최근 프로젝트");
        ImGui::Separator();
        if (recent.empty()) ImGui::TextDisabled("아직 없습니다. 오른쪽에서 프로젝트를 열거나 새로 만드세요.");
        std::string removeDir;
        for (size_t i = 0; i < recent.size(); ++i) {
            const RecentEntry& e = recent[i];
            ImGui::PushID(static_cast<int>(i));
            std::string label = e.valid ? e.info.name : "(찾을 수 없음)";
            if (e.valid && _stricmp(e.directory.c_str(), engineDir.c_str()) == 0) label += "  (엔진 예제)";
            ImGui::BeginDisabled(!e.valid || creator.running());
            if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_AllowDoubleClick,
                                  ImVec2(0, ImGui::GetTextLineHeight() * 2.2f)) &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                openProject(e.directory);
            ImGui::EndDisabled();
            if (ImGui::BeginPopupContextItem()) {
                if (ImGui::MenuItem("열기", nullptr, false, e.valid)) openProject(e.directory);
                if (ImGui::MenuItem("탐색기에서 보기", nullptr, false, e.valid)) openInExplorer(e.directory);
                if (ImGui::MenuItem("목록에서 제거")) removeDir = e.directory;
                ImGui::EndPopup();
            }
            // 이름 아래에 경로 (Selectable 높이 안쪽에 겹쳐 그린다)
            const ImVec2 min = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(ImVec2(min.x, min.y + ImGui::GetTextLineHeight() * 1.1f),
                                                ImGui::GetColorU32(ImGuiCol_TextDisabled), e.directory.c_str());
            ImGui::PopID();
        }
        if (!removeDir.empty()) {
            removeRecentProject(removeDir);
            recent = readRecent();
        }
        ImGui::TextDisabled("더블클릭으로 열기, 오른쪽 클릭으로 메뉴");
        ImGui::EndChild();
        ImGui::SameLine();

        // ---- 열기 / 새 프로젝트
        ImGui::BeginChild("##actions", ImVec2(0, -bottom), ImGuiChildFlags_Borders);
        ImGui::BeginDisabled(creator.running());
        ImGui::TextUnformatted("열기");
        ImGui::Separator();
        if (ImGui::Button("프로젝트 폴더 열기...", ImVec2(-1, 0))) {
            const std::string dir = pickFolder(window.nativeHandle(), newLocation);
            if (!dir.empty()) openProject(dir);
        }
        ImGui::Spacing();
        ImGui::Spacing();
        ImGui::TextUnformatted("새 프로젝트");
        ImGui::Separator();
        ImGui::TextDisabled("예제 게임(아레나)을 복사해 시작합니다.");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##name", "이름 (영문, exe 이름)", newName, sizeof(newName));
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize("찾아보기...").x - ImGui::GetStyle().FramePadding.x * 2 -
                                ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##location", newLocation, sizeof(newLocation));
        ImGui::SameLine();
        if (ImGui::Button("찾아보기...")) {
            const std::string dir = pickFolder(window.nativeHandle(), newLocation);
            if (!dir.empty()) strncpy_s(newLocation, dir.c_str(), _TRUNCATE);
        }
        const bool nameOk = validGameName(newName);
        if (!nameOk) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "영문자로 시작하고 영문·숫자·_만 씁니다");
        else ImGui::TextDisabled("%s\\%s", newLocation, newName);
        ImGui::BeginDisabled(!nameOk || newLocation[0] == '\0');
        if (ImGui::Button("만들기", ImVec2(-1, 0))) {
            creating = std::string(newLocation) + "\\" + newName;
            creatorLog.clear();
            message.clear();
            const std::string script = engineDir + "\\tools\\new_project.ps1";
            const std::string command = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" + script + "\" -Name " +
                                        newName + " -Path \"" + newLocation + "\"";
            if (!creator.start(command, engineDir)) {
                message = "new_project.ps1을 실행하지 못했습니다";
                messageIsError = true;
            }
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (creator.running()) ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.35f, 1.0f), "만드는 중...");
        if (!creatorLog.empty()) {
            ImGui::BeginChild("##log", ImVec2(0, 0), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(creatorLog.c_str());
            ImGui::EndChild();
        }
        ImGui::EndChild();

        if (!message.empty()) {
            if (messageIsError) ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f), "%s", message.c_str());
            else ImGui::TextUnformatted(message.c_str());
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

    imgui.shutdown();
    if (!chosen.empty()) addRecentProject(chosen);
    return chosen;
}

} // namespace mealkit::editor
