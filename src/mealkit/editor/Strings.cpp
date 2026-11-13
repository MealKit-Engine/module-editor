#include "mealkit/editor/Strings.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/Paths.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace mealkit::editor {
namespace {

struct Table {
    std::string code;
    std::unordered_map<std::string, std::string> text;   // source → translation
    std::unordered_map<std::string, std::string> shown;  // tr() results (kept so pointers stay valid)
    std::unordered_map<std::string, std::string> labels; // trl() results
};

std::unordered_map<std::string, Table> g_tables;
Table* g_current = nullptr;
bool g_override = false; // --lang: the run keeps that language
std::filesystem::file_time_type g_settingsTime{};
std::chrono::steady_clock::time_point g_nextCheck{};
std::unordered_set<std::string> g_missing; // warn only once

std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            const char c = s[++i];
            out += c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            out += s[i];
        }
    }
    return out;
}

// "source = translation" lines. Empty lines and '#' lines are skipped. Sources don't contain " = "
void parseTable(const std::string& file, Table& table) {
    size_t pos = 0;
    int lineNo = 0;
    while (pos < file.size()) {
        size_t end = file.find('\n', pos);
        if (end == std::string::npos) end = file.size();
        std::string line = file.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lineNo == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        if (line.empty() || line[0] == '#') continue;
        const size_t eq = line.find(" = ");
        if (eq == std::string::npos) {
            MEALKIT_WARN("strings/%s.txt:%d: missing ' = '", table.code.c_str(), lineNo);
            continue;
        }
        table.text[unescape(line.substr(0, eq))] = unescape(line.substr(eq + 3));
    }
}

std::string settingsPath() { return editorUserDirectory() + "\\editor_settings.txt"; }

Table& tableFor(const char* code) {
    Table& t = g_tables[code];
    t.code = code;
    return t;
}

// Only the part before "##" (ImGui ID separator) is translated
std::string translate(Table& table, const char* text) {
    const char* id = std::strstr(text, "##");
    const std::string key = id ? std::string(text, id) : std::string(text);
    if (table.code == "en" || key.empty()) return key;
    auto it = table.text.find(key);
    if (it != table.text.end()) return it->second;
    if (g_missing.insert(table.code + ":" + key).second)
        MEALKIT_WARN("No %s translation: \"%s\"", table.code.c_str(), key.c_str());
    return key;
}

Table& current() {
    if (!g_current) g_current = &tableFor(kDefaultLanguage);
    return *g_current;
}

// The language line of user settings (MealKit Manager and the Mk tools write the same file); the default if none
std::string savedLanguage() {
    std::string settings;
    if (!readTextFile(settingsPath(), settings)) return kDefaultLanguage;
    const size_t at = settings.find("language ");
    if (at == std::string::npos) return kDefaultLanguage;
    const size_t end = settings.find_first_of("\r\n", at);
    return settings.substr(at + 9, end == std::string::npos ? std::string::npos : end - at - 9);
}

std::filesystem::file_time_type settingsTime() {
    const std::string path = settingsPath();
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(std::filesystem::path(std::u8string(path.begin(), path.end())), ec);
    return ec ? std::filesystem::file_time_type{} : t;
}

void useLanguage(const std::string& code) {
    bool known = false;
    for (const Language& lang : kLanguages) known |= code == lang.code;
    g_current = &tableFor(known ? code.c_str() : kDefaultLanguage);
}

} // namespace

void initStrings(const std::string& engineDirectory, const std::string& language) {
    for (const Language& lang : kLanguages) {
        Table& t = tableFor(lang.code);
        if (t.code == "en") continue;
        const std::string path = engineDirectory + "\\engine\\res\\strings\\" + lang.code + ".txt";
        std::string file;
        if (readTextFile(path, file)) parseTable(file, t);
        else MEALKIT_WARN("Editor strings not found: %s", path.c_str());
    }
    g_override = !language.empty();
    g_settingsTime = settingsTime();
    useLanguage(g_override ? language : savedLanguage());
}

bool followLanguage() {
    if (g_override) return false;
    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextCheck) return false;
    g_nextCheck = now + std::chrono::seconds(1);
    const auto t = settingsTime();
    if (t == g_settingsTime) return false;
    g_settingsTime = t;
    const std::string before = currentLanguage();
    useLanguage(savedLanguage());
    return before != currentLanguage();
}

const char* currentLanguage() { return current().code.c_str(); }

void setLanguage(const char* code) {
    g_current = &tableFor(code);
    createDirectories(editorUserDirectory());
    // Changes only the language line and leaves other lines (toolPath etc., editor/Tools.h) as is
    std::string old, out;
    readTextFile(settingsPath(), old);
    size_t pos = 0;
    while (pos < old.size()) {
        size_t end = old.find('\n', pos);
        if (end == std::string::npos) end = old.size();
        const std::string line = old.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.rfind("language ", 0) != 0) out += line + "\n";
    }
    writeTextFile(settingsPath(), std::string("language ") + code + "\n" + out);
    g_override = false;
    g_settingsTime = settingsTime();
}

const char* tr(const char* text) {
    Table& t = current();
    auto it = t.shown.find(text);
    if (it == t.shown.end()) {
        std::string shown = translate(t, text);
        if (const char* id = std::strstr(text, "##")) shown += id;
        it = t.shown.emplace(text, std::move(shown)).first;
    }
    return it->second.c_str();
}

const char* trl(const char* label) {
    Table& t = current();
    auto it = t.labels.find(label);
    if (it == t.labels.end()) it = t.labels.emplace(label, translate(t, label) + "###" + label).first;
    return it->second.c_str();
}

const char* trData(const char* text) {
    Table& t = current();
    auto it = t.text.find(text);
    return it == t.text.end() ? text : it->second.c_str();
}

std::string trf(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buffer, sizeof(buffer), tr(format), args);
    va_end(args);
    return buffer;
}

} // namespace mealkit::editor
