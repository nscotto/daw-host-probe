# DAW Log Capture Cases

This document defines the canonical log-capture cases for DAW reconnaissance.
Use it before freezing the deterministic-loop contract and before building the
synthetic host harness. The target is broad, comparable evidence about host
behavior, not plugin correctness testing or exhaustive audio QA.

The first pass should answer: "What does this DAW send to a plugin under common
and awkward workflows?" It should avoid judging whether the plugin handled that
data correctly except when a plugin problem prevents the host data from being logged.
Any plugin behavior noticed during this phase should be treated as a side
finding and validated later with proper tests.

Each captured case should record the host, host version, plugin format, plugin
build, sample rate, buffer size, live/offline mode, project tempo, loop range,
transport flags, event ordering, note IDs where available, event times, and the
log filename.

## Logging Readiness

Do not start the full reconnaissance sweep until the debug log can describe raw
host behavior without inferring it from the plugin's internal reactions.

Preferred capture tool: a dedicated host-probe plugin. The probe should be
CLAP-first internally and should be wrapped to VST3 through the same wrapper
strategy used by the probe. Do not build a separate VST3 probe implementation unless
the wrapper prevents required host data from being observed.

Minimum required logging before Phase 0:

- One process-block line per host callback with block sequence, absolute audio
  frame, frame count, input event count, output mode if known, and live/offline
  hint if the wrapper exposes one.
- Full transport snapshot at block start and for every transport event:
  decoded flags, playing/stopped state, tempo, song position in seconds and
  beats, loop active state, loop start/end in seconds and beats, and which
  timeline fields are valid.
- One raw input-event line before handling each event: event index, event time
  within block, event type, event space, and the block sequence it belongs to.
- Raw note event payloads: note id, port, channel, key, velocity, and event
  time. Note expression should be logged if hosts send it.
- Raw MIDI-byte event payloads: port, status, data bytes, and event time.
- Raw parameter/automation event payloads: parameter id, value, event time, and
  whether the event arrived from the host event queue or the plugin UI path.
- Predicted or observed loop wrap line with block sequence, wrap frame inside
  the block, previous transport position, current transport position, and loop
  source used for the prediction.
- Lifecycle lines for activate, deactivate, start processing, stop processing,
  reset, and plugin destroy.

Current status: the existing CLAP logging is useful but not enough for the full
reconnaissance pass. It already logs some lifecycle events, transport summaries,
notes, subblocks, and predicted wraps, but it does not consistently log input
event index, event time, event type before handling, decoded transport fields,
both seconds and beats timelines, automation payloads, or explicit process-block
sequence boundaries. Add a minimum logging pass first, then use the cases below.

## Capture Levels

Use three levels so the work can progress without waiting for every host to
cover every edge case.

- Level 1: required for every host and format.
- Level 2: required for primary hosts and any host showing suspicious behavior.
- Level 3: optional stress and characterization cases.

Primary hosts are REAPER CLAP, REAPER VST3, Ableton Live VST3, Bitwig CLAP or
VST3, and Renoise. Reason, Cubase, FL Studio, and Logic can start with Level 1
plus any host-specific cases that are easy to capture.

## Shared Project Setup

Use the same baseline project where practical:

- `140 BPM Loop 5 - The Winstons - Amen.wav` feeding the probe, placed at bar
  1.1.1.
- MIDI sidechain routed to the probe.
- Diagnostic logging enabled.
- Plugin parameters kept neutral unless the case explicitly observes automation
  or parameter-event delivery.
- Project tempo set to 140 BPM.
- Standard arrangement loop from bar 2.1.1 to bar 3.1.1. This is one bar,
  4 beats, or 1.714286 seconds.
- One non-zero loop-start variant at bar 3.1.1 to bar 4.1.1.
- MIDI case selected from the case library below.
- Playback repeated for four loop wraps, then stopped during the fifth pass.
- Live playback capture.
- Offline render capture where the host supports it.

## Operator Timing Protocol

Every live capture should say exactly when playback starts, how many loop wraps
are observed, and when playback stops. Use these defaults unless a case says
otherwise.

For arrangement-loop cases:

1. Arm logging and clear or rotate the log file before pressing play.
2. Place the playhead at bar 1.1.1.
3. Press play.
4. Let playback enter the loop at bar 2.1.1 and cross the loop end at bar 3.1.1
   four times.
5. After the fourth wrap, continue into the fifth pass.
6. Press stop near the middle of the fifth pass, around bar 2.3.1, not exactly
   on a note boundary or loop boundary.
7. Wait at least two seconds with transport stopped before ending the capture.

For start-inside-loop cases:

1. Arm logging and place the playhead near the middle of the loop, around bar
   2.3.1.
2. Press play.
3. Let playback complete three loop wraps.
4. Press stop inside the loop.
5. Wait at least two seconds with transport stopped.

For stop/restart cases:

1. Press play from bar 1.1.1.
2. Let playback complete two loop wraps.
3. Press stop inside the loop, away from note and loop boundaries.
4. Wait two seconds.
5. Press play again from the same stopped position.
6. Let playback complete two additional loop wraps.
7. Press stop and wait two seconds.

For seek cases:

1. Press play from bar 1.1.1.
2. Let playback enter the loop.
3. Seek to a different position inside the loop while playback continues.
4. Let playback complete two loop wraps after the seek.
5. Press stop and wait two seconds.

For live-edit cases:

1. Press play from bar 1.1.1.
2. Wait until the second loop pass has begun.
3. Perform exactly one edit.
4. Let playback complete two wraps after the edit.
5. Perform the next edit only after those two wraps are complete.
6. Press stop after the final edit has been observed for two wraps.

For clip/session-loop cases:

1. Arm logging before launching the clip.
2. Use a one-bar clip loop where practical, and record the exact clip loop
   start, end, and length if the host uses clip-relative positions.
3. Launch the clip or scene.
4. Let the clip complete four clip-loop wraps.
5. Stop during the fifth clip pass where the host allows that timing.
6. If arrangement transport is also running, record whether arrangement loop is
   enabled or disabled.
7. Stop the clip first, then stop transport if the host separates those actions.
8. Wait two seconds before ending the capture.

For offline render captures:

1. Use the same project and case ID as the live capture.
2. Render from bar 1.1.1 through bar 3.3.1 unless the case specifies another
   loop range.
3. Include the pre-roll, four complete loop wraps, and half of the fifth pass in
   the render range.
5. Record whether the host uses realtime, offline, freeze, bounce-in-place, or
   another render mode.

## MIDI Case Library

Use the probe UI's `Write MIDI files` button to generate purpose-built MIDI
fixtures under the selected log folder's `midi/` directory. Import or drag the
named file into the DAW at the project bar stated by the case.

### M1 Single Anchor Note

- Generated file: `single-c4-anchor.mid`.
- When placed at the loop start, C4 starts at that same project bar, lasts one
  quarter note, and uses velocity 100.

Purpose: establish basic note-on/note-off timing, note ID behavior, and
pass-to-pass event stability with the least possible ambiguity.

### M2 Sparse Multi-Anchor Notes

- Generated file: `sparse-multi-anchor.mid`.
- When placed at the loop start, the file contains sparse notes across the
  one-bar loop with fixed lengths and velocity.

Purpose: sample host event timing at several loop-relative offsets without
same-key or boundary interactions.

### M3 Repeated Same-Key Notes

- Generated file: `repeated-same-key.mid`.
- Four adjacent C4 notes across the first half of the loop, all at velocity 96.

Purpose: observe same-key note identity, note-off matching data, event order,
and whether hosts reuse or regenerate note IDs across loop passes.

### M4 Same-Sample Transition

- Generated file: `same-sample-transition.mid`.
- C4 ends exactly when E4 starts, so the host event order at the shared sample
  can be captured.

Purpose: observe same-frame note-off/note-on ordering without adding overlap or
gate-boundary ambiguity.

### M5 Overlap And Choke

- Generated file: `overlap-and-choke.mid`.
- Includes overlapping same-key C4 notes and a different-key G4 overlap.

Purpose: observe what the host sends for overlapping notes, note stack cases,
and potential choke scenarios.

### M6 Boundary-Held Note

- Generated file: `boundary-held.mid`.
- C4 starts before loop end and extends past the loop boundary.

Purpose: observe whether the host resends active-note state, emits note-off at
wrap, truncates notes, or leaves the plugin to infer held state.

### M7 Boundary-Adjacent Notes

- Generated file: `boundary-adjacent.mid`.
- Includes notes exactly at loop start and close to loop start/end.

Purpose: observe host event timing and transport state close to wrap points.

### M8 Velocity Sweep

- Generated file: `velocity-sweep.mid`.
- Four C4 notes with increasing velocities.

Purpose: observe velocity payload stability across live playback, loop passes,
and offline render.

### M9 Pitch Sweep

- Generated file: `pitch-sweep.mid`.
- Notes step upward from C4 through D4, E4, G4, and C5 with the same length and
  velocity.

Purpose: observe pitch payload stability independently from velocity and note
length effects.

### M10 Live Edit Source

- Generated file: `live-edit-source.mid`.
- Start from the imported sparse-note pattern.
- During playback, move one note start earlier by a small amount.
- Move the same note later by a larger amount.
- Change one note pitch.
- Change one note velocity.
- Shorten one note-off.

Purpose: distinguish host jitter from intentional edits and discover whether
the host sends modified events immediately, on the next pass, or only after
transport restart.

## Level 1: Baseline Host Semantics

Capture these for every available host and plugin format.

### L1.1 Idle And Transport Start

- Audio: place `140 BPM Loop 5 - The Winstons - Amen.wav` at bar 1.1.1.
- Tempo: 140 BPM.
- Arrangement loop: enabled.
- Loop: start bar 2.1.1; end bar 3.1.1.
- MIDI sidechain: enabled, with no MIDI clip for this case.
- Initial transport state: stopped, playhead at bar 1.1.1, plugin loaded.
- First start/stop: press play from bar 1.1.1, then stop during the first loop
  pass before the first wrap, around bar 2.3.1.
- Restart: wait two seconds, then press play again from around bar 2.3.1.
- Loop observation: let playback wrap twice at loop end bar 3.1.1.
- Seek: while playback continues, seek back to around bar 2.3.1.
- Post-seek observation: let playback wrap two more times at loop end bar
  3.1.1.
- Final stop: stop during the next loop pass around bar 2.3.1, not on a note
  boundary and not on loop start/end.

Purpose: establish transport flag behavior, position continuity, reset behavior,
and stopped-state silence without spending time on MIDI; MIDI event baselines
are captured in L1.2.

### L1.2 Arrangement Loop and MIDI Baseline

Goal: baseline the host's one-bar arrangement loop while sweeping the core
generated MIDI files in separate capture logs, so the shared setup is
configured only once.

Create one separate capture log for each of these files:
`single-c4-anchor.mid`, `sparse-multi-anchor.mid`, `repeated-same-key.mid`,
`velocity-sweep.mid`, and `pitch-sweep.mid`.

- Audio: place `140 BPM Loop 5 - The Winstons - Amen.wav` at bar 1.1.1.
- Tempo: 140 BPM.
- Arrangement loop: enabled.
- Loop: start bar 2.1.1; end bar 3.1.1.
- MIDI sidechain: enabled.
- MIDI files: place exactly one generated file at project bar 2.1.1 for each
  capture log.
- Plugin parameters neutral.
- Initial playhead: bar 1.1.1.
- Run: press play from bar 1.1.1, count four wraps at loop end bar 3.1.1, then
  stop during the fifth loop pass around bar 2.3.1.

Capture:

- Note-on and note-off times.
- Note IDs or host identity fields where available.
- Channel, key, velocity, and note-expression data where available.
- Event ordering within the process block.
- Differences between first pass and later loop passes.

Purpose: capture the basic loop source, wrap mechanism, event repetition, and
raw MIDI semantics before testing how the plugin's MIDI handling responds.

### L1.3 Live Versus Offline

- Capture live playback.
- Capture offline render or freeze/export where supported.
- Audio: place `140 BPM Loop 5 - The Winstons - Amen.wav` at bar 1.1.1.
- Tempo: 140 BPM.
- Arrangement loop: enabled.
- Loop: start bar 2.1.1; end bar 3.1.1.
- MIDI sidechain: enabled.
- MIDI files: place `single-c4-anchor.mid` at project bar 2.1.1 in both live
  and offline captures.
- Plugin parameters neutral.
- Live run: press play from bar 1.1.1, count four wraps at loop end bar 3.1.1,
  then stop during the fifth loop pass around bar 2.3.1.
- Render from bar 1.1.1 through bar 3.3.1 where the host supports explicit
  render ranges.

Purpose: detect different block cadence, transport flags, event order, and loop
position reporting during offline rendering. Do not treat audio differences as
plugin failures in this phase unless they explain missing or corrupt logs.

### L1.4 Buffer And Sample Rate Spread

At minimum capture:

- 48 kHz with a small buffer, such as 64 or 128 samples.
- 48 kHz with a large buffer, such as 512 or 1024 samples.
- 44.1 kHz or 96 kHz if quick to configure.
- Audio: place `140 BPM Loop 5 - The Winstons - Amen.wav` at bar 1.1.1.
- Tempo: 140 BPM.
- Arrangement loop: enabled.
- Loop: start bar 2.1.1; end bar 3.1.1.
- MIDI sidechain: enabled.
- MIDI files: place `single-c4-anchor.mid` at project bar 2.1.1.
- Initial playhead: bar 1.1.1.
- Each capture log: press play from bar 1.1.1, count four wraps at loop end bar
  3.1.1, then stop during the fifth loop pass around bar 2.3.1.

Purpose: expose block-boundary and latency assumptions without turning the
first pass into full sample-rate QA.

## Level 2: Required Corner Cases

Capture these for primary hosts and any host where Level 1 logs are ambiguous.

### L2.1 Loop Boundary Inside Block

- Choose loop start/end and buffer size so the transport wrap occurs inside a
  process block where possible.
- Use Amen audio at 140 BPM with the audio placed at bar 1.1.1.
- Start from loop start bar 2.1.1 and loop end bar 3.1.1 unless a tiny
  loop-position adjustment is needed to put the wrap inside a block.
- Let playback cross the boundary four times, then stop during the fifth pass.
- MIDI files: place `boundary-adjacent.mid` at the configured loop start.

Purpose: determine whether the host reports wrap early, exactly at the split,
late, or only through position rewind on the next block.

### L2.2 Short Loop Near Buffer Size

- Use a loop whose start and end positions make the loop close to the active
  buffer size.
- Use Amen audio at 140 BPM with the audio placed at bar 1.1.1.
- Use a sub-beat loop only for this case, and record the exact loop start, loop
  end, and length.
- If possible, also test a loop shorter than the buffer size.
- MIDI files: optional; if used, place `single-c4-anchor.mid` at the configured
  short-loop start.
- Let playback cross the short loop boundary at least four times, then stop
  inside the loop.

Purpose: reveal assumptions that only work when a loop spans many process
blocks.

### L2.3 Non-Zero Loop Start

- Move the one-bar arrangement loop to bar 3.1.1 through bar 4.1.1.
- Start playback at bar 2.1.1.
- Let playback cross the loop end four times, then stop during the fifth pass
  near bar 3.3.1.
- MIDI files: place `sparse-multi-anchor.mid` at project bar 3.1.1.

Purpose: confirm whether loop-relative anchors are computed from the true loop
start rather than project position zero.

### L2.4 Loop Edit During Playback

- Move loop start during playback.
- Move loop end during playback.
- Resize the loop while MIDI notes continue to repeat.
- MIDI files: place `sparse-multi-anchor.mid` at project bar 2.1.1, so moved
  loop boundaries can be separated from MIDI edit behavior.
- Use Amen audio at 140 BPM, audio start at bar 1.1.1, initial loop start at
  bar 2.1.1, and initial loop end at bar 3.1.1.
- Start playback at bar 1.1.1, begin edits in the second loop pass, and let two
  wraps complete after each edit before making the next edit.

Purpose: capture whether the host emits explicit loop updates, position jumps,
event flushes, or stale note state.

### L2.5 MIDI Edit During Playback

- Move a note start earlier and later while playback continues.
- Drag a note-off earlier while looping.
- Change note pitch.
- Change velocity.
- MIDI files: place `live-edit-source.mid` at project bar 2.1.1. During
  playback, edit note start, note end, pitch, and velocity.
- Use Amen audio at 140 BPM, audio start at bar 1.1.1, loop start at bar 2.1.1,
  and loop end at bar 3.1.1.
- Start playback at bar 1.1.1, begin edits in the second loop pass, and let two
  wraps complete after each edit before making the next edit.

Purpose: distinguish intentional live edits from host jitter and stale learned
timing by observing what the host sends and when it sends it.

### L2.6 Same-Key And Same-Sample Transitions

- Repeated same-key notes.
- Overlapping same-key notes if the host allows them.
- Same-sample note-off followed by note-on.
- Same-sample note-on followed by note-off if the editor allows it.
- MIDI files: create one separate capture log for each file:
  `repeated-same-key.mid`, `same-sample-transition.mid`, and
  `overlap-and-choke.mid`.
- For each MIDI case, use Amen audio at 140 BPM, audio start at bar 1.1.1, loop
  start at bar 2.1.1, loop end at bar 3.1.1, playback start at bar 1.1.1, four
  loop end crossings, and a stop during the fifth pass near bar 2.3.1.

Purpose: characterize event order, note identity stability, and release/choke
semantics.

### L2.7 Held Notes Across Loop Boundary

- Start a note before loop end and hold it past the wrap.
- Test with gate enabled and disabled.
- Test with retrigger behavior enabled and disabled where applicable.
- MIDI files: place `boundary-held.mid` at project bar 2.1.1.
- For each capture log, use Amen audio at 140 BPM, audio start at bar 1.1.1,
  loop start at bar 2.1.1, loop end at bar 3.1.1, playback start at bar 1.1.1,
  four loop end crossings, and a stop during the fifth pass near bar 2.3.1.

Purpose: observe whether the host sends note-off, note-on, note expression, or
active-note state at the loop boundary.

### L2.8 Automation Inside Loop

- Automate a small set of parameters inside the loop, preferably one continuous
  parameter and one stepped or enum parameter.
- Use Amen audio at 140 BPM, audio start at bar 1.1.1, loop start at bar 2.1.1,
  and loop end at bar 3.1.1.
- Repeat live playback for four wraps, then stop during the fifth pass.
- Capture offline render if supported.

Purpose: observe automation event timing, ordering relative to MIDI, smoothing
or quantization behavior, and live/offline differences before deciding whether
automation remains host-driven or needs loop-relative capture/replay.

## Level 3: Stress And Characterization

Use these after Level 1 and Level 2 have identified unclear host behavior.

### L3.1 Clip Or Session Loops

- Ableton session clip loop.
- Bitwig clip launcher loop.
- Mixed arrangement playback plus clip playback if supported.
- Use a one-bar clip loop where practical.
- Record the exact clip loop start, end, and length if the host uses
  clip-relative positions.
- MIDI files: use `single-c4-anchor.mid` first if MIDI import is available in
  the clip/session workflow.
- Let the clip complete four wraps, then stop during the fifth clip pass where
  the host allows that timing.

Purpose: determine whether clip workflows expose a usable transport loop, only
repeat MIDI/audio events, or report a separate clip-relative timeline.

### L3.2 Tracker Or Pattern Workflow

- Renoise pattern loop.
- Use a one-bar pattern loop where practical.
- Record the exact pattern length and position range.
- Capture four pattern-loop wraps, then stop during the fifth pass where
  possible.
- Pattern jump during playback.
- Repeated note columns or overlapping notes where supported.

Purpose: characterize non-linear event ordering and pattern-loop semantics.

### L3.3 Tempo And Timeline Variants

- Use Amen audio at 140 BPM, audio start at bar 1.1.1, loop start at bar 2.1.1,
  and loop end at bar 3.1.1.
- Tempo change before the loop.
- Tempo automation inside the loop if supported.
- Beat timeline available without valid seconds loop.
- Seconds loop available without valid beat loop if any host exposes it.

Purpose: decide which host timeline is authoritative and when the plugin should
drop to live delayed processing.

### L3.4 Host Panic And Note Cleanup

- Stop playback while a note is held.
- MIDI files: place `boundary-held.mid` at project bar 2.1.1.
- Disable MIDI sidechain while notes are active.
- Bypass or disable the plugin while notes are active.
- Remove or mute the MIDI source while playback continues.

Purpose: verify whether stale notes and gate state can be cleared reliably.

### L3.5 Render Mode Variants

- Use Amen audio at 140 BPM, audio start at bar 1.1.1, loop start at bar 2.1.1,
  and loop end at bar 3.1.1.
- MIDI files: place `single-c4-anchor.mid` at project bar 2.1.1 for each render
  mode captured here.
- Skip realtime playback and standard offline render if they were already
  captured in L1.3 for this host.
- Capture freeze, stem render, bounce-in-place, or any other host-specific
  render path from bar 1.1.1 through bar 3.3.1.

Purpose: capture render paths not already covered by L1.3, focusing on
host-specific freeze, stem render, bounce-in-place, or other alternate render
modes.

### L3.6 Extreme Timing And Latency

- Use Amen audio at 140 BPM with the audio placed at bar 1.1.1.
- Start from loop start bar 2.1.1 and loop end bar 3.1.1.
- Very small buffer.
- Very large buffer.
- High latency settings.
- One-bar loop start close to project start.
- One-bar loop end close to a note boundary.

Purpose: stress event bias, plugin latency compensation, and wrap anchoring.

## Host-Specific Additions

### REAPER

- Capture CLAP and VST3 with the same project.
- Compare live and offline render.
- Test fixed and anticipative processing settings if they affect logs.

### Ableton Live

- Capture arrangement loop and session clip loop separately.
- Capture frozen/exported audio if available.
- Test notes edited while clips keep playing.

### Bitwig

- Capture CLAP if available; otherwise VST3.
- Capture arrangement loop and clip launcher loop separately.
- Capture per-note expression or changing note IDs if present in logs.

### Renoise

- Capture pattern loop and song-position loop if both are available.
- Include tracker-style repeated notes and note cuts.

### Reason

- Capture the supported wrapped format.
- Focus on arrangement loop, render path, and MIDI event order.

### Cubase

- Capture VST3 arrangement loop and offline export.
- Include same-sample or tightly adjacent note transitions where the editor
  permits it.

### FL Studio

- Capture pattern playback and playlist arrangement playback if both are
  practical.
- Capture render/export separately from live playback.

### Logic

- Outsource logs first.
- Capture AU live playback and offline bounce.
- Include single anchor note, sparse multi-anchor notes, repeated same-key
  notes, boundary-held notes, velocity sweep, and pitch sweep with rendered
  audio input.

## Classification

After each case, classify the result as one of:

- Host behavior captured and understood.
- Host behavior captured but still ambiguous.
- Logging insufficient.
- Host exposes timing behavior that must influence the contract.
- Plugin issue noticed incidentally and queued for later testing.
- Unsupported or unavailable host workflow.
