# Codex instructions - Continue WebRTC DSP integration

## Project context

This repository implements a portable C++ voice runtime for a full-duplex conversational assistant.
The runtime is organized as independent active nodes connected through bounded queues and preallocated shared buffer pools.
The main goal of the current task is to complete and validate the real WebRTC DSP node that removes TTS echo and background noise before audio reaches STT.

The current runtime already contains:

- `ActiveNodeBase`: common active-thread node base.
- `AudioTypes.h`: `AudioFormat`, `AudioFrame`, `AudioFrameHandle`, `VadEvent`, runtime stats.
- `BoundedQueue.h`: bounded queue with overflow policy.
- `SharedBufferPool.h`: safe shared buffer pool based on shared internal state.
- `Interfaces.h`: node interfaces.
- `VoiceRuntime.h`: pipeline wiring, lifecycle, queues, metrics, stop/clear order.
- `SimulatedNodes.h`: simulated microphone, AEC, VAD, STT, LLM, TTS, audio output.
- `simulation_main.cpp`: simulation entry point.
- `WebRtcDspNode.h`: preliminary WebRTC DSP node implementation.

The target DSP node must be usable as both:

```cpp
class WebRtcDspNode final
    : public ActiveNodeBase
    , public IAecDspNode
    , public IVadNode
```

It should therefore replace the previous combination of `SimulatedAecNode` plus `SimulatedVadNode` when WebRTC integrated DSP/VAD is enabled.

## Important Codex behavior

Codex should inspect the actual repository and local WebRTC headers before changing code.
Do not assume that WebRTC APIs are identical across branches or package variants.
If a local `AGENTS.md` exists, follow it first. Otherwise, this file is the main instruction file.

OpenAI Codex reads project instructions from `AGENTS.md`. If useful, copy this file to `AGENTS.md` at the repository root or merge it into the existing one.

## Design goals

The WebRTC DSP node must:

1. Run in its own thread using `ActiveNodeBase`.
2. Consume microphone/capture frames from `AudioFrameQueue`.
3. Consume TTS/render-reference frames from `AudioFrameQueue` without blocking the capture path.
4. Feed render frames into WebRTC APM reverse stream.
5. Feed capture frames into WebRTC APM capture stream.
6. Produce cleaned audio frames into the output queue.
7. Optionally emit VAD events into `VadEventQueue`.
8. Optionally gate audio output when acting as `IVadNode`.
9. Avoid dynamic allocation inside the real-time processing loop wherever practical.
10. Preserve safe shutdown behavior: no deadlocks, no `shared_ptr` deleter calling an invalid pool, no blocking destructor.
11. Remain portable across Windows, macOS, Linux, Android, and iOS.
12. Build in two modes:
    - without WebRTC: pass-through/stub mode for simulation and CI;
    - with WebRTC: real APM-based DSP.

## Current architecture expectations

The runtime wiring should support these configurations.

### Configuration A - external VAD node

```text
Mic -> optional adapter -> external VAD -> WebRtcDspNode/AEC -> STT
TTS -> reference queue -> WebRtcDspNode/AEC
```

### Configuration B - integrated DSP/VAD node, preferred

```text
Mic -> optional adapter -> WebRtcDspNode/AEC/NS/AGC/VAD -> STT
TTS -> reference queue -> WebRtcDspNode/AEC/NS/AGC/VAD
```

For Configuration B, disable the separate `SimulatedVadNode` or avoid wiring it before the DSP node.
The best VAD signal is obtained after echo cancellation and noise suppression, not before.

## Interfaces to review and possibly adjust

Review `Interfaces.h`.

`IAecDspNode` currently exposes:

```cpp
virtual void setCaptureInputQueue(AudioFrameQueue* micIn) = 0;
virtual void setRenderInputQueue(AudioFrameQueue* ttsRefIn) = 0;
virtual void setOutputQueue(AudioFrameQueue* cleanOut) = 0;
```

It is acceptable to extend it with optional DSP/VAD methods, for example:

```cpp
virtual void setVadEventQueue(VadEventQueue* events) {}
virtual void setSpeechThreshold(float threshold) {}
virtual bool isSpeaking() const { return false; }
```

Alternatively, keep `IAecDspNode` unchanged and implement the VAD-related behavior through `IVadNode` only.
Prefer the least invasive change that keeps `VoiceRuntime` clean.

If `WebRtcDspNode` implements both `IAecDspNode` and `IVadNode`, `VoiceRuntime` should detect and wire it safely, for example by `dynamic_cast<IVadNode*>` only when integrated VAD is explicitly enabled.

## BoundedQueue change required

The DSP thread must drain render/reference frames without blocking.
If not already present, add:

```cpp
bool tryPop(T& out);
```

to `BoundedQueue<T>`.

Requirements for `tryPop`:

- return `false` immediately if the queue is empty;
- return `false` if stopped and empty;
- move the oldest item into `out` when available;
- increment consumed stats;
- notify `notFull_` after popping;
- do not invoke callbacks while holding the mutex.

## DSP processing loop

Use one DSP processing thread only.
Do not call WebRTC APM concurrently from microphone and TTS threads.
The runtime threads should only push queue items.

Recommended loop:

```cpp
while (running()) {
    drainRenderQueue();

    AudioFrameHandle capture;
    if (!captureIn_ || !captureIn_->pop(capture)) break;

    drainRenderQueue();

    auto clean = pool_.acquireWithTimeout(std::chrono::milliseconds(50));
    if (!clean) {
        if (!running()) break;
        continue;
    }

    processCapture(*capture, *clean);
    updateVadState(*clean);

    if (shouldForwardFrame()) {
        if (out_) out_->push(std::move(clean));
    }
}
```

`drainRenderQueue()` should process all currently available TTS reference frames:

```cpp
AudioFrameHandle ref;
while (renderIn_ && renderIn_->tryPop(ref)) {
    processRender(*ref);
}
```

Do not use `size() > 0` followed by blocking `pop()` for this path.
That pattern is racy and may block.

## WebRTC APM behavior

The WebRTC-backed implementation should enable:

- AEC3 / echo canceller.
- Noise suppression.
- High-pass filter.
- AGC2 when available.
- Optional AGC1 fallback only if AGC2 is unavailable.
- VAD when available, or a local VAD fallback.

Suggested config object:

```cpp
struct WebRtcDspConfig {
    AudioFormat format;
    std::size_t outputPoolSize = 256;

    bool enableEchoCancellation = true;
    bool enableNoiseSuppression = true;
    bool enableHighPassFilter = true;
    bool enableAgc1 = false;
    bool enableAgc2 = true;
    bool enableVad = true;
    bool gateOutputWhenSilent = false;

    float speechThreshold = 0.5f;
    int vadHangoverMs = 500;
    int vadMode = 2;
};
```

The runtime's default internal format is currently 16 kHz, mono, 10 ms, Int16.
WebRTC APM normally expects 10 ms frames.
Validate this at initialization:

```cpp
format.frameMs == 10
format.channels == 1 or supported channel count
sampleRate in {8000, 16000, 32000, 48000}
```

If the format is unsupported, fail initialization with a clear error path rather than silently producing bad audio.

## WebRTC API adaptation

Do not hardcode a single WebRTC API variant without checking the local headers.
Different builds expose slightly different APIs.

Codex should inspect the local WebRTC include tree and adapt one of these approaches:

1. Modern APM style using `webrtc::AudioProcessingBuilder().Create()` and `webrtc::AudioProcessing::Config`.
2. Stream processing through `ProcessStream` and `ProcessReverseStream` with `StreamConfig` and channel pointer arrays.
3. Older APM APIs if the local package requires them.

Keep all WebRTC includes behind:

```cpp
#if VOICE_RUNTIME_ENABLE_WEBRTC_APM
#endif
```

The project must still compile without WebRTC installed.

## AudioFrame conversion rules

`AudioFrame` may hold either `pcm16` or `pcmF32` depending on `AudioFormat::sampleFormat`.
For the first WebRTC integration pass, support Int16 mono as the required production path.
If Float32 support is added, keep conversion explicit and avoid heap allocation in the processing loop.

Before processing, ensure:

```cpp
frame.format.sampleFormat == SampleFormat::Int16
frame.pcm16.size() == frame.format.totalSamplesPerFrame()
```

Output frame should preserve:

```cpp
clean.format = capture.format;
clean.timestampNs = capture.timestampNs;
clean.sequence = localOutputSequence++;
clean.userData = capture.userData; // optional, if useful
```

Do not alias output audio vectors to input vectors unless pass-through mode is explicitly active.

## VAD implementation strategy

The node can implement VAD in three levels.

### Level 1 - fallback energy VAD

Always provide this fallback so the simulation can run without WebRTC VAD.
Compute RMS or mean absolute amplitude on the cleaned frame.
Apply hysteresis and hangover:

- `SpeechStart` when score >= threshold and previous state was silence.
- `SpeechEnd` only after silence has lasted `vadHangoverMs`.

This is not production-grade, but it keeps tests deterministic and portable.

### Level 2 - WebRTC standalone VAD

If the local WebRTC tree exposes standalone VAD, use it after AEC/NS.
Possible headers vary by distribution, so inspect the tree before coding.
Common candidates include `common_audio/vad` or audio-processing VAD internals.

### Level 3 - APM-derived metrics

If the local APM exposes voice probability, speech activity, echo likelihood, or similar metrics, map them into `VadEvent::confidence`.
Do not depend on unstable/private APIs unless guarded.

## VoiceRuntime changes

Review `VoiceRuntime.h`.

Current `VoiceRuntime` wires an optional external `vad_` before AEC.
Add one explicit mode to `RuntimeConfig`, for example:

```cpp
enum class VadPlacement {
    Disabled,
    BeforeDsp,
    IntegratedInDsp
};
```

or minimally:

```cpp
bool enableIntegratedDspVad = false;
```

Recommended behavior:

- If `enableVad == false`: no VAD events and no VAD gating.
- If `enableVad == true` and external `vad_` exists and integrated DSP VAD is false: current behavior remains unchanged.
- If integrated DSP VAD is true and `aec_` implements `IVadNode`: wire mic directly to `aec_`, not through `vad_`.
- Set the DSP VAD event queue and speech threshold.

Avoid ambiguous dual-VAD pipelines unless explicitly requested.

## CMake requirements

The project must support:

```bash
cmake -S . -B build
cmake --build build -j
```

without WebRTC.

Add optional WebRTC build switches:

```bash
cmake -S . -B build-webrtc \
  -DVOICE_RUNTIME_ENABLE_WEBRTC_APM=ON \
  -DVOICE_RUNTIME_WEBRTC_ROOT=/path/to/webrtc/src \
  -DVOICE_RUNTIME_WEBRTC_LIB=/path/to/libwebrtc.a
```

or adapt to the local WebRTC package layout.

CMake should define:

```cpp
VOICE_RUNTIME_ENABLE_WEBRTC_APM=1
```

only when headers and libraries are present.

If WebRTC is not found but the flag is ON, fail CMake configuration with a clear error.
If the flag is OFF, build the pass-through/stub implementation.

## Test requirements

Add or update example programs:

1. `simulation_main.cpp`
   - existing simulation with simulated AEC/VAD should continue to work.

2. `simulation_main_webrtc_dsp.cpp`
   - uses `WebRtcDspNode`.
   - runs without WebRTC in pass-through mode.
   - runs with WebRTC in real DSP mode when enabled.

3. Optional unit-style tests:
   - queue `tryPop()` behavior;
   - DSP stop while waiting on capture queue;
   - DSP stop while output pool is exhausted;
   - VAD start/end event generation;
   - TTS reference queue draining;
   - no deadlock on `return 0` from main.

Expected simulation behavior:

- program exits cleanly;
- no blocked destructor;
- all node threads joined;
- queues stopped and cleared;
- no pool lifetime issue;
- metrics print finite high-water marks;
- final stats are printed.

## Shutdown constraints

Preserve the existing shutdown philosophy:

1. Stop metrics.
2. Stop all queues to unblock producers/consumers.
3. Stop nodes.
4. Clear all queues to release `shared_ptr` handles.
5. Stop shared pools.
6. Flush disk buffers.

Never introduce a custom deleter that captures a raw pointer to a pool object.
Use `SharedBufferPool` as currently implemented, based on shared internal state.

## Performance constraints

Inside the DSP loop:

- no unbounded allocations;
- no blocking on render/reference queue;
- no logging per frame unless debug mode is enabled;
- no calls to `queue.size()` for synchronization;
- no callbacks while queue or pool mutexes are held;
- no multi-threaded access to WebRTC APM instance.

## Files likely to change

Start by opening these files:

```text
include/voice_runtime/Interfaces.h
include/voice_runtime/RuntimeConfig.h
include/voice_runtime/VoiceRuntime.h
include/voice_runtime/BoundedQueue.h
include/voice_runtime/WebRtcDspNode.h
examples/simulation_main.cpp
examples/simulation_main_webrtc_dsp.cpp
CMakeLists.txt
```

If the repository layout differs, locate the equivalent files.

## Deliverables

Codex should produce:

1. Working C++ source changes.
2. Updated `WebRtcDspNode.h` or split `.h/.cpp` implementation if the file becomes too large.
3. Updated CMake configuration.
4. Updated simulation example.
5. Updated documentation, preferably:

```text
docs/WEBRTC_DSP_NODE.md
```

6. A short final report listing:
   - files changed;
   - build commands executed;
   - test commands executed;
   - whether WebRTC real mode was compiled;
   - any missing local dependencies;
   - remaining TODOs.

## Acceptance criteria

The task is complete when:

- default build without WebRTC succeeds;
- WebRTC-enabled build succeeds in the user's environment;
- simulation exits cleanly after its configured duration;
- `WebRtcDspNode` can replace `SimulatedAecNode` in the runtime;
- integrated VAD can emit `VadEvent::SpeechStart` and `VadEvent::SpeechEnd`;
- optional gating does not break STT input when disabled;
- no deadlock occurs at process exit;
- no unbounded queue or buffer growth occurs under simulation load.

## Practical first steps for Codex

1. Build the current project exactly as-is.
2. Run the current simulation and confirm the baseline.
3. Add `tryPop()` to `BoundedQueue` if absent.
4. Review `WebRtcDspNode.h` and make the no-WebRTC path compile cleanly.
5. Add integrated VAD wiring to `VoiceRuntime` behind explicit config.
6. Build and run the pass-through WebRTC DSP simulation.
7. Inspect local WebRTC headers and implement real APM calls.
8. Build with `VOICE_RUNTIME_ENABLE_WEBRTC_APM=ON`.
9. Run the WebRTC DSP simulation.
10. Check shutdown with debugger or sanitizer if available.

## Notes for WebRTC integration

The ideal production ordering is:

```text
render/TTS frame -> WebRTC ProcessReverseStream
capture/mic frame -> WebRTC ProcessStream -> cleaned audio -> VAD -> STT
```

Do not put external VAD before AEC when the goal is barge-in while TTS is speaking.
The VAD should see echo-cancelled audio, otherwise it may detect the assistant's own TTS as user speech.

Use AGC2 carefully.
AGC should happen after echo cancellation and noise suppression, otherwise it can amplify residual echo and background noise.

## Do not do

- Do not integrate full WebRTC PeerConnection, RTP, video, or signaling.
- Do not add browser/network dependencies.
- Do not replace the runtime queue/pool architecture with callbacks.
- Do not allocate a new audio buffer for every frame using `new` or `make_shared` in the processing loop.
- Do not make the render/reference queue blocking in the DSP thread.
- Do not hide build failures by silently falling back to pass-through when WebRTC was explicitly requested.
