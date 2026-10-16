#pragma once

#include "mealkit/core/Types.h"

#include <string>
#include <vector>

struct ImTextureData;

namespace mealkit {
class Window;
namespace render {
class Renderer;
}
} // namespace mealkit

namespace mealkit::editor {

// 에디터 창의 제목 표시줄 색 (0xRRGGBB). ImGui 메뉴 막대 색(applyStyle의 MenuBarBg)과 맞춘다
constexpr u32 kEditorCaptionColor = 0x1A1C1F;

// 아이콘 글자 (Windows 기본 글꼴 Segoe Fluent Icons / Segoe MDL2 Assets를 기본 글꼴에 합친다). 글자와 섞어 쓴다: "\xEE\x9C\x90 New"
namespace icon {
inline constexpr const char* kAdd = "\xEE\x9C\x90";        // U+E710
inline constexpr const char* kFolder = "\xEE\xA2\xB7";     // U+E8B7
inline constexpr const char* kSearch = "\xEE\x9C\xA1";     // U+E721
inline constexpr const char* kGlobe = "\xEE\x9D\xB4";      // U+E774
inline constexpr const char* kStar = "\xEE\x9C\xB4";       // U+E734 (빈 별)
inline constexpr const char* kStarFilled = "\xEE\x9C\xB5"; // U+E735
} // namespace icon

// Dear ImGui를 MealKit 창/렌더러에 연결한다 (Win32 + DX12 백엔드). 에디터 빌드 전용.
class ImGuiLayer {
public:
    ImGuiLayer() = default;
    ~ImGuiLayer();
    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    bool init(Window& window, render::Renderer& renderer, const std::string& iniPath);
    void shutdown();

    void newFrame();
    // 렌더러 프레임이 열려 있으면 UI를 그려 넣는다. 열려 있지 않아도 ImGui 프레임은 반드시 닫는다.
    void render(render::Renderer& renderer, bool frameActive);

    // ImGui::Image로 그릴 RGBA8 텍스처. ImGui 백엔드가 다음 프레임에 GPU에 올린다. 다 쓰면 releaseTexture
    ImTextureData* createTexture(const u8* rgba, u32 width, u32 height);
    void releaseTexture(ImTextureData* texture); // GPU가 다 쓴 뒤(몇 프레임 뒤) 지운다

private:
    bool m_initialized = false;
    Window* m_window = nullptr;
    render::Renderer* m_renderer = nullptr;
    std::string m_iniPath;
    void* m_srvHeap = nullptr; // ID3D12DescriptorHeap*
    u64 m_srvCpuStart = 0;
    u64 m_srvGpuStart = 0;
    u32 m_srvStride = 0;
    std::vector<u32> m_freeSlots;
    std::vector<ImTextureData*> m_textures; // createTexture로 만든 것 (지워질 때까지)

    friend struct ImGuiLayerAccess;
};

} // namespace mealkit::editor
