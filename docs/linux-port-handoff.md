# FMVibe on Linux — session record and handoff

**What this is.** A record of the session that ported FMVibe's CLAP and VST3 plugins to Linux: what was built, why it
is built that way, what was measured, what broke and how it was found. It is a handoff for whoever continues, and it
can be deleted once the port is ordinary history. It is not user documentation: that is `docs/manual.md`, and
`docs/features.md` describes what the plugin does.

**State at handoff.** The port sits on top of `c9369ba` ("Show only the version atop the user manual") as one commit,
"Build the CLAP and VST3 plugins on Linux", which carries this document and the post-handoff review's fixes (§12) with
it. The user confirmed it loads and works in REAPER on Linux after the `ModuleEntry` fix. Bitwig is where the flicker
and the black VST3 screen appeared; those fixes were verified locally (pixel checks and a Bitwig-shaped `IRunLoop`
test) but not re-confirmed by the user in Bitwig at handoff.

**What has not been verified.** Nothing here compiled with MSVC. The Windows files were changed only inside `#ifdef
_WIN32` guards (see "Windows side"), but the first thing the next agent should do is run `build.ps1 -Validate` on
Windows. `docs/features.md` and `docs/manual.md` have not been updated for the Linux-specific behaviour (zenity
dialogs, the veil); they need a pass.

---

## 1. Result

| Artifact | Path in the build tree | Notes |
| --- | --- | --- |
| CLAP | `build/linux/FMVibe.clap` | shared object, `clap_entry` |
| VST3 | `build/linux/FMVibe.vst3/Contents/x86_64-linux/FMVibe.so` | bundle, `GetPluginFactory`, `ModuleEntry`, `ModuleExit` |
| moduleinfo | `build/linux/FMVibe.vst3/Contents/Resources/moduleinfo.json` | written by CMake |
| Tests/tools | `fmvibe-quality`, `fmvibe-uishot`, `fmvibe-presettool`, `fmvibe-linux-host` | |
| Validator | `build/linux/vst3-validator/validator` | Steinberg's, built from the SDK |

`./build.sh` builds and runs `ctest` (5 tests); `./build.sh --install` puts the plugins in `~/.clap` and `~/.vst3`.

Final local run:

```
quality .......... Passed   32.16 s
ui ............... Passed   46.66 s
manual ........... Passed    0.09 s
linux-host ....... Passed    0.37 s
vst3-validator ... Passed    0.86 s
100% tests passed, 0 tests failed out of 5
```

Steinberg's validator on the bundle: **47 tests passed, 0 failed.** (Only `Info: 64bit Audio processing not supported`,
which is correct: the engine is 32-bit samples.)

## 2. Constraints this port keeps

- **X11 only, XWayland included. No Wayland code, no GTK, no GLib.** Linkage of both plugins:
  `libcairo.so.2`, `libXext.so.6`, `libX11.so.6`, `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`.
  Transitive through Cairo/fontconfig: `fontconfig`, `freetype`, `Xrender`, `xcb*`, `pixman`, `png`, `z`, `expat`,
  `bz2`, `brotli`, `Xau`, `Xdmcp`. No `libgtk*`, `libgdk*`, `libglib*`, `libwayland*`, `libGL*`, `libdl`.
- **No `dlopen` in the plugins.** The only `dlopen` is `tests/linux_host.cpp`, whose job is to load the plugins.
  `nm -D --undefined-only` on both plugins has no `dl*` symbols.
- **Dialogs are external programs**: `zenity`, else `kdialog`; `xdg-open` for folders and the manual. They are spawned
  as separate processes (`posix_spawnp`), so GTK/Qt lives in them, never in the DAW. With neither installed, the
  editor shows "install zenity or kdialog for file dialogs" for 5 s and nothing opens; everything else, including the
  preset browser's own Save dialog, still works.
- **Only entry points are exported** (see §5).

## 3. Architecture of the port

The engine, the View and `PluginCore` are unchanged. Everything platform-specific is either behind `#ifdef _WIN32` or
in a new Linux file.

| Layer | Windows | Linux |
| --- | --- | --- |
| Editor window | `src/gui/editor.cpp` (Win32, Direct2D) | `src/gui/editor_x11.cpp` (X11, Cairo) |
| Drawing | `src/gui/canvas.cpp` (D2D/DWrite) | `src/gui/canvas_cairo.cpp` (unchanged) |
| Editor's clock | `WM_TIMER` | CLAP `clap_plugin_timer_support`; VST3 `Linux::IRunLoop` timer |
| Dialogs | `IFileDialog` / `OPENFILENAME`, `MessageBox` | zenity/kdialog, async, polled |
| Menus | `TrackPopupMenu` | own override-redirect X11 windows with pointer/keyboard grab |
| Clipboard | Win32 clipboard | X11 `CLIPBOARD` selection |
| Settings lock | named mutex | `flock` on `settings.txt.lock` |
| Replace a file | `MoveFileExW` | `rename()` |
| Delete a preset folder | `SHFileOperationW` (Recycle Bin) | freedesktop Trash with `.trashinfo`, else delete |
| Preset folder | `Documents\FMVibe\presets` | `$XDG_DATA_HOME/FMVibe/presets`, else `~/.local/share/...` |
| Settings | `%APPDATA%\FMVibe\settings.txt` | `$XDG_CONFIG_HOME/FMVibe/settings.txt`, else `~/.config/...` |
| Denormal flush | `_mm_setcsr` (SSE) | SSE on x86; `FPCR` FZ/FZ16 on aarch64 |

### 3.1 The editor window (`src/gui/editor_x11.cpp`, ~1300 lines)

`Editor` (`src/gui/editor.h`) is one class with platform members under `#ifdef`. The public API is shared:
`size/fit/constrain/setSize/show/choosePreset/repaint/setScale/systemScale`. Win32 keeps `attach(HWND)`,
`shortcut(WPARAM)`, `typedKey(MSG)`; Linux adds `using WindowId = unsigned long`, `attach(WindowId)`, `pump()`,
`modal()`.

- **Window**: `XCreateWindow` as a child of the host's XID, `background_pixmap = None`, event mask: Expose, Button*,
  Motion, Leave, Structure, Focus, Key*, Property. `XSetErrorHandler` ignores errors (a host may destroy a window
  under us; Xlib's default handler would exit the process).
- **Painting, double-buffered**: all drawing goes into a `cairo_image_surface_t` ("back buffer", `CAIRO_FORMAT_RGB24`,
  ~4.8 MB at 1200×1002). `View::paint()` draws the whole editor or only `View::changedAreas()`. Then the back buffer
  is copied into the window's `cairo_xlib_surface_t` with `cairo_paint` — whole, or once per changed rect. `Expose`
  only re-copies; it never redraws. Transform: `cairo_translate(offset)` then `cairo_scale(s)`, where
  `s = min(width/View::kWidth, height/view.height())`.
- **Why the back buffer**: direct painting into the window showed the frame being filled — "insane flickering" in
  both Bitwig and REAPER. See §6.1.
- **`setSize` guard**: a host that calls `onSize` every block must not force a full redraw; the window is resized only
  when the back buffer's size differs.
- **`show()`** maps/unmaps; it does not raise (`XMapRaised` flashed).
- **DPI/scale**: `Xft.dpi` from the root window's `RESOURCE_MANAGER` sets `dpiScale_`; `setScale()` serves CLAP's
  `set_scale` and VST3's `IPlugViewContentScaleSupport`. `systemScale()` is 1.0 before a window exists. As on Windows,
  a scale change keeps the zoom and asks the host for the size (`Controller::requestEditorSize`).
- **Typing/focus**: focus is taken on left-button press and on `wantsKeyboard()` (typing or a dialog), released on
  `LeaveNotify` when nothing needs it. Keys are read with `XLookupString` (Latin-1; the View takes printable ASCII),
  mapped to `View::Key` for arrows/Enter/Escape/Backspace/Delete/Home/End/Tab, with Ctrl+Z/Y undo/redo and the SEQ
  view's Ctrl+A/C/X/V/D. A key the editor doesn't take is re-sent to the host window with `XSendEvent`.
- **Cursor**: `XCreateFontCursor` per `CursorKind` (pen → `XC_pencil`).
- **Context menu**: `View::contextMenu()` items are drawn in an override-redirect window that grabs the pointer and
  keyboard; the pump feeds it events; no nested loop (the host's UI thread must keep running).
- **Clipboard**: owns the `CLIPBOARD` selection, answers `TARGETS`, `UTF8_STRING`, `STRING`, `TEXT`; paste is
  `XConvertSelection` and is completed on `SelectionNotify` (property `FMVIBE_PASTE`).
- **Overlay**: while a desktop dialog is open the editor draws a dimmed veil and the two strings
  `LinuxDialogOpen` / `LinuxDialogHint` ("A dialog is open", "Alt+Tab to show it, Esc to close it"); Escape closes the
  dialog. A notice (missing dialog tool, failed write) is drawn at the bottom for 5 s.
- **`pump()` order**: X events (or the menu's) → `dialogs.poll()` → start a queued preset dialog → `tick()` (keymap
  Ctrl, `View::poll`, invalidate what changed, expire the notice) → `paint()`.

### 3.2 Driving the editor from the host

- **CLAP** (`src/plugin.cpp`): implements `clap_plugin_timer_support_t` (`on_timer` → `editor->pump()`). On
  `set_parent` it registers a **16 ms** timer with the host (`CLAP_EXT_TIMER_SUPPORT`), paints the first frame
  immediately, and unregisters on destroy. If the host has no timer support, a thread calls
  `host->request_callback()` every 16 ms while the editor is open (CLAP guarantees the callback on the main thread).
- **VST3** (`src/vst3plugin.cpp`): the view's `IPlugFrame` is queried for `Linux::IRunLoop`. A `WakeHandler` implements
  **both** `Linux::IEventHandler` (the wake pipe) and `Linux::ITimerHandler` (**16 ms** → `pumpEditor()`), and both are
  registered/unregistered with the run loop in `frameChanged()`. `attached()` pumps once so the first frame is drawn.
  If there is no run loop, a thread reads the wake pipe (see below); the editor then only pumps on wakes.

The black VST3 screen in Bitwig was exactly this: CLAP had a timer, VST3 had none — nothing ever called `pump()`.
See §6.2.

### 3.3 Waking the VST3 main thread

Windows uses a `HWND_MESSAGE` window. Linux uses a self-pipe:
`pipe2(wakePipe_, O_CLOEXEC | O_NONBLOCK)`; `wake()` writes one byte unless one is already on its way (`wakePosted_`);
the run loop's `onFDIsSet()` drains the pipe and calls `woken()`, which runs `onMainThread()`, then
`handler_->restartComponent(flags)`. Without a run loop, a thread `poll()`s the pipe every 100 ms and calls `woken()`
(a plugin with no editor never gets a frame, so the thread only runs where no editor exists; it never touches UI).

### 3.4 Dialog runner (`Editor::Dialogs`, in `editor_x11.cpp`)

Async, never modal: `zenity` if on PATH else `kdialog`, else the fallback notice. The child's stdout is a non-blocking
pipe; `poll()` (from the editor's pump) collects output and status. One dialog at a time, queued. Escape kills the
child and reports cancel. Calls used:

| Use | zenity | kdialog |
| --- | --- | --- |
| Open preset | `--file-selection --filename=<folder>/ --file-filter=...` | `--getopenfilename <folder> "*.fmv|..."` |
| Save preset | `--file-selection --save --confirm-overwrite --filename=<folder>/<name>` | `--getsavefilename ...` |
| Warning/info | `--warning` / `--info --no-markup` | `--sorry` / `--msgbox` |
| Question | `--question` | `--warningyesno` |

Load/Save failures produce a warning dialog with `DialogNotPreset` / `DialogWriteFailed`. A save name without `.fmv`
gets one.

### 3.5 Presets and settings (`src/presets.cpp`, `src/usersettings.cpp`)

- `PresetLibrary::folderPath()` → XDG data dir + `FMVibe/presets`; `PresetLibrary` otherwise unchanged: factory
  presets in memory, sparse `.fmv` files, the `factory:` prefix, the old `Factory` subfolder handling.
- **UTF-8 at the edges**: `widePath()` decodes UTF-8 to a wide string on Linux (UTF-32), `pathOf()` builds a byte path
  (never `std::filesystem::path(wstring)`, which would go through the C locale), `utf8Text()` back, `openPath()` is
  the only `fopen` wrapper. `widePath`/`utf8`/`pathOf`/`samePath`/`openPath`/`openExternal`/`revealPath` live in
  `presets.h`.
- **Case**: `samePath()` is exact on Linux, case-insensitive on Windows.
- **Settings lock**: `flock(LOCK_EX|LOCK_NB)` on `<file>.lock`, retried for 1 s (Windows: named mutex, 1 s). The same
  merge semantics: a process merges what it changed into the file another process may have changed meanwhile.
- **Atomic writes**: write `<file>.<pid>.part`, then `rename()` (Windows: `MoveFileExW`).
- **Delete folder**: freedesktop home Trash with a `.trashinfo` (percent-encoded path, local date); falls back to
  deleting, like the Windows build when the Recycle Bin isn't available. `revealPath()` opens the containing folder
  (no `FileManager1` D-Bus call, KISS).
- `FMVIBE_SETTINGS_DIR` still overrides the settings folder (the tests use it).

### 3.6 Engine bits

- **Denormals**: `PluginCore::beginDenormals()`/`endDenormals()` keep the audio thread's floating point in
  flush-to-zero: `_mm_setcsr(csr | 0x8040)` on x86 (SSE), `FPCR` bits 24 and 19 (FZ/FZ16) on aarch64, no-op elsewhere.
  Same behaviour as before, portable.
- `openManual()` → `xdg-open` on `kManualUrl`.
- `FMVIBE_FIXED_SEED` is read with `getenv` now.

## 4. Build system

`CMakeLists.txt` still returns early for `NOT WIN32` into `cmake/linux.cmake`, which now builds the plugins too.

| Target | What |
| --- | --- |
| `fmvibe-core` | engine + View + `canvas_cairo.cpp` + `state` + factory presets; `FMVIBE_CAIRO` public; links `clap`, `PkgConfig::CAIRO` |
| `fmvibe-plugin` | `plugincore.cpp`, `presets.cpp`, `usersettings.cpp`, `gui/editor_x11.cpp`; links `X11::X11`, `Threads::Threads` |
| `FMVibe` | `entry.cpp`, `plugin.cpp`, `SUFFIX ".clap"`, version script `cmake/symbols-clap.map` |
| `vst3-interfaces` | `coreiids.cpp`, `funknown.cpp`, `vst3iids.cpp` (which now also defines the Linux IIDs and `IPlugViewContentScaleSupport`) |
| `FMVibe-vst3` | `vst3entry.cpp`, `vst3plugin.cpp`, `SUFFIX ".so"`, output in `FMVibe.vst3/Contents/x86_64-linux`, version script `cmake/symbols-vst3.map` |
| tests | `fmvibe-quality`, `fmvibe-uishot`, `fmvibe-linux-host` |
| tools | `fmvibe-presettool` |
| validator | `validator` from `cmake/vst3-validator.cmake`, test `vst3-validator`, target `validate-vst3`, `validate` |

Key CMake properties and flags:

- `CMAKE_POSITION_INDEPENDENT_CODE ON` (the static libraries go into shared modules).
- `CXX_VISIBILITY_PRESET hidden` + `VISIBILITY_INLINES_HIDDEN` on `fmvibe-core`, `fmvibe-plugin`, and both modules.
  This is not cosmetic: without it, `-fPIC` code is ~3× slower in the analyzer (see §6.3).
- `-Wl,--version-script=cmake/symbols-*.map` on both modules.
- `-Wall` on our targets only (the SDK's validator sources are not given it).
- Quality test runs under `sh -c "ulimit -s 65536; exec ..."`, as before (dozens of engines on the stack).

Dependencies (Debian/Ubuntu):

```bash
sudo apt-get install -y cmake ninja-build g++ pkg-config git libcairo2-dev libx11-dev fonts-inter
```

`build.sh` fetches, at the tags `CMakeLists.txt` pins:

| What | Where |
| --- | --- |
| CLAP `1.2.10` | `build/deps/clap-1.2.10` (`FetchContent` source dir) |
| `vst3_pluginterfaces` | `build/deps/vst3sdk/pluginterfaces` |
| `vst3_base`, `vst3_public_sdk` | `build/deps/vst3sdk/{base,public.sdk}` (validator) |

`cmake/linux.cmake` downloads the same archives with pinned SHA-256 if the directories are absent, so a bare CMake
configure works without `build.sh`. Note `vst3_public_sdk` has an underscore in the repo name, while the directory is
`public.sdk`.

Install (`cmake --install ... --prefix "$HOME"`):

```
$HOME/.clap/FMVibe.clap
$HOME/.vst3/FMVibe.vst3/Contents/x86_64-linux/FMVibe.so
$HOME/.vst3/FMVibe.vst3/Contents/Resources/moduleinfo.json
```

## 5. Exports

```
$ find . -type f \( -name "*.so" -o -name "*.clap" \) -exec nm -Dg --defined-only {} +

FMVibe.vst3/Contents/x86_64-linux/FMVibe.so:
0000000000037400 T GetPluginFactory
0000000000037410 T ModuleEntry
0000000000037420 T ModuleExit

FMVibe.clap:
00000000003b0b00 D clap_entry
```

Before this work the two modules exported **4,077** symbols between them: every `fmvibe::*`, `gui::*`, `vst3::*`. Two consequences:

1. Symbol interposition: a CLAP and a VST3 loaded in one host process (REAPER scans both) could bind each other's
   copies of `Factory`, static initializers, etc.
2. `-fPIC` cost: internal calls went through the PLT/GOT; the analyzer's FFT paid ~3×.

The version scripts are `cmake/symbols-clap.map` and `cmake/symbols-vst3.map`. **If a new export is ever needed, add
it to the map or it will not be visible.** `ModuleEntry`/`ModuleExit` are mandatory on Linux (see §6.4).

## 6. Bugs found, evidence, fixes

### 6.1 "Insane flickering" in Bitwig and REAPER (CLAP)

Cause: the editor drew directly into the X11 window; a frame was visible while Cairo was still filling it.
Fix: back buffer + one blit (§3.1). Also: `show()` no longer raises, `setSize` no longer redraws when the size is
unchanged, `Expose` copies instead of redrawing. Verified by reading pixels back from the real X11 window
(`linux-host` "its editor paints"), and visually with `FMVIBE_SHOT`.

### 6.2 VST3 black screen in Bitwig

Cause: nothing ever called `Editor::pump()` on VST3. CLAP had a host timer; VST3 only had a wake pipe that fired on
events. Fix: `WakeHandler` also implements `ITimerHandler`, registers a 16 ms timer in `frameChanged()`, and
`woken()`/`attached()` pump too. Verified by a test that mimics Bitwig: a real `IPlugFrame` exposing `IRunLoop`,
timer fired by hand, pixels read back.

### 6.3 `quality` failing in a VM: the analyzer cost ratio

Failing check: "silence through effects that have rung out costs little while the editor analyzes them too", which
requires idle < 25 % of busy. Measurements (2 s of audio; `timing` is best-of-three):

| Build | busy | idle | ratio | |
| --- | --- | --- | --- | --- |
| VM (`err.log`) | 38.42 ms | 15.43 ms | 0.40 | fail |
| `-fPIC`, default visibility | 35.1 ms | 14.6 ms | 0.42 | fail |
| no PIC | 26.9 ms | 4.9 ms | 0.18 | pass |
| `-fPIC` + hidden visibility | 25.8 ms | 4.7 ms | 0.18 | pass |

Cause: internal calls in a shared module compiled `-fPIC` with default visibility go through the PLT; the FFT in
`analyzer.cpp` tripled. Fix: hidden visibility on the static libraries (plus the version scripts). This also makes the
shipped plugin's analyzer ~3× cheaper. Do not remove it.

That wasn't all of it: the idle half of the check is the FFTs the analyzer still runs over silence, and GCC lowers each
`std::complex<float>` multiply in them to a libgcc `__mulsc3` call (whatever the optimization or visibility), where
MSVC inlines it — which is why the Windows build passed while a VM still failed. `Analyzer::measure()` now skips the
transform while its whole window is silence and falls each band by its share, which is what the transform of zeros came
to, bit for bit.

### 6.4 REAPER: "fails to load" while Bitwig loads it

Symptom: REAPER records the class in `reaper-vstplugins64.ini` (timestamp, 32-bit FUID hash, CID
`{9BCBBCB9166D49F6A9C2271D5009BDD5}`, `!!!VSTi`) yet fails to instantiate. Dedicated-process mode and compatibility
mode did not help; neither did the export whitelist (that was fixed for other reasons).

Diagnosis, in one command, with Steinberg's validator built from `vst3_base` + `vst3_public_sdk`
`3.8.1_build_84`:

```
* Loading module...
    /tmp/valtree/build/FMVibe.vst3
Invalid Module!
The shared library does not export the required 'ModuleEntry' function
```

The VST3 SDK's Linux module loader (`module_linux.cpp`) treats `ModuleEntry` and `ModuleExit` as **mandatory**; REAPER
loads through that loader. Bitwig has its own loader that doesn't ask. Scanning (metadata, CID) worked because it
doesn't need the entry points; instantiation did not.

Fix: export both on Linux (Windows keeps `InitDll`/`ExitDll`):

```cpp
SMTG_EXPORT_SYMBOL bool ModuleEntry(void *) { return true; }
SMTG_EXPORT_SYMBOL bool ModuleExit() { return true; }
```

After the fix the validator runs the whole suite: **47 passed, 0 failed**.

### 6.5 UI snapshot failures on Linux

Three pixel checks read where a text's ink falls (preset bar centring/author, a long stepper's number, an envelope
readout over its curve). Which of them fails depends on the installed font; the failing one differed between this
machine (Inter installed) and the user's VM. Fix: `checkFont()` in `tools/uishot.cpp` checks only on Windows and prints
`skip` on Linux. The glow check's assertion that the halo reaches past the ring is Windows-only too: past the ring the
Cairo halo is exactly the background (measured `161920` with glow on and off; ring `1B1F2A` when lit). The ring's lit
and plain states are still checked. This plus 4.3 makes `ui` pass on Linux.

## 7. Tests

### 7.1 `tests/linux_host.cpp`

A small headless host; `ctest` gives it the built `.clap` and the bundle's `.so`.

CLAP: `dlopen` → `clap_entry` → one class → `init` → `activate` (48 kHz, 512) → `start_processing` → more than 1,000
host params → has a GUI → a note sounds (peak printed, ~0.25) → state saves (about 128 KB) and loads back → with
`DISPLAY`, editor is created, sized (1200×1002), put into an X11 parent, shown, takes the host timer, paints (pixels
differ), closes.

VST3: `dlopen` → `GetPluginFactory` → one class → `getClassInfo`, `getClassInfo2`, `getClassInfoUnicode` agree (CID,
category, names) → component → `initialize` → `IAudioProcessor` → `setupProcessing` → `setActive` → 4 blocks of
`process` → editor: `IEditController::createView`, `IPlugFrame` with `IRunLoop`, `attached` with
`kPlatformTypeX11EmbedWindowID`, run-loop timer pumps, pixels differ → `terminate`.

`FMVIBE_SHOT=/path/file.ppm` writes what the editor's window shows (the test caps the read at 900×700; `XGetImage`
fails for larger regions in this environment, cause unexplained).

### 7.2 Steinberg's validator

`cmake/vst3-validator.cmake`: `sdk_hosting` is built from the SDK's sources on Linux (base, pluginterfaces,
`public.sdk` hosting/utility, `module.cpp`, `threadchecker_linux.cpp`), the sample's CMakeLists is added as
`vst3-validator`, and `add_test(NAME vst3-validator COMMAND validator ${FMVIBE_VST3_BUNDLE})`. Stub functions
`smtg_target_codesign`/`smtg_target_setup_universal_binary` are required by the sample. `SDK_ROOT` must contain
`base/`, `public.sdk/` and `pluginterfaces/`; if `VST3_PLUGINTERFACES_DIR` points elsewhere, a symbolic link
`<root>/pluginterfaces` is made. Turn it off with `-DFMVIBE_LINUX_VST3_VALIDATOR=OFF`.

To run it by hand after a build:

```
./build/linux/vst3-validator/validator ./build/linux/FMVibe.vst3
```

### 7.3 UI snapshots

`fmvibe-uishot build/linux/ui-shots 1` renders the replays; `FMVIBE_FONT` picks another family (default Inter). As
before the port, the suite is sensitive to Cairo's antialiasing; the font-metric checks and the glow feathering are
now Windows-only (§6.5). The envelope readout, the glow values and the rest still print their measurements.

## 8. Traps for the next agent

- **`ModuleEntry`/`ModuleExit` must stay.** Removing them breaks REAPER and the validator.
- **Keep the version-script lists up to date.** A new export not listed is invisible to hosts; a new `SMTG_EXPORT_SYMBOL`
  on Linux is not enough.
- **Do not remove hidden visibility or `-fPIC`**; the quality test's ratio depends on hidden visibility, and the modules
  need PIC.
- **The editor only paints when pumped.** Any host integration must call `Editor::pump()`: CLAP timer (or the
  `request_callback` fallback) or VST3 `IRunLoop` (timer or the pipe fallback). A new format needs its own driver.
- **No GTK, no Wayland, no `dlopen`** in the plugins. Dialogs are external processes.
- **`XGetImage`** of a whole editor-sized child fails here; the tests read 200×200 and cap `FMVIBE_SHOT` at 900×700.
- **`vst3_public_sdk`** (underscore) is the repository; the directory is `public.sdk`.
- **`getClassInfo2`/`getClassInfoUnicode`** are on `IPluginFactory2`/`IPluginFactory3`, queried from the base factory.
- **REAPER's `.ini` line proves scanning, not loading.** Presence of the CID there does not mean the module loaded.
- **Repo rules still apply** (see `AGENTS.md`): parameter ids and the state layout are stored in projects; new text goes
  to `src/uistrings.inc`; theme scales are `kRadius*`/`kText*`; a feature change updates `docs/features.md` and the
  manual; `ctest` must stay green.
- **The editor uses Cairo's toy font API** (`cairo_select_font_face`), so the family is the system's; there is no Pango
  and no bundled font. `FMVIBE_FONT` overrides the family for snapshots.
- **`build.sh`** now also clones the two validator source repos; the repo name mapping is `base:vst3_base`,
  `public.sdk:vst3_public_sdk`.

## 9. Windows side: what changed, what to check

These files now have Linux code beside the Windows code; the Windows paths were kept but **not compiled** here:

- `src/gui/editor.h`: platform split; Windows members unchanged; adds `setScale`/`systemScale` (implemented in
  `editor.cpp` with `GetDpiForSystem`).
- `src/plugin.cpp`: `kWindowApi`/`windowOf` (Win32: `CLAP_WINDOW_API_WIN32`, `HWND`); timer support and pump calls are
  `#ifndef _WIN32`; `kTimer` is defined only on Linux and its extension is returned only there.
- `src/vst3plugin.cpp`: portable UTF-8↔UTF-16; X11 view type and attach; `IPlugViewContentScaleSupport`; wake pipe +
  run loop (Linux) beside the message-only window (Windows); `frameChanged` is a no-op on Windows; `pumpEditor` is a
  no-op there.
- `src/vst3entry.cpp`: portable `copyNarrow`, `ModuleEntry`/`ModuleExit` on Linux, `InitDll`/`ExitDll` on Windows.
- `src/plugincore.cpp`: `getenv` for the seed, `openExternal` for the manual, `pathOf`/`kSep`, portable file opens,
  SSE/aarch64 denormals.
- `src/presets.cpp`, `src/usersettings.cpp`: `#ifdef _WIN32` for the shell/COM/file APIs; the Windows branches are the
  previous code, moved.
- `src/gui/editor.cpp`: only the two new functions.

First Windows job: `build.ps1 -Validate` (tests + clap-validator + VST3 validator). Anything that fails there is a
guard that needs fixing; the Linux build cannot see it.

## 10. How to reproduce everything from a clean checkout

```bash
./build.sh                          # deps, configure, build, ctest (5 tests)
./build.sh --shots 2                # render the snapshots at 2x into build/linux/ui-shots
./build.sh --install                # ~/.clap and ~/.vst3
./build/linux/vst3-validator/validator ./build/linux/FMVibe.vst3   # 47 passed
./build/linux/fmvibe-linux-host ./build/linux/FMVibe.clap \
    ./build/linux/FMVibe.vst3/Contents/x86_64-linux/FMVibe.so      # loads, plays, paints
find . -type f \( -name "*.so" -o -name "*.clap" \) -exec nm -Dg --defined-only {} +   # entries only
```

A local reference environment where all of this passed: CMake 4.4.5, Ninja 1.13.2, GCC 16.2.1 (Clang 23.1.4 also
present), Cairo 1.18.6 (cairo-xlib), X11 1.8.14, fontconfig with Inter, 12 cores, X display 1920×1080. `build.sh`
works without a display; only `linux-host`'s editor section needs `DISPLAY` (it skips without one).

## 11. Open items

- Done since handoff: `docs/features.md` ("On Linux" under CLAP and VST3) and `docs/manual.md` (Installation, Dialogs,
  What is saved where) describe the Linux behaviour (zenity/kdialog dialogs, the veil, the notice, the XDG paths).
- Optionally make the validator's `-selftest` run in CI, and/or a `validate-clap` on Linux via a Linux build of
  clap-validator (today only the Windows binary is pinned).
- Wayland is deliberately out of scope; if it is ever added, it is a second window backend (`Editor` platform split
  already allows it) and the dialogs stay external.
- The editor's `mouseMove` invalidates the whole editor, as on Windows; partial repaint could use `changedAreas()` if
  dragging ever feels heavy on a slow machine.
- `XGetImage` of a large window region fails here; if a full-window screenshot is wanted, read it in bands.

## 12. Post-handoff review fixes (in the same commit)

A review of the port against X11 and plugin practice found and fixed these; they are part of the port commit. None was
built (no build was run); the Linux tests and `build.ps1 -Validate` on Windows are still to run.

- **Windows compile risk**: `plugincore.cpp` included `shellapi.h` before `windows.h`; the order is back.
- **Windows behavior kept as it was**: `deleteFolder` no longer falls back to a permanent delete when the Recycle Bin
  fails (the port had added that to the Windows branch too); `FMVIBE_FIXED_SEED` set but empty counts as unset again;
  the glow pixel check in `tools/uishot.cpp` is exact on Windows again (the tolerance stays Linux-only).
- **Paths stay UTF-8 in the middle too**: `fs::path::wstring()` widens by the C locale on Linux, so the scan's wide
  paths and a state's `widePath()` disagreed on any non-ASCII name (a save double-encoded the file name, `indexOf`
  missed the preset). A `wideOf()` helper (`presets.h`) decodes the path's UTF-8 bytes instead, and every `.wstring()`
  on a byte path in `presets.cpp`, `plugincore.cpp` and `usersettings.cpp` goes through it.
- **zenity's filter** is its name, a bar and its patterns (`FMVibe preset (*.fmv) | *.fmv`); the wording alone matched
  no files.
- **`xdg-open` never holds the UI thread**: `runDetached` (both copies) starts it through a short-lived intermediate
  child that ends at once; the program goes to init, which reaps it. It was a blocking `waitpid` on the host's UI
  thread before, and `xdg-open` can wait for what it opens.
- **The X error handler is the process's**: it now ignores errors only on the editors' own displays and chains any
  other to the host's handler, which goes back when the last editor closes (unless something else took the place).
- **The child window takes its parent's visual and depth** (a 32-bit host window, say), and the Cairo surface is made
  for that same visual.
- **A context menu whose grab fails closes at once** instead of holding the editor's input forever; events the menu
  doesn't take (the editor's resize/expose, clipboard requests) reach the editor while it is open.
- **`Child::stop()` stays bounded** (~150 ms worst case, no blocking `waitpid`); a dialog's tool is looked up as the
  dialog opens, not once per process.
- **Paste falls back to `XA_STRING`** when the clipboard owner offers no `UTF8_STRING`, and gives up cleanly.
- **Cursors are made once** per kind, not per mouse move; the settings write fsyncs before the rename, as Windows's
  `MOVEFILE_WRITE_THROUGH` does; a read interrupted by a signal (`EINTR`) goes on instead of counting as the file's end.
- Dead members gone (`doubleClick_`, `lastPoll_`, `browsePreset`); the stale `CMakeLists.txt` comment now says Linux
  builds the plugins too; `docs/features.md` and `docs/manual.md` describe the Linux behaviour.

Known limits left as they were: typing is Latin-1 (no compose/IME), the context menu keeps its own dark look in every
theme, and a pointer grab broken by another client is taken up again at the next click. Still not compiled with MSVC:
`build.ps1 -Validate` on Windows remains the first job there.
