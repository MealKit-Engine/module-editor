#include "mealkit/editor/AssetThumbnails.h"

#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cmath>

#include <imgui.h>

namespace mealkit::editor {
namespace {

constexpr f32 kFovY = 30.0f * 3.14159265f / 180.0f;

std::string nativeAssetPath(std::string relative) {
    std::replace(relative.begin(), relative.end(), '/', '\\');
    return asset::ModelCache::assetsRoot() + "\\" + relative;
}

} // namespace

unsigned long long AssetThumbnails::texture(const std::string& model, render::Renderer* renderer) {
    auto it = m_entries.find(model);
    if (it == m_entries.end()) {
        if (m_entries.size() < kLimit) m_entries.emplace(model, Entry{});
        return 0;
    }
    Entry& entry = it->second;
    const double now = ImGui::GetTime();
    if (now - entry.checked >= 1.0) { // redraw after the file changes
        entry.checked = now;
        const unsigned long long time = fileWriteTime(nativeAssetPath(model));
        if (time != entry.fileTime) {
            entry.fileTime = time;
            entry.drawn = false;
        }
    }
    if (!renderer || !entry.target || !entry.drawn) return 0;
    const rhi::Texture target = renderer->renderTargetTexture(entry.target);
    return target ? static_cast<unsigned long long>(rhi::imguiTextureId(renderer->device(), target)) : 0;
}

void AssetThumbnails::render(render::Renderer& renderer, asset::ModelCache& models) {
    for (auto& [path, entry] : m_entries) {
        if (entry.drawn) continue;
        if (!entry.target) entry.target = renderer.createRenderTarget(kSize, kSize);
        if (!entry.target) return;
        entry.drawn = true;
        const asset::Model* model = models.get(path);
        const Vec3 lo = model ? model->boundsMin : Vec3{-0.5f, 0.0f, -0.5f}, hi = model ? model->boundsMax : Vec3{0.5f, 1.0f, 0.5f};
        const Vec3 size = hi - lo;
        const f32 radius = std::max(0.5f * std::sqrt(size.x * size.x + size.y * size.y + size.z * size.z), 0.05f);
        const Vec3 center{0.0f, 0.5f * size.y, 0.0f};
        const f32 distance = radius / std::sin(kFovY * 0.5f) * 1.1f;
        const Vec3 eye = center + normalize(Vec3{0.6f, 0.45f, -1.0f}) * distance;

        render::FrameParams frame;
        frame.viewProj = lookAtLH(eye, center, {0, 1, 0}) * perspectiveReversedZ(kFovY, 1.0f, distance * 0.02f, distance * 4.0f);
        frame.cameraPos = eye;
        frame.cameraForward = normalize(center - eye);
        frame.sunDirection = {0.45f, 0.75f, -0.5f};
        frame.sunColor = {1.0f, 1.0f, 1.0f};
        frame.skyColor = {0.32f, 0.32f, 0.34f};
        frame.groundColor = {0.16f, 0.16f, 0.16f};
        frame.fogColor = {0.16f, 0.17f, 0.19f};
        frame.fogDensity = 0.0f;
        m_drawList.clear();
        if (model && model->lodCount) {
            const Vec3 base{-(lo.x + hi.x) * 0.5f, -lo.y, -(lo.z + hi.z) * 0.5f};
            m_drawList.add(model->lods[0], {base, 0xFFFFFFFF, Quat{}, Vec3{1, 1, 1}, 0});
        }
        renderer.drawToTarget(entry.target, frame, m_drawList, render::LightingModel::Pbr);
        return; // one per frame
    }
}

} // namespace mealkit::editor
