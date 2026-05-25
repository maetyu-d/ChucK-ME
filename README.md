# ChucK-ME

ChucK-ME by matd.space is an app for Mac (Apple silicon only) that enables the building of ephemeral musical systems made up of states, tracks, and lanes (and linear or probabilistic transitions between them) in the ChucK programming language, then transforms (renders) the generative playback into audio files in a DAW-like arrangement for audio editing, mixing (including processing with AU and/or VST3 plugins), and WAV export. The transformation is integrated and seamless - no external ChucK application or process is required.

## Weld ChucK

This is a lean JUCE audio host with ChucK embedded in-process.

Runtime shape:

```text
JUCE audio callback
  -> EmbeddedChucKEngine
  -> ChucK::run(input, output, frames)
  -> JUCE output buffer
```

There is no `chuck` command-line process, no RtAudio stream, and no external audio server. JUCE owns the audio device and advances the ChucK VM directly.

The embedded engine is built as a reusable library target:

```cmake
target_link_libraries(YourPluginOrApp PRIVATE WeldChucK::Engine)
```

Consumers include the public umbrella header:

```cpp
#include <WeldChucKEngine.h>
```

The console executable is now only a host/test harness around that library. The `weld_chuck_engine` static archive contains the embedded engine object and exposes its JUCE/ChucK link requirements through CMake, so a plugin or app shell can depend on `WeldChucK::Engine` without compiling or linking the console harness.

The repo also contains the first fresh `wf` app stage: a new orbit/lane GUI sketch that uses the embedded ChucK engine directly. It uses the broad concepts of the earlier `of::` work, but it is not a direct port and does not copy the old SuperCollider bridge, old UI source, old demo names, or old scripts. See `docs/wf-staged-development.md`.

## Build

```sh
cmake -S . -B build -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The executable will be at:

```sh
build/WeldChucK_artefacts/Release/WeldChucK
```

Run it from a terminal. It starts the default audio device, runs the embedded ChucK program inside the JUCE callback, and quits when you press return.

The Stage 1 GUI app is built at:

```sh
build/wf_artefacts/Release/ChucK-ME.app
```

Run the headless engine self-test with:

```sh
build/WeldChucK_artefacts/Release/WeldChucK --self-test
build/WeldChucK_artefacts/Release/WeldChucK --stress-test
build/WeldChucK_artefacts/Release/WeldChucK --callback-test
build/WeldChucK_artefacts/Release/WeldChucK --fuzz-test
build/WeldChucK_artefacts/Release/WeldChucK --program-test
build/WeldChucK_artefacts/Release/WeldChucK --parameter-test
build/WeldChucK_artefacts/Release/WeldChucK --async-program-test
build/WeldChucK_artefacts/Release/WeldChucK --boundary-test
build/WeldChucK_artefacts/Release/WeldChucK --concurrency-test
build/WeldChucKEngineBoundaryTest
build/WfProgramTest
/Users/user/.local/opt/cmake/CMake.app/Contents/bin/ctest --test-dir build --output-on-failure
```

Or run the build-system check target:

```sh
cmake --build build --target check
```

## Notes

The first prototype disables ChucK MIDI, HID, serial, shell, and on-the-fly network server support so the embed stays tight. The ChucK VM, compiler, scheduler, standard UGens, and audio synthesis run inside the JUCE app.

Control values are stored in a fixed-size parameter binding table on the JUCE side and copied directly into ChucK globals from the audio callback before each `ChucK::run()` call. Indexed parameter writes are lock-free and bounded; name-based writes are available for non-real-time callers. That avoids spawning messages, allocating global update requests, or taking a UI lock on the audio thread.

ChucK program loading is transactional. New program bodies are compiled into a separate candidate ChucK VM away from the audio callback path. Parameter bindings inject `global float` declarations into the candidate program, so loaded program bodies can use named host controls without declaring boilerplate. If the candidate compiles, starts, and binds its globals, the engine performs a short locked swap. If compilation or binding fails, the current VM keeps playing and the failure is counted.

Program loading can also be queued asynchronously. `loadProgramAsync()` validates the binding request, publishes only the latest pending program to a background loader thread, and returns immediately so UI/control callers do not wait for ChucK compilation. Rapid async requests are coalesced: an older pending request can be dropped before compilation if a newer one replaces it, while the currently playing VM stays untouched until a candidate commits successfully. `waitForAsyncProgramLoads()` is available for tests and controlled shutdown paths, not for the audio callback.

The default binding set preserves the original controls: `hostFreq`, `hostGain`, and `hostBlend`. Callers can replace that with any validated binding set up to the fixed maximum: names must be ChucK-safe identifiers, duplicates are rejected, ranges/defaults must be finite and consistent, and invalid binding transactions leave the last good VM and binding table in place.

The host also preallocates conservative scratch buffers, shares the same hard block-size limit as the embedded engine, clears host outputs before rendering, clears every host output pointer before rejecting unsupported layouts, checks null channel arrays and channel pointers, suppresses denormals in the callback, sanitises non-finite control/audio values, limits output samples to a safe range, and fails silent if the device ever asks for a block larger than the prepared safety size. The callback has a final exception-containment layer so unexpected host-side failures are counted and muted instead of escaping the audio callback.

Prepare-time exceptions are caught and reported as startup failure. Render-time exceptions from ChucK are contained by muting the engine instead of allowing an exception to escape the real-time callback.

The engine rejects unsupported block sizes and channel counts before allocation, validates its prepared buffer invariants before every render, tears down ChucK defensively during release, and contains render exceptions by muting the engine. It also exposes lock-free diagnostic counters for silent callbacks, oversized blocks, render exceptions, sanitised samples, sanitised controls, internal invariant failures, rendered blocks, rendered frames, program load successes, program load failures, queued async loads, completed async loads, and coalesced async drops; the console host prints the most important counters on exit.

The tests cover cold and released engines, repeated prepare/release cycles, short and wide buffers, null and mixed-null callback channels, oversized callbacks, rejected device sample rates and block sizes, malformed input samples, deterministic fuzzed buffer/control combinations, transactional good/bad ChucK program loads, async good/bad/coalesced program loads, custom/empty/invalid parameter binding transactions, indexed control writes racing with reloads, reload storms while rendering, maximum accepted block/channel boundaries, a separate engine-library consumer target, fresh `wf` generated state programs, diagnostic counter accuracy, callback exception containment staying dormant, and render calls racing against prepare/release/control updates.

ChucK is included under its dual MIT/GPL licensing. See `third_party/chuck/LICENSE.MIT` and `third_party/chuck/LICENSE.GPL`.
