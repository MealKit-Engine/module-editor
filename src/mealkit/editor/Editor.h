#pragma once

#include "mealkit/asset/Cutscene.h"
#include "mealkit/asset/ModelCache.h"
#include "mealkit/camera/Brain.h"
#include "mealkit/editor/BuildPanel.h"
#include "mealkit/editor/TestsPanel.h"
#include "mealkit/editor/LogPanel.h"
#include "mealkit/editor/PerfPanel.h"
#include "mealkit/editor/Preferences.h"
#include "mealkit/editor/ImGuiLayer.h"
#include "mealkit/editor/Project.h"
#include "mealkit/editor/FeedbackPanel.h"
#include "mealkit/editor/ModulesPanel.h"
#include "mealkit/editor/ProjectSettingsPanel.h"
#include "mealkit/editor/TuningPanel.h"
#include "mealkit/editor/ImportServer.h"
#include "mealkit/editor/LookdevPanel.h"
#include "mealkit/editor/Tools.h"
#include "mealkit/editor/UiPanel.h"
#include "mealkit/editor/SoundPanel.h"
#include "mealkit/editor/TablesPanel.h"
#include "mealkit/game/FrameStats.h"
#include "mealkit/game/RenderStyle.h"
#include "mealkit/game/SimDebug.h"
#include "mealkit/platform/Image.h"
#include "mealkit/render/DrawList.h"
#include "mealkit/render/Renderer.h"
#include "mealkit/scene/Scene.h"

#include "mealkit/editor/AssetThumbnails.h"

#include <functional>
#include <map>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mealkit {
class Window;
}

// Scene editing screen of the general-purpose editor (MealKitEditor).
// Handles scene editing (scene tree, inspector, gizmo, undo, save), switching to play mode, and project builds (game code, shipping).
// Contains no game logic: pressing play makes the app run scene() with the game DLL's GameInstance.
namespace mealkit::editor {

enum class Mode { Edit, Play };

// Path relative to root ('/' separated) if the file is inside the root folder, empty string if outside
std::string relativeTo(const std::string& root, const std::string& path);
enum class LayoutPreset { Edit, Play, LookDev }; // [View] > [Layout]

class Editor {
public:
    bool init(Window& window, render::Renderer& renderer, const std::string& scenePath, asset::ModelCache& models);
    void shutdown();
    // Open project: [Build] menu, window title.
    void setProject(const ProjectInfo& project);
    void setEngineDirectory(const std::string& directory) { m_engineDirectory = directory; }
    // Token for the Penpot import server (before setProject, for automated tests. Random if empty)
    void setPenpotToken(const std::string& token) { m_penpotToken = token; }
    void setToolPaths(const std::vector<std::string>& folders) { m_toolPaths = folders; } // --tool-path
    // Same as pressing [Upgrade] (for automated tests)
    void requestUpgrade() {
        m_upgradeRequested = true;
        m_upgradeOffer = false;
    }
    const ProjectInfo& project() const { return m_project; }
    // Runs the project's build.bat in the background (ignored if already building).
    void startBuild(BuildKind kind);
    bool building() const { return m_buildPanel.running(); }
    // true once if the game code build finished successfully (the app reloads the DLL)
    bool takeGameCodeBuilt();
    bool takeGameCodeBuildFailed(); // true once if the game code build failed
    // Game DLL status: play button enablement and status bar text
    void setGameCodeStatus(bool loaded, const std::string& text);
    // Object kinds declared by the game DLL (reset to nullptr before unloading the DLL)
    void setGameTypes(const scene::TypeList* types) { m_gameTypes = types; }
    // Game tuning type (nullptr before unloading the DLL). Values being edited are kept as text and put back into the new type.
    void setTuningType(const reflect::TypeInfo* type) { m_tuningPanel.setTuningType(type); }
    // When play starts: writes unsaved tuning values to a temp file and returns the path (empty string if no type = default file)
    std::string writePlayTuning() { return m_tuningPanel.writePlayTuning(); }
    // true once if tuning changed (when playing solo, the app applies it to the game immediately)
    bool takeTuningChanged() { return m_tuningPanel.takeTuningChanged(); }
    const void* tuningData() const { return m_tuningPanel.tuning().data; }
    // View tuning (GameModule::viewTuningType, assets/view.tuning): edits show immediately in the running game (even with multiple players).
    void setViewTuningType(const reflect::TypeInfo* type) { m_tuningPanel.setViewTuningType(type); }
    reflect::View viewTuning() const { return m_tuningPanel.viewTuning(); }
    // Copy of view tuning read by join windows (written when multiplayer play starts, rewritten when values change)
    std::string writePlayViewTuning() { return m_tuningPanel.writePlayViewTuning(); }
    bool takeViewTuningChanged() { return m_tuningPanel.takeViewTuningChanged(); }
    static constexpr const char* kPlayViewTuningFile = TuningPanel::kPlayViewTuningFile;
    // Project settings type added by the game (GameModule::projectSettingsType, nullptr before unloading the DLL). Values are kept as text.
    void setProjectSettingsType(const reflect::TypeInfo* type) { m_projectPanel.setGameType(type); }
    // Opens the [Project Settings] window (for automated tests)
    void openProjectSettings() { openSettingsPage(SettingsPage::Project); m_settingsFrontFrames = 30; }
    // Definition data tables registered by the game ([Tables] panel, nullptr before unloading the DLL. Values are kept as text)
    void setTables(const reflect::TableList* tables) { m_tablesPanel.setTables(tables); }
    // When play starts: saves edited tables (the game reads them from files when a match starts)
    void saveTablesForPlay();
    void openTables() { openDataPage(DataPage::Table, 0); m_dataFrontFrames = 30; } // for automated tests: also brought to the front
    void openTuning() { openDataPage(DataPage::Tuning, 0); m_dataFrontFrames = 30; }
    void revealAsset(const std::string& relative);
    void openAssetGrid() { revealAsset("models/tree.glb"); m_assetThumb = 96.0f; m_assetsFront = 30; }
    Vec3 previewPoint() const { return m_cameraPos + cameraForward() * 10.0f; } // where previews play
    void setCompact() { m_prefs.compact = true; } // --compact, not saved
    // --input "move 500 300; down; wait 2; up; key ctrl+z; type abc; command Save; print": test input, one step per frame
    void setTestInput(const std::string& script);
    void openProblems() { openDebugPage(DebugPage::Problems); m_debugFrontFrames = 30; }
    void setEngineTables(const reflect::TableList& tables) { m_tablesPanel.setEngineTables(tables); }
    // Sound ([Sound] panel preview/voice list). Created and passed by the editor (nullptr = none)
    void setAudio(audio::Engine* audio) { m_audio = audio; }
    void openSounds() { openDataPage(DataPage::Sounds, 0); } // for automated tests
    // Feedback ([Feedback] panel preview). nullptr = none
    void setFeedback(feedback::Player* player) { m_feedback = player; }
    void setEventType(const reflect::TypeInfo* type) { m_feedbackPanel.setEventType(type); }
    void openFeedback() { openDataPage(DataPage::Feedback, 0); } // for automated tests
    // Game screen UI preview ([UI] panel): the game's view model list, drawn over the scene view in edit mode
    void setViewModels(const std::vector<const reflect::TypeInfo*>* types) {
        m_uiPanel.setViewModels(types);
        m_import.setViewModels(types);
    }
    void openUiPanel(const std::string& file = {}) {
        openPreviewPage(PreviewPage::Ui);
        if (!file.empty()) m_uiPanel.select(file);
    }
    bool selectUiNode(const std::string& name) { return m_uiPanel.selectNode(name); } // --ui-select (after --ui)
    void setUiPreviewState(int state) { m_uiPanel.setPreviewState(state); } // --ui-state
    void dragUiNode(const std::string& name, f32 dx, f32 dy) { m_uiPanel.queueTestDrag(name, dx, dy); } // --ui-drag
    // [UI] panel in edit mode: undo/save/delete and scene view clicks go to the screen being edited
    bool uiEditing() const { return m_uiPanelOpen && m_mode == Mode::Edit && m_uiPanel.editing(); }
    void renderUiPreview(render::Renderer& renderer, f64 dt) {
        if (m_uiPanelOpen && m_mode == Mode::Edit) m_uiPanel.render(renderer, m_viewport, dt);
    }
    // Offscreen image for the [Lookdev] window: after the main screen 3D, before UI (only when the window is open)
    void renderLookdev(render::Renderer& renderer, f64 dt);
    void openLookdev() { openPreviewPage(PreviewPage::LookDev); } // --lookdev (automated capture)
    // Camera previews in the scene view (edit mode, Esc stops): look through the selected virtual camera / play a cutscene row
    void lookThroughSelected() { m_lookThrough = m_selected; } // --look-through (automated capture)
    bool previewCutscene(const std::string& name);            // --cutscene <name>, View > Preview Cutscene
    // Reflection probe baking: after the main screen 3D, before UI. Draws one cube face offscreen per frame and reads it in endFrame
    void renderProbeBake(render::Renderer& renderer);
    void bakeProbes(u32 onlyId = 0); // 0 = all reflection probes in the scene (--bake-probes)
    // Capture for color grading ([Environment] panel button): renders the scene view camera without the LUT to art/grading/<scene>.png, with a neutral LUT next to it.
    // Grade it in an external tool (DaVinci Resolve, Photoshop etc.), export as .cube, and put it under assets/lut/. After the main screen 3D, before UI
    void renderGradeCapture(render::Renderer& renderer);
    bool bakingProbes() const { return !m_probeQueue.empty(); }
    // Simulation state during play ([State] panel). An empty View when play ends.
    void setStateView(reflect::View view) { m_stateView = view; }

    // Call every frame after pumpMessages: builds the UI and handles edit input.
    void update(f64 dt);
    // Fills the edit mode 3D scene (scene objects + selection highlight) and camera.
    void buildSceneView(render::FrameParams& frame, render::DrawList& drawList) const;
    void setDebugView(render::DebugView view) { m_debugView = view; } // --view-mode (automated capture)
    // Automated runs (--frames, --render-golden): turns off dynamic resolution (so images don't vary with GPU speed, same as the game's automated runs)
    void setAutomated(bool automated) { m_automated = automated; }
    // Graphics regression test (--render-golden): opens the scene and renders the whole window from a fixed camera (degrees) without UI
    bool openSceneForShot(const std::string& path) { return loadScene(path); }
    void buildShot(Vec3 position, f32 yawDegrees, f32 pitchDegrees, f32 width, f32 height, render::FrameParams& frame,
                   render::DrawList& drawList);
    // Call after drawing 3D, before endFrame. frameActive = false if no renderer frame was opened.
    void renderUi(bool frameActive);

    Mode mode() const { return m_mode; }
    void setMode(Mode mode);
    bool takePlayRequest();
    bool takeStopRequest();
    void requestPlay() { m_playRequest = true; } // if the game DLL isn't there yet, waits until it loads (for automated tests)
    // Replay viewing ([File] > [Open Replay], --replay): the next play replays this recording. Taken after takePlayRequest
    void requestReplay(const std::string& path) {
        m_replayPath = path;
        m_playRequest = true;
    }
    std::string takeReplayPath() { return std::exchange(m_replayPath, {}); }
    // Player count: 1 = solo, 2~4 = the editor becomes host and opens more join windows
    u32 playPlayers() const { return m_playPlayers; }
    void setPlayPlayers(u32 players) { m_playPlayers = players < 1 ? 1 : players > 4 ? 4 : players; }
    bool playClientBots() const { return m_playClientBots; }
    bool shadowCheck() const { return m_shadowCheck; }
    // Debug controls for the running simulation (pause/rewind/shadow simulation). nullptr when play ends
    void setSimDebug(game::SimDebug* debug, bool canPause);
    // Notification window in the center of the screen (closes on OK)
    void showMessage(const std::string& text) { m_message = text; }
    void setPlaySample(const game::FrameSample& sample) { m_playSample = sample; }
    void setPlayStatus(const std::string& text) { m_playStatus = text; }

    const scene::Scene& scene() const { return m_scene; }
    render::Viewport viewport() const { return m_viewport; }
    bool gameKeyboardAllowed() const; // whether the UI isn't using the keyboard during play
    bool gameMouseAllowed() const;    // whether the mouse is over the viewport during play
    bool quitRequested() const { return m_quit; }
    void saveBackupIfDirty();

    void openPalette(const std::string& text); // for automated tests/screenshots (--palette)
    void queueCommand(const std::string& name) { m_queuedCommands.push_back(name); } // --command
    void selectByIndex(int index, bool add = false); // for automated tests/screenshots (-2 = scene row)

private:
    void applyRenderStyle(const game::ProjectSettings& settings);
    struct Snapshot {
        std::vector<scene::Entity> entities;
        scene::Environment environment;
        u32 nextId = 1;
        u32 selected = 0;
        std::string label; // name shown in the [Edit] > [History] window ("Ramp change", status message)
        struct TerrainSamples {
            std::string path;
            asset::Heightmap map;
        };
        std::vector<TerrainSamples> terrains; // heightmaps of the scene's terrains (brush strokes are undone with the scene)
        struct ScatterItems {
            std::string path;
            std::vector<asset::ScatterInstance> items;
        };
        std::vector<ScatterItems> scatters; // scatter layer instances
    };

    void drawMenuBar();
    void drawStatusBar();
    void drawHierarchy(bool editing);
    void drawEntityNode(size_t index, const std::unordered_map<u32, std::vector<size_t>>& children);
    void reparent(u32 entityId, u32 parentId); // changes the parent while keeping the world position (0 = top level)
    void drawInspector(bool editing);
    void drawEnvironment(); // inside the inspector, when the scene row is selected
    void saveAll();         // Ctrl+S: scene, tuning, project settings, tables
    void takePanelStatus(); // moves status messages left by panels to the status bar
    void drawState();
    void drawPopups();
    void drawPlayControls();
    void reportSimProblems();
    void updateAutosave(f64 dt);
    void updateHotReload(f64 dt); // reload changed model/scene files (edit mode)
    void focusPanel(const char* window);
    void focusInspector();
    void drawSceneWindow(unsigned int dockId); // scene view tab: sets m_viewport
    bool sceneMouseBlocked() const;
    bool m_sceneVisible = true;
    bool m_sceneHovered = false;
    int m_sceneFrontFrames = 0;
    void focusPanelNow(); // called right before that window's Begin
    void drawGizmo();
    void drawBuildLog();
    void setupDefaultLayout(u32 dockId, LayoutPreset preset);

    void updateCamera(f64 dt);
    void handlePicking();

    scene::Entity* selected();
    // Multi-select: m_selected is the primary selection (inspector/gizmo), m_multi the others selected with it.
    // If m_selected is changed elsewhere, m_multi is cleared next frame (when it differs from m_multiOwner)
    bool isSelected(u32 id) const;
    std::vector<u32> selectionRoots() const; // selected objects that aren't descendants of other selected objects (primary first)
    void toggleSelect(u32 id);               // Shift click
    void selectMany(const std::vector<u32>& ids, bool add);
    void moveOthersWith(const scene::Transform& before, const scene::Transform& after); // by as much as the gizmo moved the primary selection
    std::vector<u32> m_multi;
    u32 m_multiOwner = 0;
    bool m_boxSelecting = false;
    Vec2 m_boxStart;
    void addEntity(const std::string& type);
    const reflect::TypeInfo* entityType(const scene::Entity& e) const; // nullptr for unknown kinds (no game code)
    bool isMarker(const scene::Entity& e) const;
    bool isArea(const scene::Entity& e) const;
    // Scene tree visibility/lock: not in the scene file but in the project's build\editor\<scene>.view (doesn't change the scene hash or others' views)
    bool isHidden(const scene::Entity& e) const; // itself or an ancestor is hidden
    bool isLocked(const scene::Entity& e) const; // itself or an ancestor is locked (not selectable by viewport click)
    std::string viewStatePath() const;
    void loadViewState();
    void saveViewState() const;
    const char* entityIcon(const scene::Entity& e) const;
    std::unordered_set<u32> m_hidden, m_locked;
    char m_treeSearch[96] = {};
    bool m_treeSearchFocus = false; // Ctrl+F
    u32 m_treeRevealed = 0;         // last selection expanded and shown in the tree
    std::vector<const reflect::TypeInfo*> allTypes() const; // built-in kinds + game kinds (by label)
    void drawAddMenuItems();
    void drawObjectFields(scene::Entity& e);
    void addModelEntity();
    void addModelEntity(const std::string& path, const Vec3* at); // at: floor point to place on (nullptr = in front of the camera)
    // [Assets] window (EditorAssets.cpp)
    void drawAssetBrowser();
    void drawAssetFolder(const std::string& root, const std::string& relative);
    void handleAssetDrop();
    void renameAsset(const std::string& from, const std::string& to);
    bool m_assetsOpen = true;
    int m_assetsFront = 0; // frames left to bring the tab to the front
    std::string m_assetFolder;   // relative to assets ("models")
    std::string m_assetSelected; // file relative to assets
    // Folder listings and missing model references, read again once a second rather than every frame
    std::map<std::string, std::vector<std::string>> m_assetDirs, m_assetFiles; // by native folder path
    std::vector<std::string> m_assetMissing;
    double m_assetScanTime = -1000.0;
    std::string m_assetRenameFrom;
    char m_assetRenameBuffer[260] = {};
    std::string chooseModel(); // file dialog → path relative to assets (empty string on cancel)
    void duplicateSelected();
    // Prefabs (scene/Scene.h): save the selection to assets/prefabs/, place copies (at: floor point, nullptr = in front of the camera)
    void saveSelectionAsPrefab();
    void placePrefab(const std::string& relative, const Vec3* at);
    void deleteSelected();
    void focusSelected();
    void newScene();
    void openSceneDialog();
    void openReplayDialog();
    void saveScenario(u32 tick);
    bool saveScene();
    bool saveSceneAs();
    bool loadScene(const std::string& path);

    Snapshot capture() const;
    void restore(const Snapshot& snapshot);
    void pushUndo(Snapshot snapshot);
    void drawHistory();
    // [Scenes] window (scene list in assets/scenes, set start scene, open) and [File] > [Recent Scenes] (per project build\editor\recent_scenes.txt)
    void drawSceneList();
    void openSceneFile(const std::string& path); // unsaved changes are kept as an autosave before opening
    void rememberRecentScene(const std::string& path);
    void loadRecentScenes();
    bool m_sceneListOpen = false;
    // Play from here (Shift+F5, solo only): during play, spawn points move under the camera (or to the selected spawn) and are restored on stop
    bool m_playFromHere = false;
    std::string m_replayPath; // replay to watch on the next play
    std::vector<std::pair<u32, scene::Transform>> m_spawnBackup;
    void moveSpawnsHere();
    std::vector<std::string> m_recentScenes; // absolute paths, most recent first  // [Edit] > [History]: undo list, click to go to that point
    bool m_historyOpen = false;
    int m_undoLabelFrame = -1; // this frame's setStatus becomes the name of the undo just pushed
    bool m_scaleLock = false;  // inspector scale ratio lock
    bool transformRow(const char* label, const char* id, f32 v[3], f32 speed, f32 lo, f32 hi, const char* format,
                      f32 defaultValue, bool* lockRatio = nullptr);
    void undo();
    void redo();
    void markChanged() { m_changed = true; }
    // Status bar + notification at the viewport's bottom right. Failures stay until closed, clicking opens that window (build log/log)
    enum class NoticeOpen : u8 { None, Log, BuildLog };
    // Command table (EditorCommands.cpp): shared by menus, shortcuts, palette, and F1. name is the English source (tr key)
    struct Command {
        const char* name;
        int chord;  // ImGuiKeyChord (0 = no shortcut)
        int chord2; // second shortcut doing the same thing
        std::function<bool()> enabled;
        std::function<void()> run;
    };
    void buildCommands();
    const Command* findCommand(const char* name) const;
    std::string shortcutText(const Command& c) const;
    void commandMenuItem(const char* name, const bool* selected = nullptr);
    void runShortcuts();
    void drawCommandPalette();
    void drawShortcutHelp(); // F1
    std::vector<Command> m_commands;
    bool m_paletteOpen = false;
    char m_paletteSearch[128] = {};
    int m_paletteCursor = 0;
    std::string m_paletteInitial;
    std::vector<std::string> m_queuedCommands;
    void setStatus(const std::string& text, bool failed = false, NoticeOpen open = NoticeOpen::Log);
    void drawNotices();
    void drawPlayFrame();     // viewport border during play
    void drawEmptyViewport(); // what to do next when there's no game DLL yet
    void captureGpuFrame(); // View menu/F11 (rhi/GpuCapture.h)
    Vec3 cameraForward() const;
    void updateTitle();
    void pollBuild();
    void startLayoutCheck();
    void pollLayoutCheck();

    Window* m_window = nullptr;
    render::Renderer* m_renderer = nullptr;
    asset::ModelCache* m_models = nullptr;
    ImGuiLayer m_imgui;

    scene::Scene m_scene;
    std::string m_scenePath;
    bool m_dirty = false;
    u32 m_selected = 0; // entity id (0 = none)
    // Camera previews (camera/Brain.h): the scene view shows m_previewPose instead of the editor camera while one runs
    void updateCameraPreview(f64 dt, f32 aspect);
    void drawCameraPreviewLabel();
    bool previewing() const { return m_lookThrough != 0 || !m_cutscenePreview.empty(); }
    u32 m_lookThrough = 0;         // virtual camera entity id (0 = off)
    std::string m_cutscenePreview; // cutscene row being played (empty = off)
    camera::Brain m_previewBrain;
    asset::Cutscenes m_previewCutscenes;
    camera::CameraPose m_previewPose;
    f32 m_previewSeconds = 0.0f;

    Mode m_mode = Mode::Edit;
    render::DebugView m_debugView = render::DebugView::None; // [View] > view mode (edit screen only)
    bool m_playRequest = false;
    bool m_stopRequest = false;
    bool m_quit = false;
    std::string m_playStatus;
    struct Notice {
        std::string text;
        f64 time = 0.0;
        bool failed = false;
        NoticeOpen open = NoticeOpen::None;
    };
    std::vector<Notice> m_notices;
    f64 m_time = 0.0;
    game::FrameStats m_perf; // status bar fps (mealkit/game/FrameStats.h)

    Vec3 m_cameraPos{0.0f, 24.0f, -48.0f};
    f32 m_cameraYaw = 0.0f;
    f32 m_cameraPitch = -0.42f;
    f32 m_cameraSpeed = 15.0f;
    bool m_looking = false;
    bool m_orbiting = false;      // Alt + left drag: orbit around the selected object (or 10 m ahead if none)
    Vec3 m_orbitPivot;
    bool m_ortho = false;         // orthographic (toolbar/axis gizmo)
    f32 m_orthoHalfHeight = 10.0f; // half of the orthographic view height (m), changed with the wheel
    // Camera bookmarks (Ctrl+1~9 save, 1~9 go): per scene in build\editor\<scene>.view
    struct CameraBookmark {
        bool set = false;
        Vec3 position;
        f32 yaw = 0.0f, pitch = 0.0f;
    };
    CameraBookmark m_bookmarks[9];
    void handleBookmarks();
    Mat4 projection(f32 aspect) const;
    void mouseRay(f32 mouseX, f32 mouseY, Vec3* origin, Vec3* dir) const; // viewport coordinates → world ray
    Mat4 m_view;
    Mat4 m_proj;
    render::Viewport m_viewport;

    int m_gizmoOp = 0; // 0 move, 1 rotate, 2 scale
    bool m_gizmoLocal = false;
    bool m_snapOn = false;   // toolbar snap: when on, snaps without Ctrl (Ctrl inverts)
    bool m_showGrid = true;  // edit screen floor grid (spaced by the snap move unit)
    void drawViewportToolbar(); // toolbar at the viewport's top left + axis gizmo at the top right
    void addGrid(render::DrawList& out) const;
    // First object the ray hits (excluding hidden/locked/skip). 0 if none
    u32 raycast(Vec3 origin, Vec3 dir, f32* distance, const std::vector<u32>& skip = {}) const;
    void pickBounds(const scene::Entity& e, const scene::Transform& w, Vec3* center, Vec3* half) const;
    void dropToFloor();             // places selected objects on the object below (or y = 0 if none)
    void alignSelection(int axis);  // moves the rest to the primary selection's position on that axis
    void distributeSelection();     // even spacing along the most spread-out axis

    // Terrain editing (EditorTerrain.cpp)
    asset::Terrain* terrainOf(const scene::Entity& e) const; // nullptr if not a terrain or its heightmap is unreadable
    void setupNewTerrain(scene::Entity& e);
    void drawTerrainInspector(scene::Entity& e);
    bool handleTerrainBrush();
    void setupNewScatter(scene::Entity& e);
    void drawScatterInspector(scene::Entity& e);
    bool handleScatterBrush(scene::Entity& e);
    bool m_scatterErase = false;
    u64 m_scatterSeed = 1;
    void captureTerrains(Snapshot& s) const;
    void restoreTerrains(const Snapshot& s);
    bool m_brushOn = false;
    int m_brush = 0; // asset::TerrainBrush
    f32 m_brushRadius = 6.0f;
    f32 m_brushStrength = 0.3f;
    bool m_brushStroke = false;
    bool m_brushHit = false;
    Vec3 m_brushPoint;
    u16 m_flattenTo = 0;

    std::vector<Snapshot> m_undo;
    std::vector<Snapshot> m_redo;
    Snapshot m_pending;
    bool m_editActive = false;
    bool m_changed = false;

    struct TreeAction {
        enum Kind { None, Reparent, Duplicate, SavePrefab, Delete, SetFolder, RenameFolder, Ungroup } kind = None;
        u32 entity = 0;
        u32 parent = 0;
    };
    std::string m_treeActionFolder; // folder of SetFolder/RenameFolder/Ungroup
    std::string m_treeFolderTo;     // RenameFolder: new path
    u32 m_folderDialogEntity = 0;   // "Move to Folder..." dialog
    std::string m_folderDialogFrom; // "Rename Folder..." dialog
    bool m_folderDialogOpen = false;
    char m_folderBuffer[128] = {};
    void drawTreeFolders(const std::string& prefix, const std::vector<size_t>& roots, const std::unordered_map<u32, std::vector<size_t>>& children);
    void setFolder(u32 entity, const std::string& folder);
    void renameFolder(const std::string& from, const std::string& to);
    TreeAction m_treeAction; // structure change chosen in the scene tree (handled after drawing the whole tree)

    char m_nameBuffer[128] = {};
    u32 m_nameBufferId = 0;
    std::string m_title;
    bool m_layoutReset = false;
    LayoutPreset m_layoutPreset = LayoutPreset::Edit;
    bool m_sceneRow = false; // the "Scene" row in the scene tree is selected (inspector = environment)

    const scene::TypeList* m_gameTypes = nullptr;
    TuningPanel m_tuningPanel;
    reflect::View m_stateView;
    const char* m_focusPanel = nullptr; // panel to bring to front (when the mode changes)
    int m_focusFrames = 0;
    u32 m_playPlayers = 1;
    bool m_shadowCheck = true;
    game::SimDebug* m_simDebug = nullptr;
    bool m_canPause = false;
    bool m_shadowReported = false;
    bool m_replayReported = false;
    bool m_playClientBots = true;
    std::string m_message;     // notification window contents
    std::string m_recoverPath; // autosave file to ask about recovering
    f64 m_autosaveTimer = 0.0;
    f64 m_hotReloadTimer = 0.0;
    game::RenderStyleState m_renderStyle;
    bool m_automated = false;
    std::vector<u32> m_probeQueue;     // probe object ids to bake (from the front)
    u32 m_probeFace = 0;               // face being drawn now (0..5)
    bool m_probeCapturePending = false; // endFrame read the face drawn last frame into m_probeCapture
    rhi::RenderTarget m_probeTarget;
    u32 m_probeTargetSize = 0;
    Image m_probeCapture;
    std::vector<f32> m_probeFaces; // 6 faces × size² × linear rgb
    bool m_gradeRequested = false, m_gradeCapturePending = false; // grading capture: button pressed / drawn last frame
    rhi::RenderTarget m_gradeTarget;
    u32 m_gradeTargetSize[2] = {};
    Image m_gradeCapture;
    mutable std::vector<render::ReflectionProbe> m_probeList; // filled every frame by buildSceneView
    mutable std::vector<render::PointLight> m_lightList;      // scene point lights (buildSceneView/probe baking)
    f64 m_animTime = 0.0; // skinned model preview clock (runs in edit mode, 0 for graphics regression tests)
    void addSceneObjects(render::DrawList& out, Vec3 eye, bool gameView) const; // gameView: only what the game shows, no highlight
    void fillEnvironment(render::FrameParams& frame) const;
    u32 probeMaxCount() const;
    AssetThumbnails m_thumbnails; // pictures in the [Assets] grid
    f32 m_assetThumb = 0.0f;      // grid cell size, 0 = list
    LookdevPanel m_lookdev; // [Lookdev] window: the selected object's model under fixed lighting on a stand
    bool m_lookdevOpen = false; // project settings lighting model/toon ramp (passed again when hot reload changes them)
    unsigned long long m_sceneTime = 0; // scene file time at open/save (whether it changed outside)
    bool m_sceneChangedWarned = false;
    bool m_autosavePending = false; // something changed since the last autosave

    ProjectInfo m_project;              // project settings saved on disk
    ProjectSettingsPanel m_projectPanel; // [Project Settings] window
    ModulesPanel m_modulesPanel;         // [Modules] window (tools\module.ps1)
    std::string m_engineDirectory;
    bool m_upgradeOffer = false;     // the project template differs from the engine version (asked once)
    bool m_upgradeRequested = false; // start the upgrade when the build finishes
    BuildPanel m_buildPanel;
    LogPanel m_logPanel;
    TestsPanel m_testsPanel;
    PerfPanel m_perfPanel;
    game::FrameSample m_playSample; // PlaySession's simulation/network numbers during play ([Performance] window)
    Preferences m_prefs;
    PreferencesPanel m_prefsPanel;
    bool m_gameCodeBuilt = false;
    bool m_gameCodeFailed = false;
    bool m_gameLoaded = false;
    std::string m_gameStatus;
    ChildProcess m_layoutCheck; // tools\check_layout.ps1 (when opening a project)
    TablesPanel m_tablesPanel;
    audio::Engine* m_audio = nullptr;
    SoundPanel m_soundPanel;
    feedback::Player* m_feedback = nullptr;
    FeedbackPanel m_feedbackPanel;
    // [Data] window (EditorData.cpp)
    enum class DataPage { Tuning, Table, Feedback, Effects, Sounds };
    void openDataPage(DataPage page, int index);
    void toggleDataPage(DataPage page, int index);
    void drawDataHub(f64 dt, bool editing);
    bool dataCanStep(bool back) const;
    void dataStep(bool back);
    bool m_dataOpen = false;
    bool m_dataFocused = false; // Ctrl+Z goes to the open document instead of the scene
    DataPage m_dataPage = DataPage::Tuning;
    int m_dataIndex = 0;
    int m_dataFrontFrames = 0;
    // [Problems] window
    struct Problem {
        bool error = true; // false = warning
        std::string text;
        DataPage page = DataPage::Tuning; // clicking opens this document (doc >= 0) ...
        int doc = -1;
        u32 entity = 0; // ... or selects this object
    };
    void updateProblems(f64 dt);
    void runTestInput();
    char m_fieldSearch[64] = {};
    std::string m_layoutLoad; // saved layout to read before the next frame
    char m_layoutName[48] = {};
    // [Assets] > Find Uses
    void findAssetUses(const std::string& relative);
    std::vector<std::string> m_assetUses; // files under assets, relative
    std::string m_assetUsesOf;
    bool m_assetUsesOpen = false;
    // Graphs of state values on the [State] page, one sample per frame during play
    struct StateGraph {
        std::string path;
        std::vector<float> samples;
        size_t next = 0;
    };
    std::vector<StateGraph> m_graphs;
    char m_graphPath[96] = {};
    void updateStateGraphs();
    void drawStateGraphs();
    std::vector<std::string> m_testInput;
    size_t m_testStep = 0;
    int m_testWait = 0;
    float m_testX = 0.0f, m_testY = 0.0f;
    void drawProblems();
    std::vector<Problem> m_problems;
    f64 m_problemTimer = 1.0; // check on the first frame
    enum class DebugPage { Log, Problems, Build, Performance, Sound, State, Tests };
    // [Settings] and [Previews] windows (EditorData.cpp)
    enum class SettingsPage { Project, Preferences, Modules, Tools, Shortcuts };
    enum class PreviewPage { Ui, LookDev, Cutscene };
    void openSettingsPage(SettingsPage page);
    void openPreviewPage(PreviewPage page);
    void drawSettingsHub();
    void drawPreviewHub();
    void drawUnsavedStatus(); // status bar: count and a list with save and revert for each document
    bool m_settingsOpen = false;
    SettingsPage m_settingsPage = SettingsPage::Project;
    int m_settingsFrontFrames = 0;
    bool m_previewOpen = false;
    PreviewPage m_previewPage = PreviewPage::Ui;
    int m_previewFrontFrames = 0;
    bool m_revertSceneArmed = false; // a second click reverts: the scene's undo steps don't survive it

    void openDebugPage(DebugPage page);
    void toggleDebugPage(DebugPage page);
    void drawDebugHub();
    bool m_debugOpen = true;
    DebugPage m_debugPage = DebugPage::Log;
    int m_debugFrontFrames = 0;
    bool m_compactApplied = false;
    bool updateFieldHandles(); // viewport handles of gizmo= fields; true while they own the mouse
    u32 m_handleEntity = 0;
    std::string m_handleField;
    struct Badge {
        bool error = false;
        std::string text;
    };
    std::unordered_map<u32, Badge> m_badges;   // scene tree: objects with a problem
    std::unordered_set<u32> m_differsFromDefault; // scene tree: objects with fields changed from their kind's defaults
    std::vector<std::string> allAssetFiles() const;
    std::vector<std::string> m_paletteAssets;
    std::vector<std::string> m_paletteRecent;
    UiPanel m_uiPanel;
    bool m_uiPanelOpen = false;
    ImportServer m_import; // Penpot plugin import (penpot/ service of the live connection server)
    live::Server m_live;   // live connection (contract between tools, starts when a project opens. Closed before m_import)
    std::vector<ToolInfo> m_tools; // [Tools] menu (searched when a project opens and on [Rescan])
    std::vector<std::string> m_toolPaths;
    std::string m_penpotToken;
    bool m_viewTableDirty = false;
    f64 m_viewTableTimer = 0.0;
    std::string m_layoutOutput;
};

} // namespace mealkit::editor
