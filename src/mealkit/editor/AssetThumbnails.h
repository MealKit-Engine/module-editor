#pragma once

#include "mealkit/render/ModelDraw.h" // asset::ModelCache + its drawing
#include "mealkit/render/Renderer.h"

#include <string>
#include <unordered_map>

// Small model pictures for the [Assets] grid: one model is drawn per frame to its own offscreen target and redrawn when its file changes.
// Kept in memory for the session, up to kLimit models; later models show their icon.
namespace mealkit::editor {

class AssetThumbnails {
public:
    // Texture for this model (relative to assets), or 0 while it isn't drawn yet. Asks for it to be drawn.
    unsigned long long texture(const std::string& model, render::Renderer* renderer);
    // Once per frame, before the UI is recorded
    void render(render::Renderer& renderer, asset::ModelCache& models);

private:
    struct Entry {
        rhi::RenderTarget target;
        unsigned long long fileTime = 0;
        double checked = 0.0;
        bool drawn = false;
    };
    static constexpr size_t kLimit = 128;
    static constexpr u32 kSize = 160;
    std::unordered_map<std::string, Entry> m_entries;
    render::DrawList m_drawList;
};

} // namespace mealkit::editor
