# Application shell: specification

Status: implemented 2026-09-24, uncommitted, with every choice in section 14 taken as
recommended (any of them can be reversed). Written 2026-09-23.

The shell is everything `NvFBCR.exe` does before a capture mode's loop starts and after it
ends: choosing the displays and the mode, the windows, the Direct3D 9 devices, loading and
enabling NvFBC, the startup capture session, handing over to the mode, and teardown. Today it
is `src/relay/NvFBCR.cpp`. This document describes the behavior the new shell must have, in
terms of what the user sees and what the log records, so it can be implemented fresh. Where
the behavior changes from today, section 12 lists the change and section 14 lists the choices
that are yours.

The rules the shell applies to a launch string stay where they are, in `src/core/LaunchOptions.h`
under the policy suite: the option registry, the usage list, the mode grammar, dependency
settling and the relaunch round trip. The new rules this document adds (section 4.4) go there
too, with suite tests.

## 1. Modules

| file | holds |
|---|---|
| `Main.cpp` | `WinMain`: the sequence in section 2 and nothing else; every exit goes through the failure helper |
| `Failure.h/.cpp` | the `Failure` value, the exit helper, the warning popup, the log hint (section 10) |
| `Displays.h/.cpp` | enumeration: adapter ordinal, device name, desktop rectangle, friendly name, refresh rate (section 3) |
| `Prompts.h/.cpp` | the console: display list, the three prompts, the re-asks, accepting or refusing a command line (section 4) |
| `OutputWindow.h/.cpp` | the window class, the output window, the hidden D3D9 host window, the message pump shared by every mode (section 5) |
| `D3D9Setup.h/.cpp` | Direct3D 9Ex, the present device, the back buffer, the display-mode comparison, flip mode, and one builder for windowed present parameters that the ring and the diag mode also use (section 6) |
| `NvFBCSession.h/.cpp` | load, status, enable, relaunch, and the startup capture session with its grab parameters (section 7) |
| `RelayContext.h` | what the shell hands a capture mode (section 8; decision 6) |

`NvFBCR.cpp` is deleted. `NvFBCLoader.h`, `SimpleLogger.h`, `AdminCheck.h` and `NvFBCApi.h`
stay in `src/common/` and keep their roles.

## 2. Startup sequence

Each step either succeeds or returns a `Failure`; `WinMain` runs them in this order and turns
the first failure into teardown and a popup (section 10). Nothing else in the process exits.

1. **DPI awareness.** Per-monitor v2, before any call that reads a monitor rectangle, so every
   rectangle is in physical pixels.
2. **Single instance.** Take the named lock `Global\NvFBCR_SingleInstance`. If another process
   holds it, show the "already running" popup and exit. This runs before the first log line,
   because opening the log truncates it, and the log belongs to the running instance.
3. **Log.** The first line marks the start (section 11). A relaunched process says so.
4. **Displays.** Create the Direct3D 9Ex object and enumerate the displays (section 3). Fewer
   than two displays is a failure.
5. **NvFBC library.** Load `NvFBC64.dll` from System32 through `NvFBCLoader`. A missing or
   incomplete DLL is a failure, reported before any prompt so nobody answers questions first.
6. **What to run.** Accept the command line, or ask at the prompts (section 4). The result is
   a source display, a target display, a mode string and the options.
7. **Settle and build the mode.** `launch::ResolveDependencies`, then the capture mode object
   from the mode string. The mode string was validated in step 6, so this cannot fail. The
   `-flipex` adjustments are made here (section 6).
8. **NvFBC status.** Read the capture status. If capture is not possible, turn NvFBC on and
   relaunch (section 7). This happens before any window exists, so a relaunch never flashes a
   window on the capture card.
9. **Windows.** Register the class, create and show the output window on the target display,
   and create the hidden host window when the mode presents through D3D11 (section 5).
10. **Present device.** Create it and fetch its back buffer (section 6).
11. **Startup capture session.** Create the NvFBC session on the present device and set it up
    to write into the back buffer (section 7).
12. **Mode setup.** `Setup` on the mode.
13. **Run.** The mode's loop, until the output window closes or the mode stops.
14. **Teardown** (section 9), then the popup if the run ended in a failure, then exit.

Compared with today, steps 5 and 8 move earlier: today the library loads after the window is
shown, and "not enabled" is discovered only when the session creation in step 11 fails, by
which time the output window is on screen.

## 3. Displays

- One entry per Direct3D 9 adapter (`GetAdapterCount`), which is one per display attached to the
  desktop. The entry's number is the adapter ordinal; it is the number the user types and the
  number `-source` and `-target` take.
- Per entry: the GDI device name and desktop rectangle from `GetMonitorInfo` on the adapter's
  monitor, the refresh rate from `EnumDisplaySettings` on the device name (0 when unreadable),
  and the friendly name.
- **Friendly names are matched by device name.** For each active path from `QueryDisplayConfig`,
  read the source's GDI device name (`DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME`) and the
  target's friendly name, and give the name to the entry with that device name. Today the name
  of path *i* goes to adapter *i*, which relies on two orderings agreeing (nothing documents
  that they do) and writes past the end of the list when there are more paths than adapters,
  as in a duplicated desktop.
- Friendly names are best effort. When `QueryDisplayConfig` fails or a display reports no
  name, that entry shows its device name, with one log line; it is never a startup failure.
- Every entry is logged (section 11), so a log shows the numbering the user chose from.

## 4. Choosing what to run

### 4.1 The command line

A command line is **used** only when it is complete and valid:

- `-source` and `-target` are both present, are numbers, name displays in the list, and are
  different displays;
- `-framerate`, when present, is a mode `launch::ParseMode` accepts; absent means the default,
  `b:vsync`;
- every other token is an option in the registry, with a valid value.

Then the relay runs with no console. Otherwise, when the command line is not empty, the relay
opens the console, prints one line saying why it was not used, and asks at the prompts, starting
from the defaults: nothing from the refused command line carries over (decision 1). An empty
command line goes straight to the prompts.

The one line (UI text):

```
The command line was not used: there is no display 7. Choose below instead.
```

with the reason being one of: `-source must be a display number, not 'abc'`; `there is no
display 7`; `the game display and the capture card display must be different`; `-source is
missing` (or `-target`); `'bogus' is not one of the modes`; `'-srcc' is not one of the
options`; or an option's value message (section 4.4).

### 4.2 The prompts

The console is allocated when the prompts are needed and freed when they are done. What it
shows, in order (the wording is today's except where marked new):

```

[0] ROG PG279QM, 2560x1440 at (0,0)
[1] EVGA XR1 Pro, 2560x1440 at (2560,0)

Game display number ? 
Capture card display number ? 

<the usage list: launch::UsageLines()>

Mode and options (press Enter for the default) ? 
```

Answers:

| prompt | answer | result |
|---|---|---|
| either display | a number in the list | accepted |
| either display | a number not in the list | new: `There is no display 5. Type a number from the list.` and ask again |
| either display | not a number | new: `Type the number in brackets beside the display.` and ask again |
| capture card display | the game display's number | new: `The capture card display must be a different display from the game display.` and ask again (decision 2) |
| mode | empty | the default mode, `b:vsync`, with default options |
| mode | a mode, then options (`b:vsync -src 60`) | accepted |
| mode | options only (`-src 60`) | new: the default mode with those options. Today the first token becomes the mode and the relay exits |
| mode | a mode it does not know | new: `'bogus' is not one of the modes listed above. Type one of them, or press Enter for the default.` and ask again. Today: logged and exits |
| mode | an option it does not know | new: `'-srcc' is not one of the options listed above.` and ask again. Today: printed and ignored |
| mode | an option with a bad value | new: the option's value message (section 4.4) and ask again. Today: logged and ignored |

A re-asked mode answer is typed again in full; nothing from the refused answer is kept. The
display prompts are not asked again when only the mode answer was refused.

`-source`, `-target` and `-framerate` are not options at the mode prompt (today's rule): typed
there, they are unknown options and re-ask.

Today, after the mode answer, the prompt prints "Game frame rate set to ...", "Blend tint: on"
and "Frame marker: on", then frees the console at once, so the lines vanish before anyone can
read them. They are dropped; the `Resolved options:` line records the same facts.

If reading the console fails (end of input), the relay ends without a popup and logs one line.
In practice closing the console window ends the process before this is reached.

### 4.3 What the log records about the choice

One line says how the choice was made (section 11): from the command line, or from the prompts
with the answers as typed. Each refused command line and each refused prompt answer gets a line
with the text as typed and the reason. Today an empty `Command line received: ''` is the only
trace of a prompt launch, and the answers themselves are recoverable only from the resolved
lines.

### 4.4 New rules in `LaunchOptions.h`

Pure functions next to the existing ones, with suite tests:

- **The mode-prompt answer:** split into a mode and options; a first token beginning with `-`
  means the default mode; every token validated; the result is the mode string and options, or
  the first problem as a plain message.
- **Command-line acceptance:** the rules of section 4.1 over `launch::ParseCommandLine`'s
  result and the display count, returning either "use it" or the reason.
- **Value messages** replace today's warning strings, which end in "- ignored" and are no longer
  true on either path: `-lag takes a whole number from 0 to 200, not '500'`; `-src takes a
  frame rate above 0 and up to 1000, not 'abc'`; `-source must be a display number, not 'abc'`
  (and `-target`). The suite's three checks on the old strings are updated to these.

The launch tests print nothing when they pass, so the suite's output stays the 527 lines of the
baseline.

## 5. Windows

- **Class.** One window class for both windows, arrow cursor, a window procedure that ends the
  message loop when the output window is destroyed. Registration is checked. The class name
  changes from `WindowClass` to `NvFBCR output`; nothing outside the process refers to it.
- **Output window.** Title `NvFBCR`, `WS_POPUP` with `WS_EX_TOPMOST`, at the target display's
  desktop rectangle and exactly its size, shown with the launch's show command. Creation is
  checked. Closing it (Alt+F4 while it has focus) is the normal way a run ends.
- **Host window.** Only when the mode presents through its own D3D11 swapchain (`b:vsync`):
  title `NvFBCR D3D9 host`, `WS_POPUP`, 1x1 at (0,0), never shown. Both D3D9 devices (present
  and the ring's capture device) are created on it, leaving the output window to the D3D11
  swapchain alone. Log line as today.
- **Message pump.** One function, used by every mode's loop: removes and dispatches every
  waiting message and reports whether the loop should end. It replaces the four copies of the
  loop in the mode files.
- The Win32 calls that take strings are made through their explicit `A` names.

## 6. Direct3D 9 devices

Behavior unchanged from today; only the structure is new.

- **Present device adapter:** the target display's adapter when the mode presents on the target
  (`PresentsOnTargetAdapter`), otherwise the source display's, because the legacy modes have
  NvFBC write straight into this device's back buffer. Logged as today.
- **Device window:** the host window when there is one, otherwise the output window.
- **Present parameters:** windowed; back buffer the target display's size; format
  `A2R10G10B10`, or with `-flipex` the display mode's own format; 1 back buffer (2 with
  `-flipex`); swap effect `DISCARD` (`FLIPEX` with `-flipex`); presentation interval from the
  mode. Behavior flags hardware vertex processing and multithreaded, plus present statistics
  with `-flipex`.
- **Display-mode comparison:** the `Display mode on adapter ...` MATCH or MISMATCH line, logged
  on every run as today.
- **Flip mode:** with `-flipex`, frame latency 1 and its log line; a refused flip-mode device is
  a failure with its own popup and never falls back to bitblt. `-flipex` is ignored with a log
  line when the mode presents through D3D11 (today) and, new, when the mode grabs into the back
  buffer (`vsync`, the plain timer, `diag`): those modes use the back buffer fetched once at
  startup, which flip mode rotates, so the combination cannot work.
- **Back buffer:** buffer 0 fetched once, kept for the legacy modes' capture and as the D3D9
  present path's fallback target.
- **Shared builder:** one function returns the windowed present parameters for a window, size,
  format, buffer count, swap effect and interval. The present device, the ring's capture device
  and the diag mode's raster device all use it, with the values they use today.
- **Sink refresh:** the target display's refresh rate from step 4, logged as the `Target
  display refresh:` line, and handed to the mode (the tooth guard arms off it).

## 7. NvFBC: load, status, enable, relaunch, startup session

- **Load** (step 5): `NvFBCLoader::load()`, which loads by full System32 path and requires all
  four exports. Its log lines are unchanged.
- **Status** (step 8): `getStatus` on GPU 0, as today (the relay supports one NVIDIA GPU).
  Logged in one line: capture possible, can create now.
- **Not possible, not administrator:** a failure with the "needs administrator" popup. The
  manifest elevates every launch, so this is reached only by an account that cannot elevate.
  Today's console text ("run NvFBCR.exe normally") goes.
- **Not possible, administrator:** `enable(NVFBC_STATE_ENABLE)`. Failure is a failure with its
  popup. On success, relaunch:
  - tear down what exists (the Direct3D object and the library);
  - build the command line from the resolved choice: `"<exe>" -source <n> -target <n>
    -framerate <mode> <options> -relaunched`, with `b:vsync` for an empty mode and the options
    from `launch::FormatOptions`, as today, plus the hidden command-line-only flag
    `-relaunched` (`launch::RelaunchArguments`);
  - log it, flush the log, release the single-instance lock, start the child, and exit with 0
    without logging again;
  - a failed start is a failure with its popup.

  The marker was first an inherited environment variable. The build with it was flagged by
  Defender as `Trojan:Win32/Sabsik.FL.A!ml` (2026-09-24), and setting an environment variable
  before starting itself again was the likeliest new feature, so the marker moved onto the
  command line, which the relaunch already builds.
- **The relaunched process:** sees `-relaunched`, says so on its first log line, and appends to the
  log instead of truncating it, so one file holds both processes (decision 4). If capture is
  still not possible, it does not relaunch again: that is a failure with its popup. Today nothing
  stops a relaunched process from enabling and relaunching again.
- **Startup session** (step 11): `create` a `NVFBC_TO_DX9_VID` session on GPU 0 bound to the
  present device, then set it up: hardware cursor on, no stereo, no difference map, 10-bit ARGB,
  one buffer (the back buffer), HDR requested. Grab parameters: no wait, scaled to the target
  size, with the frame-grab info. All as today.
- **Session failures:** when `create` fails, read the status again. Cannot create now means
  another program holds a capture session: the "capture in use" popup. Anything else, and a
  failed setup, is the "could not start capturing" popup. Today's log lines naming "Driver R355+
  with Tesla/Quadro/GRID" go.
- **The temporal modes** release the startup session and create their own on the ring's capture
  device (cursor off, blocking grab with timeout), as today. That session is then the ring's.

## 8. Handing over to the mode

- **Context** (decision 6): instead of the eleven globals the modes reach with `extern`
  (`g_pD3DEx`, `g_pD3D9Device`, `g_backbuffer`, `g_sourceAdapterIndex`, `g_targetAdapterIndex`,
  `BUF_WIDTH`, `BUF_HEIGHT`, `g_targetRefreshHz`, `g_flipEx`, `pNVFBCLib`, `NvFBCDX9`), and the
  device window the temporal mode recovers through `GetCreationParameters`, the shell fills one
  `RelayContext` and passes it to `Setup` and `Run`. The session pointer in it has one owner at a time: the shell
  until a mode takes it over, then the mode, which clears it in the context when it releases it.
- **Mode interface:** `Setup` returns nothing on success or a `Failure`. `Run` returns nothing
  when the output window was closed, or the `Failure` that stopped it. A mode never shows an
  exit popup itself; the shell does, after teardown. The frame-timing warning (ETW did not
  start, the run continues) stays in the temporal mode and uses the helper's warning form.
- **What a mode reports** (decision 5 for the run-time ones):

| where | today | reported as |
|---|---|---|
| `TemporalCaptureMode.cpp:377`, the ring does not start | log only, the relay just ends | "could not start capturing", or "capture in use" when the ring's own session cannot be created now |
| `TemporalCaptureMode.cpp:386`, the present path refuses | popup inside the mode | the same popup text, shown by the shell after teardown |
| `VsyncCaptureMode.cpp:31`, `TimerCaptureMode.cpp:36`, `DiagCaptureMode.cpp:87`, session invalidated | log only | "capture lost" |
| `TemporalCaptureMode.cpp:675`, the capture thread stopped | log only | "capture lost" |
| `TemporalCaptureMode.cpp:676`, the swapchain stalled | log only | "output stalled" |

## 9. Teardown

In this order, whatever ended the run:

1. The mode's loop has returned; its capture thread is stopped.
2. Each NvFBC session is released by its owner, before the device it is bound to: the ring
   releases its own in its destructor before its capture device; the shell releases the startup
   session if it still owns it. Today the ring's session is released by the shell after the ring
   has already released the capture device the session is bound to.
3. The mode is destroyed (ETW summary and stop, present path, ring, capture device).
4. The back buffer, the present device and the Direct3D object are released.
5. The windows are destroyed. Today they are left to process exit, so a failure popup could
   appear with the black output window still on the capture card.
6. The library is unloaded.
7. `Shutdown complete`, and a flush.
8. After a failure, the helper: `Stopping: <reason>` as the log's last line, then the popup.
9. Exit: 0 after a closed window or a relaunch, 1 after a failure. The single-instance lock is
   released by the process ending.

## 10. Failures

### 10.1 The helper

A `Failure` holds a popup title, the popup text (plain words: what failed, then what to do),
and a log line (jargon, codes). The helper, called only by `WinMain` after teardown, logs
`Stopping: <log line>`, flushes, shows the popup and returns the exit code. The popup text gets
the log hint appended, the wording the present path's popup uses today:

- log on: `The reason is in NvFBCR.log.`
- log off: `To record the reason, create an empty NvFBCR.log beside NvFBCR.exe and run again.`

The one failure raised before the log exists (already running) has no log line and no hint.
Popup style as today: an error icon, OK only, foreground and topmost, title `NvFBCR: <title>`.
The code that finds a failure also logs its details with its own tag, as the ring and the
present path do today, so the log keeps the tag of where it happened.

A missing `nvofapi64.dll` is the one startup failure without our popup: the exe links it at load
time, so Windows refuses to start the process with its own message naming the file, before any
of our code runs (decision 6 of the cleanup spec). The README's Prerequisites names the file.

### 10.2 Every failure

Today: 13 of `WinMain`'s returns are failures; the lock's has a popup and the not-admin one
prints to a console, and the other 11 only log, with the log off by default.

| # | what failed | today (`NvFBCR.cpp`) | title | text (before the log hint) |
|---|---|---|---|---|
| 1 | another relay holds the lock | popup, `:709` | already running | Another NvFBCR is already running, and only one can run at a time.<br><br>Close it first. If no window is visible, end NvFBCR.exe in Task Manager. |
| 2 | Direct3D 9Ex cannot be created | unchecked, `:265`; a failure would crash on the next line | could not start | NvFBCR could not start the Windows graphics system it draws with (Direct3D 9).<br><br>Restart the PC. If that does not help, update or reinstall the graphics driver. |
| 3 | fewer than two displays | not checked | needs two displays | NvFBCR shows one display's picture on another, so it needs two displays, and Windows reports only 1.<br><br>Check that the capture card is connected and turned on, and that Windows extends the desktop onto it (Settings, System, Display: Extend these displays). Then start NvFBCR again. |
| 4 | the display list cannot be read | log, `:718`, and never taken: the check tests the wrong value | (not a failure) | Friendly names fall back to device names (section 3). |
| 5 | `NvFBC64.dll` missing or incomplete | log, `:882` | NVIDIA capture not found | NvFBCR could not load NvFBC64.dll, the NVIDIA screen capture library that comes with the NVIDIA graphics driver.<br><br>Install or update the NVIDIA graphics driver, then start NvFBCR again. |
| 6 | NvFBC is off and the process is not administrator | console, `:930` | NVIDIA capture is off | NVIDIA screen capture (NvFBC) is turned off on this PC, and turning it on needs administrator rights, which NvFBCR does not have.<br><br>Sign in with an administrator account and start NvFBCR again, or ask an administrator to run NvFBCEnable.exe -enable once. |
| 7 | turning NvFBC on fails | log, `:963` | NVIDIA capture is off | NVIDIA screen capture (NvFBC) is turned off on this PC, and NvFBCR could not turn it on (error 0x...).<br><br>Run NvFBCEnable.exe -enable, then start NvFBCR again. If that fails too, update the NVIDIA graphics driver. |
| 8 | the relaunch does not start | log, `:1012` | could not restart | NvFBCR turned on NVIDIA screen capture, but could not restart itself to use it (error ...).<br><br>Start NvFBCR again. |
| 9 | still off after the relaunch | not handled: relaunches again | NVIDIA capture is off | NvFBCR turned on NVIDIA screen capture and restarted, but capture is still not available.<br><br>Restart the PC, then start NvFBCR again. If that does not help, update the NVIDIA graphics driver. |
| 10 | a window cannot be created | log for the host window, `:865`; unchecked for the output window | could not start | NvFBCR could not create its window on the capture card display (error ...).<br><br>Start NvFBCR again. If this keeps happening, restart the PC. |
| 11 | the present device or its back buffer cannot be created | log, `:900`, `:909` | could not start | NvFBCR could not set up drawing on the capture card display (error 0x...).<br><br>Check that the capture card display is connected and turned on, then start NvFBCR again. If this keeps happening, update the graphics driver. |
| 12 | `-flipex` device refused | popup and log, `:476` | flip mode unavailable | -flipex was asked for, but the graphics driver refused it (error 0x...). NvFBCR does not fall back to its usual output, because the capture would then be labelled wrongly.<br><br>Start NvFBCR without -flipex. |
| 13 | another program holds a capture session | log, `:1019`, with stale driver advice | capture in use | Another program is using NVIDIA screen capture, so NvFBCR cannot capture the game display.<br><br>Close other capture and streaming programs, and end any NvFBCR.exe still listed in Task Manager. Then start NvFBCR again. |
| 14 | the capture session cannot be created or set up for another reason | log, `:1019`, `:1047` | could not start | NvFBCR could not start capturing the game display (error ...).<br><br>Start NvFBCR again. If this keeps happening, restart the PC or update the NVIDIA graphics driver. |
| 15 | the mode cannot set up | log, `:1066` | could not start | NvFBCR could not start the mode you chose.<br><br>Start NvFBCR again, and press Enter at the mode prompt for the default mode. |
| 16 | the ring does not start | log only, inside the mode | as 13 or 14 | as 13 or 14 |
| 17 | the present path refuses | popup inside the mode | could not start | NvFBCR could not set up its output on the capture card display, so it will not start.<br><br>To try the older output method, type b:dwm at the mode prompt. (today's text) |
| 18 | capture lost during the run | log only | stopped | NvFBCR stopped because it lost the capture of the game display. This can happen when the game display changes resolution.<br><br>Start NvFBCR again. |
| 19 | the output stalled during the run | log only | stopped | NvFBCR stopped because the capture card display stopped taking new frames.<br><br>Check that the capture card is connected and turned on, then start NvFBCR again. To try the older output method, type b:dwm at the mode prompt. |

An invalid mode is no longer an exit: it re-asks (section 4). Rows 18 and 19 are decision 5.

## 11. The log contract

**Parsed by a tool.** `mktrace.py` reads `Resolved options: src rate hint <n> fps` and
`Resolved options: ... extra lag <n> ms`. The whole `Resolved options:` line is kept word for
word, field for field, in the same order, which also keeps `relaylog.py` echoing it as a header
line (it echoes any line containing "comb lock"). `mktrace.py`'s comb-lock pattern and
`relaylog.py`'s other header keys come from `TemporalCaptureMode.cpp`, which this work does not
reword.

**The startup lines, in order** (tags change to the new files; `startupcmp.py` compares message
text only):

| line | status |
|---|---|
| `NvFBCR starting` (`NvFBCR starting, relaunched after turning on NvFBC` in a relaunched process) | new first line, replaces `=== NvFBCR Starting ===` which today comes third |
| `Wall clock <date> <time> UTC (unix <n> ns) at QPC <ticks>` | new, decision 7 |
| `Command line: '<text>'` | was `Command line received: '<text>'` |
| `Display [<n>] <name> (<device>), <w>x<h> at (<x>,<y>), <hz> Hz`, one per display | new |
| the loader's two load lines | unchanged, now before the choice |
| `Launch from the command line: game display <n>, capture card display <n>, mode '<mode>'`, or `Launch from the prompts: game display <n>, capture card display <n>, mode line '<answer>'` | new, replaces `Parsed args - hasArgs: ...` |
| `Command line not used: <reason>`, `Prompt answer refused: '<answer>': <reason>` | new, one per refusal |
| the dependency note, `Delivery-lateness correction off: ...` | unchanged |
| `-flipex ignored: ...` | unchanged for `b:vsync`; a new variant for the modes that grab into the back buffer |
| `Source display: ...`, `Target display: ...`, `Target display refresh: ...`, `Capture mode: ...` | unchanged |
| `Resolved options: ...` | unchanged, word for word; now built by `launch::ResolvedOptionsLine`, which the suite pins |
| `NvFBC status: capture possible <yes/no>, can create now <yes/no>` | new |
| `Buffer size: ...`, `D3D9 devices hosted on a hidden window; ...`, `Present device adapter: ...`, `Display mode on adapter ...`, the flip-mode latency line | unchanged |
| the loader's `NvFBC instance created successfully (...)` | unchanged |
| `Startup capture session set up on the back buffer (hardware cursor on, 10-bit ARGB, no-wait grab)` | replaces `NvFBCToDX9Vid instance created successfully` |
| the mode's own setup lines | unchanged |
| `Entering capture loop - mode: ...` | unchanged |

**Relaunch lines** (in the first process): `NvFBC status: ...`, the loader's `NvFBC is enabled`,
`Relaunching with command line: '...'` (unchanged). With decision 4 these stay in the file,
followed by the relaunched process's lines.

**Teardown lines:** `Shutting down: <why>` (the output window closed, a failure, relaunching,
or the console's input ended), the mode's summaries, the loader's `NvFBC library closed`, then
`Shutdown complete`, and after a failure `Stopping: <reason>` as the last line. These replace
`Cleanup started` and `Cleanup completed`.

The reference logs (`defaults.log`, `cli_shortcut.log`, `cli_badindex.log`, `cli_relaunch.log`)
are replaced by logs from the new build (section 16); `startupcmp.py` needs the wall-clock line
masked the way it masks the QPC origin, if decision 7 is taken.

## 12. Changes from today

| | today | after |
|---|---|---|
| 1 | 11 of 13 startup failures only log | every failure has a popup (section 10) |
| 2 | an invalid mode logs and exits | the prompt asks again; a command line goes to the prompts |
| 3 | an unknown option or bad value at the prompt is printed and ignored, and the console closes before it can be read | the prompt asks again |
| 4 | an options-only answer (`-src 60`) exits: `-src` becomes the mode | the default mode with those options |
| 5 | a command line with a problem falls to the prompts silently, carrying its valid options and its mode into them, so Enter may not give the default | the console says why and the prompts start from the defaults (decision 1) |
| 6 | the same display for both is accepted | refused (decision 2) |
| 7 | the output window is shown before NvFBC is found to be off, then the process relaunches | NvFBC is settled before any window |
| 8 | the relaunched process truncates the first one's log | it appends (decision 4) |
| 9 | nothing bounds the relaunch | one relaunch at most |
| 10 | a missing `NvFBC64.dll` is found after the prompts, and only logged | found before them, with a popup |
| 11 | not-admin advice says to run NvFBCR.exe normally | plain advice that is true under the manifest |
| 12 | a run that stops by itself only logs | popup (decision 5) |
| 13 | friendly names matched by position | matched by device name |
| 14 | lines printed after the mode answer vanish | dropped |
| 15 | `-flipex` accepted with `vsync`, timer and `diag` modes | ignored with a log line |
| 16 | teardown releases a session after its device and leaves the windows to process exit | section 9's order |
| 17 | startup log as in `defaults.log` | section 11 |

**Not changed:** the exe name, the log file and its opt-in rule, the single-instance lock's name,
every flag and mode and the usage list, the prompt wording, the `Resolved options:` line, every
Direct3D and NvFBC parameter, the present device's adapter choice, the temporal modes' session
rebind, the admin manifest, and every per-frame and summary line.

## 13. Bugs found reading today's shell

- `NvFBCR.cpp:718`: `if (!InitDisplays())` never fires on failure, because `InitDisplays`
  returns `HRESULT_FROM_WIN32(result)`, which is nonzero. `NvFBCR.cpp:265`: the result of
  `Direct3DCreate9Ex` is not checked, and a failure dereferences NULL on the next line.
- `NvFBCR.cpp:323-343`: friendly names assigned by path index (section 3), including a write
  past the end of the list when there are more active paths than adapters.
- `NvFBCR.cpp:845`: neither `RegisterClassEx` nor the output window's `CreateWindowEx` is
  checked.
- `NvFBCR.cpp:646-673`: an options-only answer becomes the mode; `NvFBCR.cpp:659-670` prints
  lines the console closes on at `:688`.
- `NvFBCR.cpp:1006`: an unbounded relaunch when enabling reports success but capture stays
  impossible.
- `CaptureRing.cpp:210-216`: when the ring's own session cannot be created, `NvFBCDX9` still
  points at the session it has just released, and `Cleanup` releases it a second time.
- `CaptureRing.cpp:58` with `NvFBCR.cpp:225`: the ring's session is released after the capture
  device it is bound to.
- Outside the shell, noted and not changed here: `DiagCaptureMode.cpp:95` passes the
  presentation interval as `PresentEx`'s flags, which on `diag:vsync` is `D3DPRESENT_DONOTWAIT`
  (both are 1), the mistake `VsyncCaptureMode.cpp:38-41` warns about. `diag` is a hidden
  diagnostic mode.

## 14. Decisions for you

| # | question | recommendation |
|---|---|---|
| 1 | A command line with a problem: start the prompts from the defaults, or carry its valid parts into them? | **Defaults.** The console says what was wrong, and what the user then types is what runs. Carrying parts over means Enter at the mode prompt may not give the default the prompt promises. A shortcut with a retired flag (`-subgen`) now stops at the prompts saying so, where today the flag is skipped. |
| 2 | Refuse the same display for game and capture card? | **Yes.** The topmost output window would cover the game it captures. |
| 3 | Re-ask at the mode prompt for an unknown option or a bad value too, not only for an invalid mode? | **Yes.** Today they are printed and ignored, and the console closes before the line can be read. |
| 4 | Keep the first process's lines when it relaunches (the child appends, marked by a hidden `-relaunched` flag)? | **Yes.** The marker is needed anyway to stop a second relaunch; appending costs one branch in the logger's open. |
| 5 | Popups for a run that stops by itself (capture lost, output stalled)? | **Yes.** The output window closes either way; without a popup the relay seems to vanish mid-stream. |
| 6 | Pass one `RelayContext` to the modes instead of the twelve `extern` globals? | **Yes.** The session gets one owner, which fixes both ring bugs in section 13, and the mode files are being touched for the message pump regardless. The alternative keeps the globals, defined in the new files. |
| 7 | A UTC wall-clock line at startup, paired with a QPC reading, to align the log with the OBS frame trace's `unix_ns`? | **Yes.** One line; `startupcmp.py` masks it. |
| 8 | ASLR for the relay in the new project file (today `RandomizedBaseAddress` is false; both tools have it on by default)? | **Yes, on**, matching the tools and the template; the import check and the Defender scan judge it. |
| 9 | `NvFBCR.slnx` (the XML solution format VS2026 creates) instead of a new `NvFBCR.sln`? | **Yes**, if the first CI run builds it; it has no GUID boilerplate. Both workflows name the solution and change with it. |
| 10 | Move the output window when the desktop layout changes (the README's display-coordinates note)? | **Not in this phase.** It is new behavior with its own tests; the README keeps the advice, rewritten in our words. |

## 15. The rest of phase 7

- **Project file** from a clean VS2026 template with a new GUID, keeping what today's encodes:
  x64 Debug and Release, toolset `v145`, `MultiByte`, static runtime (`MultiThreaded`,
  `MultiThreadedDebug`), `RequireAdministrator`, Windows subsystem, include directories
  `../common;../core;../../third_party`, the pre-build step that makes `nvofapi64.lib` from
  `nvofapi64.def` into `$(IntDir)` with `$(IntDir)` on the library path, the libraries the code
  links (`d3dcompiler`, `dxva2`, `d3d9`, `winmm`), the map file in Release, the preprocessor
  definitions as they are, output to `build/$(Configuration)/$(Platform)/`, and Release
  optimization exactly as it is (phase 8 decides it). Dropped as template leftovers with no
  effect on the Release build: `MinimalRebuild`, the `Midl` block, `_ProjectFileVersion`,
  `ProjectName` (the file name gives the same), and the explicit copy of the default library
  list. ASLR on, per decision 8. Stored LF like every project file and checked out CRLF.
- **Solution** per decision 9, with the three projects; `.gitattributes` gives `*.slnx` the
  same CRLF checkout as `*.sln`.
- **C++20:** `stdcpp20` in all three projects; `-std=c++20` in `dev-build.yml`'s suite step (the
  only workflow that compiles the suite; `release.yml` has none) and in the local suite command.
  `/std:c++20` turns on `/permissive-`; CI names whatever the existing modules need.
- **README:** Collin's prose (the intro, Prerequisites, the display-coordinates note and "Why?")
  replaced with our own; "Why?" either goes or comes back built only on the cost ladder
  (`docs/relay-cost-results.md`); Credits keeps a line that the project began as his relay, with
  the link. The stale items fixed at the same time: "In progress" (`etw-frame-timing` is merged),
  the Architecture section's 8 ring slots (16, or 32 at `-lag 75`), and the intro's claim that
  nothing leaves the D3D9 context (`b:vsync` presents through D3D11).

## 16. Checks and runs

**Checks:** `git blame -w -M -C` over the tree finds no line by CB; the suite output is the
baseline's 527 lines, byte for byte; CI green on both jobs; the import check against
`import-baseline-75af577` with every difference explained (expected: the console, window and
time calls the new behavior uses); a Defender scan of all three exes; `tagcheck.py` on every
run's log.

**Runs** (`NvFBCR.log` present beside the exe unless the row says otherwise):

| # | content | what you type | mode | flags | log | expect |
|---|---|---|---|---|---|---|
| 1 | desktop, 10 s | `0`, `1`, Enter | `b:vsync` | none | `shell_default.log` | runs; startup lines as section 11; `Resolved options:` identical to `defaults.log`'s; becomes the new default reference |
| 2 | desktop, 10 s | `0`, `1`, `-src 60` | `b:vsync` | `-src 60` | `shell_optsonly.log` | runs with src 60 (today this exits) |
| 3 | desktop, 10 s | `0`, `1`, `bogus`, `b:vsync -srcc 60`, `b:vsync -lag 500`, Enter | `b:vsync` | none | `shell_reask_mode.log` | three re-ask messages; runs the default; three `Prompt answer refused` lines |
| 4 | desktop, 10 s | `5`, `x`, `0`, `0`, `1`, Enter | `b:vsync` | none | `shell_reask_disp.log` | "no display 5", "type the number", "must be different"; runs |
| 5 | desktop, 10 s | shortcut `-source 0 -target 1 -src 60` | `b:vsync` | `-src 60` | `cli_shortcut.log` | no console |
| 6 | desktop, 10 s | shortcut `-source abc -target 1 -nolock`, then `0`, `1`, Enter | `b:vsync` | none (the command line is not used) | `cli_badindex.log` | "The command line was not used: -source must be a display number"; comb lock on |
| 7 | desktop, 10 s | shortcut `-source 7 -target 1`, then `0`, `1`, Enter | `b:vsync` | none | `cli_noindex.log` | "there is no display 7" |
| 8 | desktop, 10 s | shortcut `-source 0 -target 1 -framerate bogus`, then `0`, `1`, Enter | `b:vsync` | none | `cli_badmode.log` | "'bogus' is not one of the modes" |
| 9 | desktop, 10 s | `NvFBCEnable.exe -disable`, then shortcut `-source 0 -target 1 -framerate b:vsync -src 90 -nolock` | `b:vsync` | `-src 90 -nolock` | `cli_relaunch.log` | one relaunch, no window before it; the log holds both processes; display names move on (1/2 to 11/12) |
| 10 | desktop | start a second `NvFBCR.exe` during run 1's kind of run | | | first run's log | "already running" popup; the first run's log intact |
| 11 | desktop | `0`, `1`, Enter; after 10 s change the game display's resolution in Windows Settings, then change it back | `b:vsync` | none | `shell_capture_lost.log` | expected: "stopped" popup naming the lost capture, with "The reason is in NvFBCR.log." If the run carries on instead, that is the answer to record |
| 12 | desktop | as 11 with no `NvFBCR.log` | `b:vsync` | none | none | the same popup with the "create an empty NvFBCR.log" hint |
| 13 | 60x2 gameplay, a few minutes | `0`, `1`, `b:vsync -src 60` | `b:vsync` | `-src 60` | `shell_60x2.log` | per-frame behavior in line with recent 60x2 logs: lock holds, synth share, long blend runs, present gaps |

The kickoff listed "a display index that does not exist on the command line" among the popups to
provoke; under this specification it is not a popup but a console line and the prompts (row 7),
which is what the README already promises ("a number that no longer exists sends the relay back
to asking"). Failures 2, 3, 5 to 9, 13 and 14 cannot be provoked safely on the rig and are
checked by reading the code.
