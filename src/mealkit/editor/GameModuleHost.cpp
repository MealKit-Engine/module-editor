#include "mealkit/editor/GameModuleHost.h"

#include "mealkit/core/Log.h"
#include "mealkit/platform/DynamicLibrary.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/System.h"

#include <filesystem>
#include <system_error>

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

    // hot\<이름>.<번호>.dll 로 복사해서 불러온다. 지난 복사본은 지운다(불러온 것은 지워지지 않으므로 남는다).
    const fs::path hotDir = source.parent_path() / "hot";
    fs::create_directories(hotDir, ec);
    for (const auto& entry : fs::directory_iterator(hotDir, ec)) fs::remove(entry.path(), ec);
    const u32 next = m_generation + 1;
    // 프로세스 번호를 넣는다: 여러 명 플레이의 참가 창(같은 에디터 실행 파일)도 각자 복사본을 쓴다
    const fs::path copy = hotDir / (source.stem().wstring() + L"." + std::to_wstring(processId()) + L"." +
                                    std::to_wstring(next) + L".dll");
    if (!fs::copy_file(source, copy, fs::copy_options::overwrite_existing, ec)) {
        error = "cannot copy the game DLL (still being written?): " + ec.message();
        return false;
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

    unload(); // 새 모듈이 준비된 뒤에만 이전 것을 내린다
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

bool GameModuleHost::olderThanEngine(const std::string& dllPath) {
    const u64 game = fileWriteTime(dllPath);
    const u64 engine = fileWriteTime(executableDirectory() + "\\mealkit.dll");
    return game != 0 && engine != 0 && game < engine;
}

} // namespace mealkit::editor
