#include "mealkit/editor/ImportServer.h"

#include "mealkit/core/Log.h"
#include "mealkit/platform/Paths.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace mealkit::editor {
namespace {

constexpr const char* kImportMagic = "MEALKIT-IMPORT 1";

using live::jsonList;
using live::jsonString;
using live::Response;

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// 받은 파일 경로: 상대 경로, 영문자·숫자·_-./, ".." 없음
bool safePath(const std::string& p) {
    if (p.empty() || p[0] == '/' || p.find("..") != std::string::npos || p.find("//") != std::string::npos) return false;
    for (const char c : p)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '/'))
            return false;
    return true;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
bool endsWith(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

std::string kindName(reflect::Kind k) {
    switch (k) {
    case reflect::Kind::Bool: return "bool";
    case reflect::Kind::Int:
    case reflect::Kind::UInt: return "int";
    case reflect::Kind::Float: return "float";
    case reflect::Kind::String: return "string";
    case reflect::Kind::Color: return "color";
    case reflect::Kind::Ticks: return "time";
    case reflect::Kind::Array: return "list";
    default: return "other";
    }
}

// 뷰 모델 필드를 펼친다: 구조체는 a.b, 목록(MEALKIT_LIST)은 목록 이름과 원소 필드(목록 안 노드가 쓰는 이름)
void addFields(const reflect::TypeInfo& type, const std::string& prefix, std::string& out, bool& first, u32 depth) {
    for (const reflect::FieldInfo& f : type.fields) {
        const std::string name = prefix + f.name;
        out += std::string(first ? "" : ",") + "{\"name\":" + jsonString(name) + ",\"kind\":" + jsonString(kindName(f.value->kind)) + "}";
        first = false;
        if (depth >= 3) continue;
        if (f.value->kind == reflect::Kind::Struct && f.value->type) addFields(*f.value->type, name + ".", out, first, depth + 1);
        if (f.value->kind == reflect::Kind::Array && f.value->element && f.value->element->kind == reflect::Kind::Struct && f.value->element->type)
            addFields(*f.value->element->type, name + "[].", out, first, depth + 1);
    }
}

// .ui의 "color 이름 값" / "number 이름 값" 줄을 이름으로 바꾸거나 더한다 (손으로 쓴 다른 토큰은 둔다)
std::string mergeTheme(const std::string& existing, const std::string& incoming) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos < existing.size()) {
        size_t end = existing.find('\n', pos);
        if (end == std::string::npos) end = existing.size();
        std::string line = existing.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        pos = end + 1;
    }
    auto keyOf = [](const std::string& line) {
        const size_t a = line.find(' ');
        if (a == std::string::npos) return std::string();
        const size_t b = line.find(' ', a + 1);
        const std::string kind = line.substr(0, a);
        if (kind != "color" && kind != "number") return std::string();
        return line.substr(0, b == std::string::npos ? line.size() : b);
    };
    pos = 0;
    bool header = lines.empty(); // 새 파일: 첫 토큰 앞의 머리말·형식 줄을 그대로 쓴다
    while (pos < incoming.size()) {
        size_t end = incoming.find('\n', pos);
        if (end == std::string::npos) end = incoming.size();
        const std::string line = incoming.substr(pos, end - pos);
        pos = end + 1;
        const std::string key = keyOf(line);
        if (key.empty()) {
            if (header && !line.empty()) lines.push_back(line);
            continue;
        }
        header = false;
        bool replaced = false;
        for (std::string& l : lines)
            if (keyOf(l) == key) l = line, replaced = true;
        if (!replaced) lines.push_back(line);
    }
    std::string out;
    for (const std::string& l : lines)
        if (!(l.empty() && &l == &lines.back())) out += l + "\n";
    return out;
}

// Penpot이 플러그인 코드를 부르는 출처 (https://*.penpot.app, 직접 띄운 Penpot은 localhost)
bool penpotOrigin(const std::string& origin) {
    const std::string o = lower(origin);
    if (startsWith(o, "https://")) {
        const std::string host = o.substr(8);
        if (host == "penpot.app" || endsWith(host, ".penpot.app")) return true;
    }
    for (const char* local : {"http://localhost", "http://127.0.0.1", "https://localhost"})
        if (o == local || startsWith(o, (std::string(local) + ":").c_str())) return true;
    return false;
}

} // namespace

void ImportServer::attach(live::Server& server, const ImportProject& project) {
    m_server = &server;
    m_project = project;
    live::Service s;
    s.name = "penpot";
    s.allowOrigin = penpotOrigin;
    s.handle = [this](const live::Request& r) { return handle(r); };
    server.addService(std::move(s));
    if (server.running()) MEALKIT_INFO("Penpot plugin: %s", pluginUrl().c_str());
}

std::string ImportServer::pluginUrl() const {
    if (!m_server || !m_server->running()) return {};
    return m_server->baseUrl() + "penpot/manifest.json";
}

std::vector<std::string> ImportServer::takeEvents() {
    std::vector<std::string> out;
    out.swap(m_events);
    return out;
}

Response ImportServer::handle(const live::Request& req) {
    if (req.method == "GET" && req.path == "api/project") return project();
    if (req.method == "POST" && req.path == "api/export") {
        if (!m_server || req.origin != m_server->origin()) { // 이 서버가 준 플러그인 패널만 쓴다
            Response r;
            r.status = 403;
            r.body = "{\"error\":\"export must come from the plugin panel\"}";
            return r;
        }
        return importFiles(req.body);
    }
    if (req.method == "GET") return serveFile(req.path.empty() ? "ui.html" : req.path);
    Response r;
    r.status = 404;
    r.body = "{\"error\":\"not found\"}";
    return r;
}

Response ImportServer::serveFile(const std::string& name) const {
    Response r;
    static const std::pair<const char*, const char*> files[] = {
        {"manifest.json", "application/json"},
        {"plugin.js", "text/javascript"},
        {"ui.html", "text/html; charset=utf-8"},
        {"icon.png", "image/png"},
    };
    for (const auto& [file, type] : files) {
        if (name != file) continue;
        if (!readTextFile(m_project.engineDir + "/tools/penpot-plugin/dist/" + file, r.body)) break;
        r.contentType = type;
        // 플러그인 코드·아이콘·패널 주소는 이 서비스의 토큰 경로 (Penpot이 상대 주소를 어디에 붙일지 기대지 않는다)
        const std::string base = m_server ? m_server->baseUrl() + "penpot/" : std::string();
        for (size_t at = r.body.find("__MEALKIT_BASE__"); at != std::string::npos; at = r.body.find("__MEALKIT_BASE__", at + base.size()))
            r.body.replace(at, 16, base);
        // Penpot은 아이콘을 "플러그인 출처 + / + 값"으로 읽는다: 출처 기준 경로 (앞의 /는 Penpot이 붙인다)
        const std::string path = (m_server ? m_server->token() : std::string()) + "/penpot/";
        for (size_t at = r.body.find("__MEALKIT_PATH__"); at != std::string::npos; at = r.body.find("__MEALKIT_PATH__", at + path.size()))
            r.body.replace(at, 16, path);
        return r;
    }
    r.status = 404;
    r.body = "{\"error\":\"not found\"}";
    return r;
}

Response ImportServer::project() const {
    std::vector<std::string> fonts;
    for (const char* ext : {".ttf", ".otf"})
        for (const std::string& f : listFiles(m_project.projectDir + "/assets/fonts", ext)) fonts.push_back(f.substr(0, f.size() - 4));
    // 액션: 지금 .ui 파일에 쓰인 것 (게임 코드가 받는 이름)
    std::vector<std::string> actions, uiFiles;
    const std::string uiDir = m_project.projectDir + "/assets/ui";
    for (const std::string& f : listFiles(uiDir, ".ui")) {
        uiFiles.push_back(f);
        std::string text;
        if (!readTextFile(uiDir + "/" + f, text)) continue;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            for (const char* k : {"button ", "back "})
                if (startsWith(line, k)) {
                    const std::string a = line.substr(std::char_traits<char>::length(k));
                    if (!a.empty() && a.find(' ') == std::string::npos && std::find(actions.begin(), actions.end(), a) == actions.end())
                        actions.push_back(a);
                }
        }
    }
    std::sort(actions.begin(), actions.end());
    std::string vms = "[";
    if (m_viewModels)
        for (size_t i = 0; i < m_viewModels->size(); ++i) {
            const reflect::TypeInfo& t = *(*m_viewModels)[i];
            vms += std::string(i ? "," : "") + "{\"name\":" + jsonString(t.name) + ",\"fields\":[";
            bool first = true;
            addFields(t, "", vms, first, 0);
            vms += "]}";
        }
    vms += "]";
    Response r;
    r.body = "{\"name\":" + jsonString(m_project.name) + ",\"language\":" + jsonString(m_project.language) +
             ",\"fonts\":" + jsonList(fonts) + ",\"actions\":" + jsonList(actions) + ",\"uiFiles\":" + jsonList(uiFiles) +
             ",\"viewModels\":" + vms + "}";
    return r;
}

Response ImportServer::importFiles(const std::string& body) {
    std::vector<std::string> written, errors, warnings, cleanDirs;
    size_t pos = body.find('\n');
    if (pos == std::string::npos || body.compare(0, pos, kImportMagic) != 0) {
        Response r;
        r.status = 400;
        r.body = "{\"ok\":false,\"errors\":[\"bad request (expected " + std::string(kImportMagic) + ")\"]}";
        return r;
    }
    ++pos;
    const std::string& root = m_project.projectDir;
    while (pos < body.size()) {
        const size_t lineEnd = body.find('\n', pos);
        if (lineEnd == std::string::npos) break;
        const std::string header = body.substr(pos, lineEnd - pos);
        pos = lineEnd + 1;
        // "<종류> <경로> <크기> [force]"
        std::vector<std::string> words;
        for (size_t a = 0; a < header.size();) {
            size_t b = header.find(' ', a);
            if (b == std::string::npos) b = header.size();
            if (b > a) words.push_back(header.substr(a, b - a));
            a = b + 1;
        }
        const unsigned long long size = words.size() >= 3 ? std::strtoull(words[2].c_str(), nullptr, 10) : 0;
        if (words.size() < 3 || pos + size > body.size()) {
            errors.push_back("bad entry: " + header);
            break;
        }
        const std::string data = body.substr(pos, static_cast<size_t>(size));
        pos += static_cast<size_t>(size);
        if (pos < body.size() && body[pos] == '\n') ++pos;
        const std::string& k = words[0];
        const std::string& p = words[1];
        const bool force = words.size() >= 4 && words[3] == "force";
        if (k != "strings" && !safePath(p)) {
            errors.push_back("unsafe path: " + p);
            continue;
        }
        std::string target;
        if (k == "ui" && startsWith(p, "ui/") && endsWith(p, ".ui") && p != "ui/theme.ui") {
            target = root + "/assets/" + p;
            std::string old;
            if (!force && readTextFile(target, old) && !startsWith(old, "# MealKit UI exported from Penpot")) {
                errors.push_back("assets/" + p + " is hand-written; export again with \"Replace hand-written files\" to overwrite it");
                continue;
            }
        } else if (k == "png" && startsWith(p, "ui/") && endsWith(p, ".png")) {
            target = root + "/assets/" + p;
        } else if (k == "clean" && startsWith(p, "ui/") && endsWith(p, "/")) {
            cleanDirs.push_back(p);
            continue;
        } else if (k == "theme" && p == "ui/theme.ui") {
            target = root + "/assets/" + p;
            std::string old;
            readTextFile(target, old);
            const std::string merged = mergeTheme(old, data);
            if (merged == old) continue;
            createDirectories(parentDirectory(target));
            if (writeTextFile(target, merged)) written.push_back("assets/" + p);
            else errors.push_back("cannot write assets/" + p);
            continue;
        } else if (k == "strings") {
            // 프로젝트 기본 언어의 문자열 표에 없는 키만 더한다 (있는 키는 표가 이긴다, 다르면 경고)
            target = root + "/assets/strings/" + m_project.language + ".txt";
            std::string text;
            readTextFile(target, text);
            std::vector<std::pair<std::string, std::string>> existing;
            size_t lp = 0;
            while (lp < text.size()) {
                size_t le = text.find('\n', lp);
                if (le == std::string::npos) le = text.size();
                std::string line = text.substr(lp, le - lp);
                lp = le + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t eq = line.find(" = ");
                if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
                existing.emplace_back(line.substr(0, eq), line.substr(eq + 3));
            }
            std::string added;
            size_t dp = 0;
            while (dp < data.size()) {
                size_t de = data.find('\n', dp);
                if (de == std::string::npos) de = data.size();
                const std::string line = data.substr(dp, de - dp);
                dp = de + 1;
                const size_t eq = line.find(" = ");
                if (eq == std::string::npos) continue;
                const std::string key = line.substr(0, eq), value = line.substr(eq + 3);
                const auto it = std::find_if(existing.begin(), existing.end(), [&](const auto& e) { return e.first == key; });
                if (it == existing.end()) added += line + "\n";
                else if (it->second != value)
                    warnings.push_back("string " + key + ": Penpot has \"" + value + "\", the table has \"" + it->second + "\" (kept the table)");
            }
            if (added.empty()) continue;
            if (!text.empty() && text.back() != '\n') text += "\n";
            text += "\n# From Penpot\n" + added;
            if (writeTextFile(target, text)) written.push_back("assets/strings/" + m_project.language + ".txt");
            else errors.push_back("cannot write the string table");
            continue;
        } else if (k == "penpot" && endsWith(p, ".penpot") && p.find('/') == std::string::npos) {
            target = root + "/art/ui/" + p;
        } else if (k == "boxes" && endsWith(p, ".boxes") && p.find('/') == std::string::npos) {
            target = root + "/art/ui/boxes/" + p;
        } else if (k == "preview" && endsWith(p, ".png") && p.find('/') == std::string::npos) {
            target = root + "/art/ui/preview/" + p; // 픽셀 비교 (보조 검사)
        } else {
            errors.push_back("not allowed: " + k + " " + p);
            continue;
        }
        createDirectories(parentDirectory(target));
        if (writeTextFile(target, data)) written.push_back(target.substr(root.size() + 1));
        else errors.push_back("cannot write " + target);
    }
    // 이번에 쓰지 않은 예전 구운 PNG를 지운다 (화면 폴더 assets/ui/<화면>/)
    for (const std::string& dir : cleanDirs)
        for (const std::string& f : listFiles(root + "/assets/" + dir, ".png")) {
            const std::string rel = "assets/" + dir + f;
            if (std::find(written.begin(), written.end(), rel) == written.end() && removeFile(root + "/" + rel)) written.push_back(rel + " (removed)");
        }
    char line[160];
    std::snprintf(line, sizeof(line), "Penpot: wrote %u file(s), %u error(s), %u warning(s)", static_cast<unsigned>(written.size()),
                  static_cast<unsigned>(errors.size()), static_cast<unsigned>(warnings.size()));
    m_events.push_back(line);
    MEALKIT_INFO("%s", line);
    for (const std::string& e : errors) MEALKIT_WARN("Penpot import: %s", e.c_str());
    Response r;
    r.body = "{\"ok\":" + std::string(errors.empty() ? "true" : "false") + ",\"written\":" + jsonList(written) + ",\"errors\":" + jsonList(errors) +
             ",\"warnings\":" + jsonList(warnings) + "}";
    return r;
}

} // namespace mealkit::editor
