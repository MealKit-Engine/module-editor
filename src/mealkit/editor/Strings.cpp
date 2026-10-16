#include "mealkit/editor/Strings.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/Paths.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace mealkit::editor {
namespace {

struct Table {
    std::string code;
    std::unordered_map<std::string, std::string> text;   // 원문 → 번역
    std::unordered_map<std::string, std::string> shown;  // tr() 결과 (포인터를 오래 쓰도록 보관)
    std::unordered_map<std::string, std::string> labels; // trl() 결과
};

std::unordered_map<std::string, Table> g_tables;
Table* g_current = nullptr;
std::unordered_set<std::string> g_missing; // 한 번만 경고

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

// "원문 = 번역" 줄. 빈 줄과 '#' 줄은 건너뛴다. 원문에는 " = "를 쓰지 않는다
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

// "##"(ImGui ID 구분) 앞부분만 번역한다
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

} // namespace

void initStrings(const std::string& engineDirectory) {
    for (const Language& lang : kLanguages) {
        Table& t = tableFor(lang.code);
        if (t.code == "en") continue;
        const std::string path = engineDirectory + "\\engine\\res\\strings\\" + lang.code + ".txt";
        std::string file;
        if (readTextFile(path, file)) parseTable(file, t);
        else MEALKIT_WARN("Editor strings not found: %s", path.c_str());
    }
    std::string settings;
    std::string code = kDefaultLanguage;
    if (readTextFile(settingsPath(), settings)) {
        const size_t at = settings.find("language ");
        if (at != std::string::npos) {
            size_t end = settings.find_first_of("\r\n", at);
            code = settings.substr(at + 9, end == std::string::npos ? std::string::npos : end - at - 9);
        }
    }
    bool known = false;
    for (const Language& lang : kLanguages) known |= code == lang.code;
    g_current = &tableFor(known ? code.c_str() : kDefaultLanguage);
}

const char* currentLanguage() { return current().code.c_str(); }

void setLanguage(const char* code) {
    g_current = &tableFor(code);
    createDirectories(editorUserDirectory());
    writeTextFile(settingsPath(), std::string("language ") + code + "\n");
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
