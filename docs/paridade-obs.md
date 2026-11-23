# Vertical editor = exact copy of the OBS editor

Owner's rule (final, non-negotiable): the vertical editor (preview dock, scenes dock, sources dock) must be
**identical to OBS's own** — every function, menu entry, shortcut, icon, toolbar, dialog and behaviour. The only
difference is that it edits the vertical canvas (1080x1920) instead of the main one. Never ship a subset, never ask
the owner whether an OBS feature should be included: if OBS has it, we have it. Only leave something out when it is
technically impossible for a non-main canvas (e.g. "Resize output (source size)", "Show in Multiview"), and list it in
the "Impossible" section at the end with the reason.

Reference = the source of the OBS version installed on the owner's PC: **OBS 33.0.1**, unpacked at
`.deps/obs-studio-33.0.1` (git-ignored). The plugin builds against the 31.1.1 SDK (`.deps/obs-studio-31.1.1`); if
ported code needs a libobs/frontend API newer than 31.1.1, adapt or resolve it at runtime — do not change the SDK.

## Approach: port OBS code, don't re-imagine it
Copy the OBS frontend classes into `src/obsport/` (GPL, keep the OBS copyright header + "Adapted for the LiveHub
vertical canvas"), renaming classes with an `LH` prefix to avoid symbol clashes with obs64.exe, and replace every
`OBSBasic::Get()`/main-canvas access with a small adapter (`src/obsport/VerticalContext.hpp`) that returns the
vertical canvas, its current scene, its preview etc. Keep OBS's logic line-for-line wherever possible so behaviour
matches exactly. Strings: OBS's own locale via `obs_frontend_get_locale_string(key)` (same keys as OBS's
`QTStr(key)`). Icons/theme: use the same objectNames, `class` properties and Q_PROPERTYs OBS uses so the active OBS
theme styles our widgets the same way (theme files: `C:\Program Files\obs-studio\data\obs-studio\themes`).
Source-type icons: read the Q_PROPERTYs of the OBS main window (`imageIcon`, `cameraIcon`, ... see
`frontend/widgets/OBSBasic.hpp` + `OBSBasic_Icons.cpp`), including `GetCustomIcon` behaviour.

Selection = libobs scene-item selection (`obs_sceneitem_select`, signal `item_select/item_deselect`), exactly like
OBS (multi-select included). Undo/redo = OBS's own stack via `obs_frontend_add_undo_redo_action`, with the same
action names OBS uses (`Undo.*` locale keys), for every operation OBS makes undoable.

## Components (each must reach 100% parity)
1. **Scenes dock** — `frontend/components/SceneTree.*`, scenes part of `frontend/forms/OBSBasic.ui`,
   `frontend/widgets/OBSBasic_Scenes.cpp` (context menu, add/duplicate/rename/remove, order, grid mode, filters,
   screenshot, projectors, per-scene transition override, scene hotkeys), undo.
2. **Sources dock** — `SourceTree*`, `VisibilityItem*`, `OBSBasic_SceneItems.cpp`, `OBSBasic_Clipboard.cpp`,
   `dialogs/OBSBasicSourceSelect.*`, `CreateSourcePopupMenu`/`CreateAddSourcePopupMenu` (all entries: groups,
   copy/paste source & filters & transform, transform submenu, scale filtering, blending, deinterlacing, show/hide
   transitions, hide in mixer, colour, projectors, screenshot, interact, filters, properties ...), drag & drop,
   rename editor, undo, keyboard shortcuts.
3. **Preview dock** — `OBSBasicPreview.*` (multi-select, rubber band, hover, crop, rotation, spacing helpers,
   overflow, zoom/scroll/space-pan, lock preview, preview scaling), the zoom bar under the preview
   (`- NN% + Dimensionar à janela ▼`), the context source toolbar under it (`OBSBasic_ContextToolbar.cpp`,
   `components/*SourceToolbar*`), preview context menu, preview projector/screenshot, double-click = properties.
4. **Vertical transitions** — the vertical canvas switches scenes with the transition currently selected in OBS
   (same type/settings/duration, private instance per canvas), plus per-scene overrides, like OBS.

## Verification (mandatory before any report)
Self-test mode: when env var `LHMS_SELFTEST=<dir>` is set, the plugin, after `OBS_FRONTEND_EVENT_FINISHED_LOADING`,
grabs PNGs (`QWidget::grab`) of OBS's own Scenes/Sources docks, preview area and their context menus (opened
programmatically, grabbed via `QApplication::activePopupWidget()`, then closed — read-only, never triggers an
action) and the same for our docks, writes them to `<dir>`, and logs `[LHMS selftest] done`. Compare pairs side by
side; differences other than vertical/horizontal content are bugs. The self-test must never modify scenes, sources
or settings.

Testing: use an isolated **portable** OBS copy (`portable_mode.txt`) with the conflicting third-party plugins
disabled in `config/obs-studio/plugin_manager/modules.json`, streaming pointed to a local RTMP server and
obs-websocket on a separate port. Portable mode only loads plugins from the copy itself, so install the plugin in the
classic layout (`obs-plugins/64bit/LiveHub-multistream.dll` + `data/obs-plugins/LiveHub-multistream/`). Close OBS
gracefully before replacing the DLL. Never start streaming or recording, and back up the scene collections first.

## Progress
(each task appends: what was ported, commit, self-test result)

### Task 1+2 (2026-11-22): self-test harness + Sources dock port
- `src/selftest.cpp` (LHMS_SELFTEST=<dir>): PNGs of OBS/our docks and menus (+submenus) and `menus.txt` text tree.
- `src/obsport/`: SourceTree, SourceTreeItem (+Model/Delegate/Label), SourceSelect dialog (+FlowLayout/Frame, SourceSelectButton,
  Thumbnail*, trimmed ScreenshotObj, .ui via AUTOUIC), `LHSourcesDock` (OBSBasic sources dock: actions, order/transform menus,
  CreateSourcePopupMenu, clipboard, ColorChange, show/hide transition menus, UpdateEditMenu, shortcuts), `VerticalContext` adapter
  (current scene = active vertical scene, undo via obs_frontend_add_undo_redo_action with the Undo.* strings).
- SourceTree/SourceTreeItem/SourceSelectButton keep the OBS class names (themes style them by type selector).
- Icons: missing because OBS tints the label pixmap with `idian::Utils::applyColorToIcon`; ported.
- vdocks.cpp `Hub` is now a facade over libobs selection (obs_sceneitem_select + item_select/item_deselect signals).
- Self-test (st1): Sources empty-area menu identical; item menu identical except source-dependent entries (the first item of the
  two scenes are different source types) and the Impossible entry below.

### Task A+B (2026-11-22): Scenes dock + vertical transitions
- `src/obsport/`: `LHSceneTree` (SceneTree incl. grid mode, class name kept), `LHSceneRenameDelegate`, `LHNameDialog` (OBS NameDialog),
  `LHScenesDock` (scenes dock from OBSBasic.ui + OBSBasic_Scenes.cpp: toolbar, F2/Del shortcuts, add/duplicate/rename/remove with OBS
  validation and Undo.* actions, order submenu, drag & drop reorder, copy/paste filters, scene screenshot, projector submenu, per-scene
  transition override menu, grid/list toggle; LiveHub link entry is the last one), `LHProjector` (OBSProjector for a vertical scene).
  vdocks.cpp `DockCenas` removed; dock id `livehub-vertical-cenas` kept. Grid mode is stored in its own key (`LHVerticalSceneGridMode`).
- Engine (`vertical.c`): channel 0 of the canvas is a private transition (clone of OBS's current transition: same id/settings/duration,
  refreshed on TRANSITION_CHANGED/DURATION_CHANGED/LIST_CHANGED and on every scene change), `vertical_cena_ativar`, the vendor request
  `vertical.cena.ativar` and the link auto-switch use it. Per-scene override = keys `transition` / `transition_duration` of the scene's
  private settings (same as OBS; saved with the scene). Scene hotkeys `OBSBasic.SelectScene` are registered for every vertical scene.
  New API: `vertical_cena_ativar_cortando`, `vertical_cena_restaurar` (undo of a removal), `vertical_transicoes_atualizar`.
- Self-test (st2): scene item/empty menus and the Order, Projector and Transition override submenus are identical to OBS's (the unordered_map
  order of the transitions also matches); only differences are the Impossible item below and the LiveHub link entry.

### Task C (2026-11-22): Preview dock
- `src/obsport/`: `LHQTDisplay` (OBSQTDisplay), `LHPreview` (OBSBasicPreview, line-for-line: overlay, multi-select, rubber band, hover,
  crop/rotate/stretch/snap with the user's OBS settings, spacing helpers, overflow settings, zoom/scroll/space+middle pan, lock, scrollbars,
  nudge keys), `LHPreviewDock` + `LHPreviewDockOBS.cpp` (canvasEditor/contextContainer of OBSBasic.ui via `forms/LHPreviewDock.ui`,
  OBSBasic_Preview.cpp: RenderMain/ResizePreview/scaling menu/Enable Preview/Lock, OBSBasic_ContextToolbar.cpp), `OBSPreviewScalingComboBox/Label`,
  `SourceToolbar` + Color/Image/Text/Browser/Game/Device/Display/Window/Audio/AppAudio toolbars + `MediaControls` (all ported), `LHDisplayHelpers.hpp`.
- Preview menu = `LHSourcesDock::CreateSourcePopupMenu(idx, preview=true)` (same function as the Sources menu; preview-only entries: Enable Preview,
  Lock Preview, Preview Scaling submenu, Preview Projector submenu, Screenshot (Preview)). `LHProjector::OpenPreview` = OBS "Preview" projector
  (renders the canvas output texture).
- Settings that OBS keeps per main preview use our own keys in the user config (`LHVPreviewEnabled/Locked/ScalingEnabled/ScalingLevel/ScrollX/ScrollY`);
  the ShowContextToolbars / ShowSafeAreas / SpacingHelpersEnabled / Overflow* / Accessibility colours / snapping settings of OBS are honoured as-is.
- The preview follows `vertical_ui_hook_*`: display + GPU objects are destroyed (display first) before the canvas teardown; the graphics thread only reads
  cached state (canvas info, scene) - no engine lock, no Qt.
- vdocks.cpp: old Hub/Visor/DockPreview and the hand-written menus/dialogs/helpers removed (3000 -> ~250 lines); only the LiveHub "Transmitir vertical"
  button (footer under the context bar) and the dock registration remain.
- Self-test (st3): `lhms_*` sections for preview area, zoom bar, context bar and the preview menus (item + empty, all submenus). Empty-area menu identical
  to OBS; item menu identical except source-type dependent entries (the two scenes' first items differ), the lock/zoom state (own keys) and the Impossible
  entry below. `LHMS_SELFTEST_SELECT=1` leaves the first item selected (to look at the overlay/context bar with a window screenshot).
- OBS has no double-click handler in the preview (QWidget default = a second press), so none was added.

## Impossible for a non-main canvas
- "Resize output (source size)" (ResizeOutputSizeOfSource): resizes the main video/output; the vertical canvas is fixed 1080x1920.
- "Show in Multiview" (scene context menu): OBS's multiview only draws main-canvas scenes; the vertical scenes cannot be added to it.
- Scene projector: obs_frontend_open_projector cannot find canvas scenes, so `LHProjector` renders the vertical scene itself; it is not
  saved/restored with OBS's saved projectors (OBS only stores projectors of its own scenes).
- Settings edits of OBS's current transition (properties dialog) are picked up at the next scene event, not instantly (no frontend event).
- "Resize output (source size)" in the preview item menu (same entry as the Sources menu, see above).
- Preview/Program labels, Studio Mode preview (`IsPreviewProgramMode`) and the Multiview: the vertical canvas has no Studio Mode.
