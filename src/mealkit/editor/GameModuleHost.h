#pragma once

#include "mealkit/core/Types.h"
#include "mealkit/game/GameInstance.h"

#include <string>

// The general-purpose editor loads and swaps the project's game DLL (hot reload).
// The original DLL is left alone and a copy in the hot\ folder is loaded → the original can be rebuilt while the editor is running.
namespace mealkit::editor {

class GameModuleHost {
public:
    GameModuleHost() = default;
    ~GameModuleHost();
    GameModuleHost(const GameModuleHost&) = delete;
    GameModuleHost& operator=(const GameModuleHost&) = delete;

    // Loads a copy of dllPath. On failure, keeps the previous module and returns false.
    // The caller must first delete all GameInstances made with this module.
    bool load(const std::string& dllPath, std::string& error);
    void unload();

    bool loaded() const { return m_module != nullptr; }
    const game::GameModule* module() const { return m_module; }
    u32 generation() const { return m_generation; } // load count (for display)

    game::GameInstance* create() const { return m_module ? m_module->create() : nullptr; }
    void destroy(game::GameInstance* instance) const {
        if (m_module && instance) m_module->destroy(instance);
    }

    // Whether the original DLL changed after loading and writing seems finished (true when the same modification time is seen twice in a row).
    // Call at about 0.5 s intervals.
    bool sourceChangedAndStable();
    // Whether the original DLL is older than the engine → game code must be rebuilt.
    // Engine time = the newest of the engine import lib (changes only when exports change) and engine headers in engineDirectory (outside editor/)
    static bool olderThanEngine(const std::string& dllPath, const std::string& engineDirectory);
    // Whether the original DLL is older than the project's game code (source/, sim/, view/, app/, modules/ .h/.cpp/.inl and CMakeLists.txt):
    // the code was edited outside the editor (VS Code, git pull) while the editor was closed → rebuild when the project opens
    static bool olderThanSources(const std::string& dllPath, const std::string& projectDirectory);

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
