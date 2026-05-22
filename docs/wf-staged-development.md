# wf:: weld staged development

This is a new ChucK-first instrument/app built from scratch with the same broad concepts as the earlier `of::` work:

- orbiting states
- per-state tracks, with lanes inside each track
- generated live synthesis
- state transitions as musical structure
- project/session data later
- embedded audio, with no SuperCollider process or bridge

It is not a direct port. The old project is useful as a memory of the interaction model, but the implementation, starter material, lane programs, UI code, and audio path are new.

## Stage 1: live orbit sketch

Current scope:

- A new JUCE GUI app target: `wf`
- Fresh orbit/state canvas
- One starter state named `State 1`
- Five tracks per state, with five generated ChucK lanes per track
- Basic transport: play/stop, previous, next, pick
- Live controls for gain, rate, density, and colour
- Embedded ChucK rendering through `WeldChucK::Engine`
- Program-generation smoke test for every starter state

This stage proves that the app shell can drive generated ChucK programs through the embedded engine without SuperCollider.

## Stage 2: editable machine

Next scope:

- Add/remove/rename states
- Add/remove/rename lanes
- Edit lane roles and synthesis seeds
- Manual transition graph
- Stable project model independent of the UI
- Save/load a new `.wfweld` project format

## Stage 3: stronger musical timing

Next scope:

- Replace simple state reloads with a persistent ChucK performance program
- Host-controlled lane gates and state transitions
- Per-lane fades and mutes
- Real transition scheduling from the app model
- More expressive lane generators

## Stage 4: rendering and export

Next scope:

- Offline bounce path using the embedded engine
- Render selected lane/state/whole machine
- Waveform previews
- Deterministic export tests
