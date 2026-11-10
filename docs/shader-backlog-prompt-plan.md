# Pause gameplay for shader compilation: implementation plan

Status: implemented as an opt-in desktop Vulkan feature. Desktop compilation and
automated tests pass. Cold/warm gameplay checks, frame-time/overhead measurements
and threshold tuning remain manual work.

Validation on Ubuntu 24.04, GCC 13.3, Qt 6.4.2 and CMake 3.31.10:

- `cmake --build build --target shader_jobs_tests shader_backlog_tests yuzu --parallel 2`
  completed, including the `build/bin/lichii` executable.
- `ctest --test-dir build -R '^(shader_jobs_tests|shader_backlog)$' --output-on-failure`
  passed both suites: 22 scheduler cases and 17 reported Qt checks (including
  initialization/cleanup).
- `git diff --check` passed. No live game was started or stopped for validation.

Implementation notes:

- Runtime pipeline jobs have explicit origin metadata. Startup/cache prebuilds and
  stage-only translation jobs remain outside the trigger.
- A finite group of job handles supplies progress without reassigning live batches.
  Each frontend controller is scoped to one emulation session and is destroyed
  before normal shutdown/restart; no stale controller is reused for another game.
- Code inspection confirms guest pause suspends guest execution/timing without
  stopping the shader scheduler. Tests exercise worker progress while a simulated
  guest stays paused; an affected live game has not been exercised yet.
- The asynchronous pause path observes the core's acknowledged paused state.
  Manual pause/resume, focus handling, cache reload and shutdown manage ownership.
- Upstream nx2 was fetched and inspected for an equivalent implementation. GitHub
  API PR/issue searches were unavailable in this environment.

## Goal and scope

Offer a choice when a large shader compilation backlog persists during gameplay:
pause and finish the known builds, or continue playing while they compile.

The first version targets the desktop Qt frontend and Vulkan renderer. It does not
predict upcoming scenes, discover shaders while the guest is paused, resume the
entire skipped startup precache, or change asynchronous rendering settings.
OpenGL and headless frontends retain their current behavior.

Add an opt-in global/per-game setting under Graphics > Advanced:
**Ask to pause when many shaders are compiling**. Default it to off initially.
The choice is separate from the startup precache setting and must not be changed
silently by graphics presets.

## Existing integration points

- `src/video_core/shader_jobs.h/.cpp`: the Vulkan shader scheduler already exposes
  synchronized queued/running/completed/cancelled/failed counters and owns its
  worker pools. Priority is not job origin: Background jobs can come from cache
  prebuilding, while runtime requests can be promoted.
- `src/video_core/renderer_vulkan/vk_pipeline_cache.h/.cpp`: owns the scheduler,
  submits pipeline work and loads startup/cache prebuilds.
- `src/video_core/shader_notify.h/.cpp`: the existing Building label deliberately
  retains completed work briefly. Its displayed count is unsuitable as a backlog
  trigger.
- `src/yuzu/main_window.h/.cpp`: already polls status every 500 ms and provides
  `OnPauseGame()` / `OnStartGame()`.
- `src/qt_common/render/emu_thread.cpp`: pause requests reach the emulation control
  thread asynchronously. A UI pause request is not proof that the guest has paused.
- `src/common/settings.h`, `src/qt_common/config/shared_translation.cpp` and
  `src/yuzu/configuration/configure_graphics_advanced.cpp`: setting registration,
  translated labels/tooltips and generated Advanced controls.

## 1. Prove pause and compilation can coexist

Before wiring the dialog, trace runtime pipeline jobs and verify they can finish
with the guest paused. Check stage continuations, captured shader environments,
driver work and resource lifetime. They must not require new guest commands or
the UI thread to make progress.

Exercise the normal pause path with blocked/in-flight test jobs. Confirm that
compilation continues and shutdown still cancels/joins correctly. Do not add a
GPU-idle wait to the UI thread: a shader stall could make it block indefinitely.

Also survey upstream branches/PRs for an equivalent feature before implementation
to avoid duplicating existing work. Preserve unrelated local edits, including the
graphics preset changes already present in this checkout.

Deliverable: verified pause/worker behavior and a documented list of runtime job
submission sites. If workers depend on guest progress, resolve that dependency
before offering an automatic wait action.

## 2. Add accurate, thread-safe backlog reporting

Expose an immutable compilation snapshot through a narrow renderer/rasterizer
interface. Unsupported renderers report unsupported, rather than a misleading
empty successful result. Qt must never access the private scheduler directly.

Track runtime pipeline work separately from startup precache, cached prebuild and
maintenance work. Add explicit job-origin metadata where needed; do not infer
origin from priority. Count each logical build once across CPU and driver stages,
without counting its stage translations again. Audit synchronous paths and report
the feature's coverage honestly rather than implying every shader stall is queued.

The snapshot should include:

- Queued and running runtime builds, separated from other work.
- Terminal success, failure and cancellation counters.
- Age of the oldest pending runtime build and recent completion throughput.
- A session identifier so stale UI updates cannot affect a restarted game.

Provide a nonblocking way to capture and poll a finite group of the outstanding
runtime builds when the player chooses to wait. It must cover both queued and
running jobs, retain state safely through shutdown, and count failed/cancelled
jobs as terminal. Existing batches group jobs at submission time; do not reassign
live jobs to a new batch merely to wait for them.

Do not copy shader payloads, hold scheduler locks while updating Qt, or scan an
ever-growing job history on each poll. Reuse scheduler accounting and bound the
extra bookkeeping by outstanding work.

## 3. Implement a testable detection policy

Use a small policy/controller separate from widgets, with an injectable clock.
Poll at 500 ms, independently of whether the status bar is visible.

Initial tuning values, subject to measurement:

- At least 64 pending runtime builds continuously for 2 seconds.
- The oldest outstanding runtime build is at least 1 second old.
- Re-arm only after fewer than 8 remain continuously for 2 seconds.
- At least 60 seconds between prompts.

These detect sustained work, not proven frame drops. Avoid hard-coded FPS targets:
games may intentionally run at 30 FPS or vary their frame rate. Log backlog size,
age, throughput and available frame statistics when prompting so thresholds can
be evaluated with real sessions. Do not show an unverified ETA.

Suppress detection during startup/cache reload, shutdown, restart, user pause,
debugger stops, another modal/app-level blocking flow, or while this feature owns
a pause. Reset it for each game session. Disabling the option or selecting
Don't ask again this session suppresses further prompts.

Recheck the backlog immediately before requesting a pause and once pause is
acknowledged. If it already cleared, avoid opening a stale prompt and release only
the pause owned by this feature.

## 4. Add the pause and choice flow

Use an asynchronous, window-modal Qt dialog owned by MainWindow, with the existing
application styling and translated strings. Do not block the UI on job waits or
enter a synchronous scheduler drain.

State flow:

`Monitoring -> Pause requested -> Choice -> Waiting -> Resume -> Cooldown`

`Choice -> Continue playing -> Resume -> Cooldown`

Any active state can terminate on shutdown, restart, session change or renderer
loss. Session suppression is orthogonal to these states.

After confirmed pause, show:

> Shaders are still compiling
>
> N shader builds are pending. Continuing may cause stutters or temporarily
> missing effects, depending on your rendering settings.
>
> [Wait for builds] [Continue playing]
>
> [ ] Don't ask again this session

While the choice is open, keep the pending count live. If the backlog clears,
dismiss the obsolete prompt and safely resume.

**Wait for builds** captures the remaining group and switches the dialog to
progress: `Completed X of Y builds`, remaining count and a **Continue playing**
button. Poll the group even though gameplay is paused. If more work arrives from
commands already in flight, keep it outside the fixed progress total and show its
remaining count separately. Do not promise all future shader compilation is done.

Resume when the captured group reaches terminal states. If failures occurred,
report them without claiming all builds succeeded. A lack of progress must leave
Continue playing usable; never automatically cancel driver compilation.

Continue playing, Escape and closing the dialog resume gameplay without cancelling
shader builds. Don't ask again this session affects future prompts only.

Track a session-scoped pause owner/token. Resume only if this feature still owns
the pause and the same game is active. Coordinate with background-focus pause,
manual pause, debugger stops and applet dialogs so none can accidentally resume
or be resumed by this feature. Shutdown/restart invalidates the token and closes
the dialog before renderer resources are destroyed.

## 5. Tests and validation

Add focused tests alongside the existing shader scheduler tests, plus Qt/controller
tests using simulated snapshots rather than a real game:

- CPU-to-driver transitions, priority promotion and failures do not duplicate or
  leak pending counts; cached prebuilds do not trigger a runtime prompt.
- Fast bursts do not prompt; sustained backlogs do; hysteresis, cooldown and
  session suppression prevent repeated prompts.
- Workers complete the captured work while the guest is paused; new jobs do not
  inflate its progress total; failure/cancellation cannot leave it waiting forever.
- Clearing the backlog while requesting pause or displaying the choice avoids a
  stale dialog and preserves correct pause ownership.
- Continue, Escape, dialog close, user pause, focus changes, stop, restart and
  renderer loss never resume the wrong session or access destroyed state.
- Disabled, unsupported renderer and headless paths retain current behavior.

Run focused tests and a desktop Vulkan compile using the configured build tools.
Validate manually with a cold-cache scene and then a warm-cache repeat: verify
actual pausing, audible/input behavior, progress, safe resume and no repeated
prompt. Measure polling/bookkeeping overhead and compare compilation/frame times
with the setting off. A completed compile/test run does not establish game-level
correctness.

Document the option and its limits in `docs/user/Graphics.md`, preserving the
existing local edits. Keep it opt-in until gameplay measurements support the
thresholds and pause behavior.

## Acceptance criteria and delivery order

1. Validate worker progress during pause and finish the telemetry interface.
2. Implement and test the detection/controller policy.
3. Wire the setting, asynchronous dialog and pause ownership.
4. Run compile/tests, perform cold/warm gameplay checks and update user docs.

The feature is ready when a sustained runtime backlog produces one actionable
choice, waiting completes known work without freezing the UI, continuing leaves
compilation running, and every exit path preserves the correct game/pause state.
Predictive discovery, background continuation of the full startup precache,
adaptive worker limits and automatic compile-speed boosts are separate work.
