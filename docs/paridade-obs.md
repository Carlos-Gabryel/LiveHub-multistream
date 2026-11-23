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

Restarting the owner's OBS for a test: close it gracefully (`(Get-Process obs64).CloseMainWindow()`, wait for exit),
install (`cmake --install build_x64 --config RelWithDebInfo --prefix C:\ProgramData\obs-studio\plugins`, then delete
only `C:\ProgramData\obs-studio\plugins\LiveHub-multistream\bin\64bit\*.pdb`), start
`C:\Program Files\obs-studio\bin\64bit\obs64.exe` with that folder as working directory. Never start streaming or
recording. Scene collection backup: `F:\Backups\obs-studio-20261122-2214`.

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

## Impossible for a non-main canvas
- "Resize output (source size)" (ResizeOutputSizeOfSource): resizes the main video/output; the vertical canvas is fixed 1080x1920.

