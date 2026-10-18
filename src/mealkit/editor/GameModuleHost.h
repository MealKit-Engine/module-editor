#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/game/GameInstance.h"

#include <string>

// 범용 에디터가 프로젝트의 게임 DLL을 불러오고 바꿔 끼운다 (핫 리로드).
// 원본 DLL은 그대로 두고 hot\ 폴더의 복사본을 불러온다 → 에디터가 켜져 있어도 원본을 다시 빌드할 수 있다.
namespace mealkit::editor {

class GameModuleHost {
public:
    GameModuleHost() = default;
    ~GameModuleHost();
    GameModuleHost(const GameModuleHost&) = delete;
    GameModuleHost& operator=(const GameModuleHost&) = delete;

    // dllPath의 복사본을 불러온다. 실패하면 이전 모듈을 그대로 두고 false.
    // 호출하는 쪽은 그 전에 이 모듈로 만든 GameInstance를 모두 지워야 한다.
    bool load(const std::string& dllPath, std::string& error);
    void unload();

    bool loaded() const { return m_module != nullptr; }
    const game::GameModule* module() const { return m_module; }
    u32 generation() const { return m_generation; } // 불러온 횟수 (화면 표시용)

    game::GameInstance* create() const { return m_module ? m_module->create() : nullptr; }
    void destroy(game::GameInstance* instance) const {
        if (m_module && instance) m_module->destroy(instance);
    }

    // 원본 DLL이 불러온 뒤 바뀌었고, 쓰기가 끝난 것 같은가 (같은 수정 시각을 두 번 연속 보면 true).
    // 0.5초 정도 간격으로 부른다.
    bool sourceChangedAndStable();
    // 원본 DLL이 엔진보다 오래되었는가 → 게임 코드를 다시 빌드해야 한다.
    // 엔진 시각 = 엔진 import lib(내보내기가 바뀔 때만 바뀐다)과 engineDirectory의 엔진 헤더(editor/ 밖) 중 가장 새것
    static bool olderThanEngine(const std::string& dllPath, const std::string& engineDirectory);

private:
    void* m_dll = nullptr; // loadLibrary()
    const game::GameModule* m_module = nullptr;
    std::string m_sourcePath;
    std::string m_loadedCopy;
    u64 m_loadedWriteTime = 0;
    u64 m_seenWriteTime = 0;
    u32 m_generation = 0;
};

} // namespace mealkit::editor
