#include "aster/editor/GameModuleHost.h"

#include "aster/core/Log.h"
#include "aster/platform/Paths.h"

#include <filesystem>
#include <system_error>
#include <windows.h>

namespace aster::editor {
namespace fs = std::filesystem;

namespace {

fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string toUtf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

u64 writeTime(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(fromUtf8(path).c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<u64>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
}

} // namespace

GameModuleHost::~GameModuleHost() { unload(); }

bool GameModuleHost::load(const std::string& dllPath, std::string& error) {
    std::error_code ec;
    const fs::path source = fromUtf8(dllPath);
    const u64 stamp = writeTime(dllPath);
    if (stamp == 0) {
        error = "game DLL not found: " + dllPath;
        return false;
    }

    // hot\<이름>.<번호>.dll 로 복사해서 불러온다. 지난 복사본은 지운다(불러온 것은 지워지지 않으므로 남는다).
    const fs::path hotDir = source.parent_path() / "hot";
    fs::create_directories(hotDir, ec);
    for (const auto& entry : fs::directory_iterator(hotDir, ec)) fs::remove(entry.path(), ec);
    const u32 next = m_generation + 1;
    const fs::path copy = hotDir / (source.stem().wstring() + L"." + std::to_wstring(next) + L".dll");
    if (!fs::copy_file(source, copy, fs::copy_options::overwrite_existing, ec)) {
        error = "cannot copy the game DLL (still being written?): " + ec.message();
        return false;
    }

    HMODULE dll = LoadLibraryW(copy.c_str());
    if (!dll) {
        error = "LoadLibrary failed (" + std::to_string(GetLastError()) + "): " + toUtf8(copy);
        fs::remove(copy, ec);
        return false;
    }
    auto get = reinterpret_cast<game::GetGameModuleFn>(GetProcAddress(dll, game::kGetGameModuleSymbol));
    const game::GameModule* module = get ? get() : nullptr;
    if (!module || !module->create || !module->destroy) {
        error = "the DLL does not export asterGetGameModule (ASTER_GAME_MODULE missing?)";
        FreeLibrary(dll);
        fs::remove(copy, ec);
        return false;
    }
    if (module->apiVersion != game::kApiVersion) {
        error = "game DLL API version " + std::to_string(module->apiVersion) + " != engine " +
                std::to_string(game::kApiVersion) + " - rebuild the game code";
        FreeLibrary(dll);
        fs::remove(copy, ec);
        return false;
    }

    unload(); // 새 모듈이 준비된 뒤에만 이전 것을 내린다
    m_dll = dll;
    m_module = module;
    m_sourcePath = dllPath;
    m_loadedCopy = toUtf8(copy);
    m_loadedWriteTime = m_seenWriteTime = stamp;
    m_generation = next;
    ASTER_INFO("Game module '%s' loaded (#%u): %s", module->name, m_generation, dllPath.c_str());
    return true;
}

void GameModuleHost::unload() {
    if (!m_dll) return;
    FreeLibrary(static_cast<HMODULE>(m_dll));
    std::error_code ec;
    fs::remove(fromUtf8(m_loadedCopy), ec);
    m_dll = nullptr;
    m_module = nullptr;
    m_loadedCopy.clear();
}

bool GameModuleHost::sourceChangedAndStable() {
    if (m_sourcePath.empty()) return false;
    const u64 stamp = writeTime(m_sourcePath);
    if (stamp == 0 || stamp == m_loadedWriteTime) return false;
    const bool stable = stamp == m_seenWriteTime;
    m_seenWriteTime = stamp;
    return stable;
}

bool GameModuleHost::olderThanEngine(const std::string& dllPath) {
    const u64 game = writeTime(dllPath);
    const u64 engine = writeTime(executableDirectory() + "\\aster.dll");
    return game != 0 && engine != 0 && game < engine;
}

} // namespace aster::editor
