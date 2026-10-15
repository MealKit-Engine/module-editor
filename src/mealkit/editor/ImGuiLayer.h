#pragma once

#include "mealkit/core/Types.h"

#include <string>
#include <vector>

namespace mealkit {
class Window;
namespace render {
class Renderer;
}
} // namespace mealkit

namespace mealkit::editor {

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

    friend struct ImGuiLayerAccess;
};

} // namespace mealkit::editor
