// Heightmap terrain editing (scene kind terrain, asset/Terrain.h): new terrains, PNG import, height brushes (raise/lower/flatten/smooth).
// Brushes change the cached heightmap in place (asset::ModelCache::terrain); scene save writes edited heightmaps back to their PNG files.
#include "mealkit/editor/Editor.h"

#include "mealkit/render/ModelDraw.h" // asset::ModelCache + its drawing
#include "mealkit/editor/FieldEditor.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/FileDialog.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/Window.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>

namespace mealkit::editor {
using scene::Entity;

namespace {

constexpr u32 kNewTerrainSamples = 129;  // 128 cells: 1 m cells on the default 128 m terrain
constexpr u16 kNewTerrainLevel = 16384;  // a quarter up, so brushes can also dig down
const FileFilter kPngFilters[] = {{"Heightmap (*.png)", "*.png"}, {"All files (*.*)", "*.*"}};

} // namespace

asset::Terrain* Editor::terrainOf(const scene::Entity& e) const {
    return e.is(scene::kTypeTerrain) ? m_models->terrain(scene::terrainFile(e)) : nullptr;
}

void Editor::setupNewTerrain(scene::Entity& e) {
    std::string stem = m_scenePath.empty() ? std::string("untitled") : fileName(m_scenePath);
    if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem.resize(dot);
    const std::string path = scene::terrainFileFor(stem, e);
    std::erase_if(e.fields, [](const auto& f) { return f.first == "heightmap"; });
    e.fields.emplace_back("heightmap", path);
    e.scale = {128.0f, 32.0f, 128.0f};
    // The flat surface sits at the old floor height (box bottom + a quarter)
    e.position = {e.position.x, 16.0f - 8.0f, e.position.z};
    e.color = 0xFF6E9C76;
    m_models->setTerrain(path, asset::flatHeightmap(kNewTerrainSamples, kNewTerrainLevel), true);
}

void Editor::drawTerrainInspector(scene::Entity& e) {
    if (!sectionHeader(tr("Terrain"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    const std::string file = scene::terrainFile(e);
    asset::Terrain* terrain = terrainOf(e);
    if (terrain) {
        ImGui::TextDisabled(tr("%u x %u samples · %.2f m cells%s"), terrain->map.size, terrain->map.size,
                            e.scale.x / static_cast<f32>(terrain->map.size - 1), terrain->dirty ? tr(" · not saved") : "");
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s", file.empty() ? tr("No heightmap") : trf("Cannot read %s", file.c_str()).c_str());
    }
    if (ImGui::Button(tr("Import PNG..."))) {
        const std::string root = asset::ModelCache::assetsRoot();
        const std::string picked = openFileDialog(m_window->nativeHandle(), "", root, kPngFilters, 2);
        const std::string relative = picked.empty() ? std::string() : relativeTo(root, picked);
        asset::Heightmap map;
        std::string error;
        if (!picked.empty() && relative.empty()) {
            setStatus(tr("The file is outside the assets folder. In multiplayer everyone needs it at the same path"), true);
        } else if (!relative.empty() && !asset::loadHeightmap(picked, map, error)) {
            setStatus(trf("Could not read the heightmap: %s", error.c_str()), true);
        } else if (!relative.empty()) {
            pushUndo(capture());
            std::erase_if(e.fields, [](const auto& f) { return f.first == "heightmap"; });
            e.fields.emplace_back("heightmap", relative);
            m_models->setTerrain(relative, std::move(map), false);
            setStatus(trf("Heightmap: %s", relative.c_str()));
        }
    }
    if (!terrain) return;
    ImGui::SameLine();
    ImGui::Checkbox(tr("Brush"), &m_brushOn);
    ImGui::BeginDisabled(!m_brushOn);
    const char* names[] = {tr("Raise"), tr("Lower"), tr("Flatten"), tr("Smooth")};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(names[i], m_brush == i)) m_brush = i;
    }
    ImGui::SliderFloat(propertyRow(tr("Radius (m)")).c_str(), &m_brushRadius, 0.5f, 64.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat(propertyRow(tr("Strength")).c_str(), &m_brushStrength, 0.01f, 1.0f, "%.2f");
    ImGui::TextDisabled("%s", tr("Left drag paints · Flatten keeps the height under the stroke start · Ctrl+Z undoes a stroke"));
    ImGui::EndDisabled();
}

// Brush stroke in the viewport while a terrain is selected and the brush is on. true = the mouse belongs to the brush this frame
bool Editor::handleTerrainBrush() {
    Entity* e = selected();
    m_brushHit = false;
    if (!m_brushOn || !e || !m_multi.empty()) {
        m_brushStroke = false;
        return false;
    }
    if (e->is(scene::kTypeScatter)) return handleScatterBrush(*e);
    asset::Terrain* terrain = terrainOf(*e);
    if (!terrain) return false;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool inViewport = mouse.x >= m_viewport.x && mouse.y >= m_viewport.y && mouse.x < m_viewport.x + m_viewport.width &&
                            mouse.y < m_viewport.y + m_viewport.height;
    const scene::Transform w = scene::worldTransform(m_scene, *e);
    Vec3 origin, dir;
    mouseRay(mouse.x, mouse.y, &origin, &dir);
    const f32 t = inViewport ? asset::raycastTerrain(terrain->map, w.position, w.scale, origin, dir, 10000.0f) : -1.0f;
    if (t >= 0.0f) {
        m_brushHit = true;
        m_brushPoint = origin + dir * t;
    }
    if (m_brushStroke && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_brushStroke = false;
    if (!m_brushStroke && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_brushHit && !sceneMouseBlocked() && !io.KeyAlt) {
        m_brushStroke = true;
        pushUndo(capture());
        const f32 bottom = w.position.y - w.scale.y * 0.5f;
        m_flattenTo = static_cast<u16>(std::lround(std::clamp((m_brushPoint.y - bottom) / std::max(w.scale.y, 0.001f), 0.0f, 1.0f) * 65535.0f));
    }
    if (m_brushStroke && m_brushHit) {
        const f32 dt = std::min(io.DeltaTime, 0.1f);
        const auto brush = static_cast<asset::TerrainBrush>(m_brush);
        // Raise/Lower: strength 1 = a fifth of the box height per second at the center
        const f32 amount = (brush == asset::TerrainBrush::Raise || brush == asset::TerrainBrush::Lower ? 0.2f : 6.0f) *
                           m_brushStrength * dt;
        const asset::SampleRect rect =
            asset::applyBrush(terrain->map, w.position, w.scale, brush, m_brushPoint, m_brushRadius, amount, m_flattenTo);
        if (!rect.empty) {
            m_models->terrainChanged(*terrain, rect);
            m_dirty = true;
        }
    }
    if (m_brushHit) { // brush outline on the surface
        const Mat4 viewProj = m_view * m_proj;
        auto screen = [&](Vec3 p, ImVec2* out) {
            const f32 cx = p.x * viewProj.m[0][0] + p.y * viewProj.m[1][0] + p.z * viewProj.m[2][0] + viewProj.m[3][0];
            const f32 cy = p.x * viewProj.m[0][1] + p.y * viewProj.m[1][1] + p.z * viewProj.m[2][1] + viewProj.m[3][1];
            const f32 cw = p.x * viewProj.m[0][3] + p.y * viewProj.m[1][3] + p.z * viewProj.m[2][3] + viewProj.m[3][3];
            if (cw <= 0.0f) return false;
            *out = {m_viewport.x + (cx / cw * 0.5f + 0.5f) * m_viewport.width, m_viewport.y + (0.5f - cy / cw * 0.5f) * m_viewport.height};
            return true;
        };
        ImVec2 points[48];
        int count = 0;
        for (int i = 0; i < 48; ++i) {
            const f32 a = static_cast<f32>(i) * (6.2831853f / 48.0f);
            Vec3 p = m_brushPoint + Vec3{std::cos(a) * m_brushRadius, 0.0f, std::sin(a) * m_brushRadius};
            f32 y = p.y;
            if (asset::terrainHeightAt(terrain->map, w.position, w.scale, p.x, p.z, &y)) p.y = y;
            if (screen(p, &points[count])) ++count;
        }
        if (count > 2) ImGui::GetForegroundDrawList()->AddPolyline(points, count, IM_COL32(255, 220, 90, 220), ImDrawFlags_Closed, 2.0f);
    }
    return inViewport && (m_brushStroke || m_brushHit);
}

void Editor::captureTerrains(Snapshot& s) const {
    for (const Entity& e : m_scene.entities) {
        if (const asset::Terrain* t = terrainOf(e)) s.terrains.push_back({t->path, t->map});
        if (e.is(scene::kTypeScatter)) {
            const std::string file = scene::readObject<scene::ScatterFields>(e).file;
            if (!file.empty()) s.scatters.push_back({file, m_models->scatter(file).scatter.items});
        }
    }
}

void Editor::restoreTerrains(const Snapshot& s) {
    for (const auto& [path, map] : s.terrains) {
        asset::Terrain* t = m_models->terrain(path);
        if (t && t->map.size == map.size && t->map.samples == map.samples) continue;
        m_models->setTerrain(path, map, true);
    }
    for (const auto& [path, items] : s.scatters) {
        asset::ModelCache::ScatterEntry& entry = m_models->scatter(path);
        if (entry.scatter.items.size() == items.size() &&
            std::equal(items.begin(), items.end(), entry.scatter.items.begin(), [](const auto& a, const auto& b) {
                return a.position.x == b.position.x && a.position.z == b.position.z && a.scale == b.scale;
            }))
            continue;
        entry.scatter.items = items;
        entry.scatter.grouped = false;
        entry.dirty = true;
    }
}

// ---------------------------------------------------------------- Scatter layers (asset/Scatter.h)

void Editor::setupNewScatter(Entity& e) {
    std::string stem = m_scenePath.empty() ? std::string("untitled") : fileName(m_scenePath);
    if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem.resize(dot);
    std::erase_if(e.fields, [](const auto& f) { return f.first == "file"; });
    e.fields.emplace_back("file", scene::scatterFileFor(stem, e));
    e.color = 0xFF5AB45A;
    m_brushOn = true;
}

void Editor::drawScatterInspector(Entity& e) {
    if (!sectionHeader(tr("Scatter"), ImGuiTreeNodeFlags_DefaultOpen)) return;
    const scene::ScatterFields f = scene::readObject<scene::ScatterFields>(e);
    if (f.file.empty()) {
        ImGui::TextDisabled("%s", tr("No instance file"));
        return;
    }
    asset::ModelCache::ScatterEntry& entry = m_models->scatter(f.file);
    ImGui::TextDisabled(tr("%zu instances%s"), entry.scatter.items.size(), entry.dirty ? tr(" · not saved") : "");
    ImGui::Checkbox(tr("Brush"), &m_brushOn);
    ImGui::SameLine();
    if (ImGui::Button(tr("Clear")) && !entry.scatter.items.empty()) {
        pushUndo(capture());
        entry.scatter.items.clear();
        entry.scatter.grouped = false;
        entry.dirty = true;
    }
    ImGui::BeginDisabled(!m_brushOn);
    if (ImGui::RadioButton(tr("Paint"), !m_scatterErase)) m_scatterErase = false;
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Erase"), m_scatterErase)) m_scatterErase = true;
    ImGui::SliderFloat(propertyRow(tr("Radius (m)")).c_str(), &m_brushRadius, 0.5f, 64.0f, "%.1f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat(propertyRow(tr("Strength")).c_str(), &m_brushStrength, 0.01f, 1.0f, "%.2f");
    ImGui::TextDisabled("%s", tr("Left drag paints onto terrain and floors (Shift erases) · density and sizes are fields below · Ctrl+Z undoes a stroke"));
    ImGui::EndDisabled();
}

bool Editor::handleScatterBrush(Entity& e) {
    const scene::ScatterFields f = scene::readObject<scene::ScatterFields>(e);
    if (f.file.empty()) return false;
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mouse = io.MousePos;
    const bool inViewport = mouse.x >= m_viewport.x && mouse.y >= m_viewport.y && mouse.x < m_viewport.x + m_viewport.width &&
                            mouse.y < m_viewport.y + m_viewport.height;
    Vec3 origin, dir;
    mouseRay(mouse.x, mouse.y, &origin, &dir);
    f32 distance = 0.0f;
    if (inViewport && raycast(origin, dir, &distance, {e.id}) && distance < 1e29f) {
        m_brushHit = true;
        m_brushPoint = origin + dir * distance;
    }
    if (m_brushStroke && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) m_brushStroke = false;
    if (!m_brushStroke && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_brushHit && !sceneMouseBlocked() && !io.KeyAlt) {
        m_brushStroke = true;
        pushUndo(capture());
    }
    asset::ModelCache::ScatterEntry& entry = m_models->scatter(f.file);
    if (m_brushStroke && m_brushHit) {
        const bool erase = m_scatterErase != io.KeyShift;
        if (erase) {
            if (asset::eraseScatter(entry.scatter, m_brushPoint, m_brushRadius)) entry.dirty = m_dirty = true;
        } else {
            asset::ScatterPaint p;
            p.center = m_brushPoint;
            p.radius = m_brushRadius;
            p.density = f.density;
            p.minScale = f.minScale;
            p.maxScale = f.maxScale;
            p.randomYaw = f.randomYaw;
            // Strength 1 fills about the whole circle in half a second
            const f32 area = 3.14159265f * m_brushRadius * m_brushRadius * std::max(f.density, 0.0f);
            const u32 maxAdd = static_cast<u32>(std::ceil(area * m_brushStrength * std::min(io.DeltaTime, 0.1f) * 2.0f));
            auto random01 = [this] { // editor-only randomness (not simulation)
                m_scatterSeed = m_scatterSeed * 6364136223846793005ull + 1442695040888963407ull;
                return static_cast<f32>(m_scatterSeed >> 40) / 16777216.0f;
            };
            const f32 top = m_brushPoint.y + m_brushRadius + 50.0f;
            auto floorAt = [&](f32 x, f32 z, f32* y) {
                f32 d = 0.0f;
                if (!raycast({x, top, z}, {0.0f, -1.0f, 0.0f}, &d, {e.id}) || d > 1e29f) return false;
                *y = top - d;
                return true;
            };
            if (asset::paintScatter(entry.scatter, p, maxAdd, floorAt, random01)) entry.dirty = m_dirty = true;
        }
    }
    if (m_brushHit) { // brush outline (flat circle at the hit height)
        const Mat4 viewProj = m_view * m_proj;
        ImVec2 points[48];
        int count = 0;
        for (int i = 0; i < 48; ++i) {
            const f32 a = static_cast<f32>(i) * (6.2831853f / 48.0f);
            const Vec3 p = m_brushPoint + Vec3{std::cos(a) * m_brushRadius, 0.0f, std::sin(a) * m_brushRadius};
            const f32 cx = p.x * viewProj.m[0][0] + p.y * viewProj.m[1][0] + p.z * viewProj.m[2][0] + viewProj.m[3][0];
            const f32 cy = p.x * viewProj.m[0][1] + p.y * viewProj.m[1][1] + p.z * viewProj.m[2][1] + viewProj.m[3][1];
            const f32 cw = p.x * viewProj.m[0][3] + p.y * viewProj.m[1][3] + p.z * viewProj.m[2][3] + viewProj.m[3][3];
            if (cw <= 0.0f) continue;
            points[count++] = {m_viewport.x + (cx / cw * 0.5f + 0.5f) * m_viewport.width, m_viewport.y + (0.5f - cy / cw * 0.5f) * m_viewport.height};
        }
        const bool erase = m_scatterErase != io.KeyShift;
        if (count > 2)
            ImGui::GetForegroundDrawList()->AddPolyline(points, count, erase ? IM_COL32(255, 110, 90, 220) : IM_COL32(120, 230, 120, 220),
                                                        ImDrawFlags_Closed, 2.0f);
    }
    return inViewport && (m_brushStroke || m_brushHit);
}

} // namespace mealkit::editor
