#include "mealkit/editor/GameModuleHost.h"

#include "mealkit/core/Log.h"
#include "mealkit/platform/DynamicLibrary.h"
#include "mealkit/platform/Paths.h"
#include "mealkit/platform/System.h"

#include <algorithm>
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

    // hot\<이름>.<프로세스>.<번호>.dll 로 복사해서 불러온다.
    // 프로세스 번호를 넣는다: 여러 명 플레이의 참가 창(같은 에디터 실행 파일)도 각자 복사본을 쓴다
    const fs::path hotDir = source.parent_path() / "hot";
    fs::create_directories(hotDir, ec);
    const u32 next = m_generation + 1;
    const std::wstring mine = source.stem().wstring() + L"." + std::to_wstring(processId()) + L".";
    const fs::path copy = hotDir / (mine + std::to_wstring(next) + L".dll");
    if (!fs::copy_file(source, copy, fs::copy_options::overwrite_existing, ec)) {
        error = "cannot copy the game DLL (still being written?): " + ec.message();
        return false;
    }
    // 지난 복사본 지우기: 내 것은 모두, 다른 프로세스 것은 1분 넘게 된 것만 (막 복사해 불러오려는 참가 창의 파일을 지우지 않게).
    // 불러온 DLL은 지워지지 않으므로 남는다. 만든 시각은 OS 단위(Windows 100ns)라 내 새 복사본과 비교한다
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

bool GameModuleHost::olderThanEngine(const std::string& dllPath, const std::string& engineDirectory) {
    const u64 game = fileWriteTime(dllPath);
    if (game == 0) return false;
    // mealkit.dll 시각은 쓰지 않는다: 엔진 .cpp만 바뀌어도 다시 링크되지만 게임 DLL은 다시 링크되지 않는다.
    // import lib은 내보내기가 바뀔 때만 링커가 다시 쓴다 → 게임 DLL이 다시 링크돼야 하는 때와 맞다.
    const std::string bin = executableDirectory();
    u64 engine = fileWriteTime(toUtf8(fromUtf8(bin).parent_path() / "engine" / "mealkit.lib"));
    if (engine == 0) engine = fileWriteTime(bin + "\\mealkit.dll"); // import lib이 없는 배치
    // 헤더만 바뀐 경우(구조체 배치, 인라인 함수)는 내보내기가 같아도 게임 코드를 다시 컴파일해야 한다
    std::error_code ec;
    const fs::path headers = fromUtf8(engineDirectory) / "engine" / "src" / "mealkit";
    for (fs::recursive_directory_iterator it(headers, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& path = it->path();
        if (it.depth() == 0 && path.filename() == "editor") { // 에디터 헤더는 게임 코드가 쓰지 않는다
            it.disable_recursion_pending();
            continue;
        }
        if (path.extension() != ".h") continue;
        engine = std::max(engine, fileWriteTime(toUtf8(path)));
    }
    return engine != 0 && game < engine;
}

} // namespace mealkit::editor
