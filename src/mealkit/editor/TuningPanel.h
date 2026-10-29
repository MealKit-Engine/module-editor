#pragma once

#include "mealkit/core/Reflect.h"
#include "mealkit/data/Tables.h"

#include <string>
#include <vector>

// Editor [Tuning] panel: edits game tuning (assets/game.tuning, simulation/content hash) and view tuning (assets/view.tuning)
// with types declared by the game DLL. Value instances are of the DLL's types, so they're kept as text before unloading the DLL and put back into the new type.
namespace mealkit::editor {

class TuningPanel {
public:
    static constexpr const char* kWindow = "Tuning###Tuning";
    static constexpr const char* kPlayViewTuningFile = "play.view.tuning";

    void setProjectDirectory(const std::string& directory) { m_projectDir = directory; }
    // Game tuning type (nullptr before unloading the DLL). Read from the file when first received.
    void setTuningType(const reflect::TypeInfo* type) { setDocType(m_tuning, type); }
    // View tuning (GameModule::viewTuningType)
    void setViewTuningType(const reflect::TypeInfo* type) { setDocType(m_viewTuning, type); }

    // When play starts: writes unsaved tuning values to a temp file and returns the path (empty string if no type = default file)
    std::string writePlayTuning() { return writePlayCopy(m_tuning, "play.tuning"); }
    // Copy of view tuning read by join windows (written when multiplayer play starts, rewritten when values change)
    std::string writePlayViewTuning() { return writePlayCopy(m_viewTuning, kPlayViewTuningFile); }
    // true once if changed (passed to the running game)
    bool takeTuningChanged() { return takeChanged(m_tuning); }
    bool takeViewTuningChanged() { return takeChanged(m_viewTuning); }
    reflect::View tuning() const { return {m_tuning.value.type(), m_tuning.value.data()}; }
    reflect::View viewTuning() const { return {m_viewTuning.value.type(), m_viewTuning.value.data()}; }

    // Resolves game tuning row references against the current tables and adds missing row references to problems ([Tables] panel list)
    void resolveRows(const data::TableIndexList& index, std::vector<std::string>* problems);

    bool dirty() const { return m_tuning.dirty || m_viewTuning.dirty; }
    void saveDirty(); // saves only edited files
    // dockWith: window to dock as a tab on first open. playing/multiplayer are used for the "applied immediately" hint.
    void draw(const char* dockWith, bool playing, bool multiplayer, const data::TableIndexList& index);
    // Text to show in the status bar (empty string if none)
    std::string takeStatus() { return std::move(m_status); }

private:
    // One tuning file
    struct Doc {
        const char* file = ""; // file name inside assets
        reflect::Instance value;
        reflect::TextFields text; // keeps values when swapping the DLL
        bool loaded = false;
        bool dirty = false;   // differs from the file
        bool changed = false; // not yet passed to the running game
    };

    void setDocType(Doc& doc, const reflect::TypeInfo* type);
    std::string docPath(const Doc& doc) const;
    bool load(Doc& doc);
    bool save(Doc& doc);
    std::string writePlayCopy(const Doc& doc, const char* fileName);
    static bool takeChanged(Doc& doc);
    void drawDoc(Doc& doc, bool liveInMultiplayer, bool playing, bool multiplayer, const data::TableIndexList& index);

    std::string m_projectDir;
    Doc m_tuning{"game.tuning"};     // game tuning (simulation, content hash)
    Doc m_viewTuning{"view.tuning"}; // view tuning
    std::string m_status;
};

} // namespace mealkit::editor
