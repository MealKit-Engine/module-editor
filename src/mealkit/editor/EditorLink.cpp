// MkRender link (MealKit-Contract docs/CONTRACT.md "Scene service"): the MkRender scene editor finds this editor through the live connection
// and asks it to play the saved scene (optionally from its camera, solo), stop, look through a virtual camera or preview a cutscene.
// The editor's play view does the playing; MkRender only edits files.
#include "mealkit/editor/Editor.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/Strings.h"
#include "mealkit/platform/Paths.h"

#include <cstdlib>

namespace mealkit::editor {
namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') out += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && hexDigit(s[i + 1]) >= 0 && hexDigit(s[i + 2]) >= 0) {
            out += static_cast<char>(hexDigit(s[i + 1]) * 16 + hexDigit(s[i + 2]));
            i += 2;
        } else out += s[i];
    }
    return out;
}

// a=b&c=d form -> value of key ("" if missing)
std::string formValue(const std::string& body, const std::string& key) {
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t end = body.find('&', pos);
        if (end == std::string::npos) end = body.size();
        const std::string pair = body.substr(pos, end - pos);
        const size_t eq = pair.find('=');
        if (urlDecode(pair.substr(0, eq)) == key) return eq == std::string::npos ? std::string() : urlDecode(pair.substr(eq + 1));
        pos = end + 1;
    }
    return {};
}

live::Response reply(int status, const std::string& json) {
    live::Response r;
    r.status = status;
    r.body = json;
    return r;
}
live::Response fail(int status, const std::string& message) { return reply(status, "{\"ok\":false,\"error\":" + live::jsonString(message) + "}"); }

f32 number(const std::string& text) { return static_cast<f32>(std::strtod(text.c_str(), nullptr)); }

} // namespace

void Editor::addSceneLink() {
    live::Service s;
    s.name = "scene";
    s.handle = [this](const live::Request& req) { return handleSceneLink(req); };
    m_live.addService(std::move(s));
}

// The scene MkRender sent (relative to assets or absolute) as the open scene: loads it when it's another file, or reloads the
// saved file MkRender just wrote. Refuses while the editor holds unsaved edits of another scene. Empty = ok
std::string Editor::linkScene(const std::string& file) {
    if (file.empty()) return {};
    std::string path = file;
    for (char& c : path)
        if (c == '/') c = '\\';
    if (!(path.size() > 1 && path[1] == ':')) path = m_project.directory + "\\assets\\" + path;
    if (!fileExists(path)) return "no scene file " + file;
    if (m_dirty) return "the editor has unsaved changes in " + fileName(m_scenePath) + ": save or revert them there first";
    if (!loadScene(path)) return "cannot open " + file;
    return {};
}

live::Response Editor::handleSceneLink(const live::Request& req) {
    const char* mode = m_mode == Mode::Edit ? "edit" : "play";
    if (req.method == "GET" && req.path == "state") {
        std::string scene = m_scenePath;
        const std::string assets = m_project.directory + "\\assets\\";
        if (scene.rfind(assets, 0) == 0) scene = scene.substr(assets.size());
        for (char& c : scene)
            if (c == '\\') c = '/';
        return reply(200, std::string("{\"mode\":\"") + mode + "\",\"scene\":" + live::jsonString(scene) + ",\"dirty\":" + (m_dirty ? "true" : "false") +
                              ",\"game\":" + (m_gameLoaded ? "true" : "false") + ",\"players\":" + std::to_string(m_playPlayers) + "}");
    }
    if (req.method != "POST") return fail(404, "not found");
    if (req.path == "stop") {
        if (m_mode == Mode::Edit) {
            m_lookThrough = 0;
            m_cutscenePreview.clear();
            return reply(200, "{\"ok\":true,\"mode\":\"edit\"}");
        }
        m_stopRequest = true;
        return reply(200, "{\"ok\":true}");
    }
    if (m_mode != Mode::Edit) return fail(409, "the editor is playing (stop first)");
    if (req.path == "play") {
        if (!m_gameLoaded) return fail(409, "the game code isn't loaded in the editor yet (build it there)");
        if (const std::string error = linkScene(formValue(req.body, "file")); !error.empty()) return fail(409, error);
        const bool from = formValue(req.body, "from") == "1";
        if (from && m_playPlayers == 1) {
            m_cameraPos = {number(formValue(req.body, "x")), number(formValue(req.body, "y")), number(formValue(req.body, "z"))};
            m_cameraYaw = number(formValue(req.body, "yaw"));
            // A selected spawn point is used as is (moveSpawnsHere), anything else means the floor under the camera
            const u32 spawn = static_cast<u32>(std::strtoul(formValue(req.body, "spawn").c_str(), nullptr, 10));
            const scene::Entity* e = spawn ? m_scene.find(spawn) : nullptr;
            m_selected = e && e->is(scene::kTypeSpawn) ? spawn : 0;
            m_multi.clear();
        }
        m_playFromHere = from && m_playPlayers == 1;
        m_playRequest = true;
        MEALKIT_INFO("MkRender link: play %s%s", fileName(m_scenePath).c_str(), m_playFromHere ? " from its camera" : "");
        return reply(200, "{\"ok\":true}");
    }
    if (req.path == "look") {
        if (const std::string error = linkScene(formValue(req.body, "file")); !error.empty()) return fail(409, error);
        const std::string cutscene = formValue(req.body, "cutscene");
        if (!cutscene.empty()) {
            if (!previewCutscene(cutscene)) return fail(404, "cutscene '" + cutscene + "' has no playable camera clip");
            return reply(200, "{\"ok\":true}");
        }
        const u32 id = static_cast<u32>(std::strtoul(formValue(req.body, "camera").c_str(), nullptr, 10));
        const scene::Entity* e = m_scene.find(id);
        if (!e || !e->is(scene::kTypeVirtualCamera)) return fail(404, "no virtual camera with id " + std::to_string(id) + " in the saved scene");
        m_cutscenePreview.clear();
        m_lookThrough = id;
        m_sceneOpen = true;
        return reply(200, "{\"ok\":true}");
    }
    return fail(404, "not found");
}

} // namespace mealkit::editor
