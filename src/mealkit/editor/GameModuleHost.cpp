#include "mealkit/editor/GameModuleHost.h"

#include "mealkit/core/Log.h"
#include "mealkit/editor/Project.h"
#include "mealkit/platform/DynamicLibrary.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/System.h"

#include <algorithm>
#include <filesystem>
#include <system_error>
#include <vector>

namespace mealkit::editor {
namespace fs = std::filesystem;

namespace {

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string toUtf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

} // namespace

GameModuleHost::~GameModuleHost() { unload(); }

bool GameModuleHost::load(const std::string& dllPath, std::string& error) {
    std::error_code ec;
    const fs::path source = fromUtf8(dllPath);
    const u64 stamp = fileWriteTime(dllPath);
    if (stamp == 0) {
        error = "game DLL not found: " + dllPath;
        return false;
    }

    // Copied to hot/<name>.<process>.<number>.dll (.so, .dylib) and loaded.
    // Includes the process number: join windows of multiplayer play (same editor executable) each use their own copy
    const fs::path hotDir = source.parent_path() / "hot";
    fs::create_directories(hotDir, ec);
    const u32 next = m_generation + 1;
    const std::wstring mine = source.stem().wstring() + L"." + std::to_wstring(processId()) + L".";
    const fs::path copy = hotDir / (mine + std::to_wstring(next) + fs::path(libraryExtension()).wstring());
    if (!fs::copy_file(source, copy, fs::copy_options::overwrite_existing, ec)) {
        error = "cannot copy the game DLL (still being written?): " + ec.message();
        return false;
    }
    // Delete old copies: all of mine, and others' only if older than 1 minute (so a join window's just-copied file isn't deleted before loading).
    // Loaded DLLs can't be deleted, so they remain. Creation time is in OS units (Windows 100ns), so compared with my new copy
    const unsigned long long now = fileCreationTime(toUtf8(copy));
    constexpr unsigned long long kStale = 60ull * 10'000'000ull;
    for (const auto& entry : fs::directory_iterator(hotDir, ec)) {
        if (entry.path() == copy) continue;
        const bool own = entry.path().filename().wstring().rfind(mine, 0) == 0;
        const unsigned long long created = fileCreationTime(toUtf8(entry.path()));
        if (own || (created != 0 && now > created + kStale)) fs::remove(entry.path(), ec);
    }

    void* dll = loadLibrary(toUtf8(copy), error);
    if (!dll) {
        fs::remove(copy, ec);
        return false;
    }
    auto get = reinterpret_cast<game::GetGameModuleFn>(findSymbol(dll, game::kGetGameModuleSymbol));
    const game::GameModule* module = get ? get() : nullptr;
    if (!module || !module->create || !module->destroy) {
        error = "the DLL does not export mealkitGetGameModule (MEALKIT_GAME_MODULE missing?)";
        freeLibrary(dll);
        fs::remove(copy, ec);
        return false;
    }
    if (module->apiVersion != game::kApiVersion) {
        error = "game DLL API version " + std::to_string(module->apiVersion) + " != engine " +
                std::to_string(game::kApiVersion) + " - rebuild the game code";
        freeLibrary(dll);
        fs::remove(copy, ec);
        return false;
    }

    unload(); // the previous one is unloaded only after the new module is ready
    m_dll = dll;
    m_module = module;
    m_sourcePath = dllPath;
    m_loadedCopy = toUtf8(copy);
    m_loadedWriteTime = m_seenWriteTime = stamp;
    m_generation = next;
    MEALKIT_INFO("Game module '%s' loaded (#%u): %s", module->name, m_generation, dllPath.c_str());
    return true;
}

void GameModuleHost::unload() {
    if (!m_dll) return;
    freeLibrary(m_dll);
    std::error_code ec;
    fs::remove(fromUtf8(m_loadedCopy), ec);
    m_dll = nullptr;
    m_module = nullptr;
    m_loadedCopy.clear();
}

bool GameModuleHost::sourceChangedAndStable() {
    if (m_sourcePath.empty()) return false;
    const u64 stamp = fileWriteTime(m_sourcePath);
    if (stamp == 0 || stamp == m_loadedWriteTime) return false;
    const bool stable = stamp == m_seenWriteTime;
    m_seenWriteTime = stamp;
    return stable;
}

bool GameModuleHost::olderThanEngine(const std::string& dllPath, const std::string& engineDirectory) {
    const u64 game = fileWriteTime(dllPath);
    if (game == 0) return false;
    // mealkit.dll's time isn't used: it's relinked even when only engine .cpp changes, but the game DLL isn't relinked.
    // The linker rewrites the import lib only when exports change → matches when the game DLL needs relinking.
    const std::string bin = executableDirectory();
    u64 engine = fileWriteTime(toUtf8(fromUtf8(bin).parent_path() / "engine" / "mealkit.lib"));
    if (engine == 0) engine = fileWriteTime(engineLibraryPath()); // layouts without an import lib (Linux: libmealkit.so)
    // When only headers changed (struct layout, inline functions), game code must be recompiled even if exports are the same.
    // Header folders as the game's compile line has them: the kernel (engine/core/<part>/include), engine modules (modules/<name>/include)
    // and the pinned MkRender/MkUI/MkAudio/MkContract headers (third_party/mk*/include). The layout moved from engine/src in 2026-11-08;
    // a scan of a folder that no longer exists silently finds nothing, so every root is listed from what is on disk.
    std::error_code ec;
    const fs::path root = fromUtf8(engineDirectory);
    std::vector<fs::path> includeRoots;
    for (const fs::path& group : {root / "engine" / "core", root / "modules", root / "third_party"}) {
        for (fs::directory_iterator it(group, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (group.filename() == "modules" && name == "editor") continue; // editor headers aren't used by game code
            if (group.filename() == "third_party" && name.rfind("mk", 0) != 0) continue; // libraries without engine types (Jolt, ImGui ...) change with a version bump
            includeRoots.push_back(it->path() / "include");
        }
        ec.clear();
    }
    for (const fs::path& include : includeRoots) {
        for (fs::recursive_directory_iterator it(include, fs::directory_options::follow_directory_symlink, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path& path = it->path();
            if (path.extension() != ".h") continue;
            engine = std::max<u64>(engine, fileWriteTime(toUtf8(path)));
        }
        ec.clear();
    }
    return engine != 0 && game < engine;
}

bool GameModuleHost::olderThanSources(const std::string& dllPath, const std::string& projectDirectory) {
    const u64 game = fileWriteTime(dllPath);
    if (game == 0) return false;
    const fs::path root = fromUtf8(projectDirectory);
    if (fileWriteTime(toUtf8(root / "CMakeLists.txt")) > game) return true;
    std::error_code ec;
    for (const char* folder : {"source", "sim", "view", "app", "modules"}) { // projects use source/, engine samples sim/ view/ app/
        for (fs::recursive_directory_iterator it(root / folder, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path ext = it->path().extension();
            if (ext != ".h" && ext != ".cpp" && ext != ".inl" && it->path().filename() != "CMakeLists.txt") continue;
            if (fileWriteTime(toUtf8(it->path())) > game) return true;
        }
        ec.clear();
    }
    return false;
}

} // namespace mealkit::editor
