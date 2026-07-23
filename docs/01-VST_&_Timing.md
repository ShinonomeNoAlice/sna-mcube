# Dev Log: The 11 Commits of VST Host & WASAPI Timing Hell

> **Scope**: `plugins/wasapiexclusiveout` (WASAPI Exclusive Mode Output & VST3 Host)  
> **Target Commit Range**: `cd71280` .. `36e4679`

---

## Executive Summary & Background

When playing high-resolution audio (176.4 kHz / 192 kHz+) through `wasapiexclusiveout` with real-time VST3 audio visualizer plugins like **MiniMeter**, the visualizer animations (spectrum, LUFS meters, oscilloscope) experienced severe micro-stuttering, tearing, and intermittent audio crackling.

What started as a seemingly simple ask ("fix MiniMeter UI stuttering") unraveled into an 11-commit descent into real-time thread contention, buffer size mismatches, resampler pacing, event-driven DAC hardware synchronization, and OS clock drift vs. audio hardware timeline synthesis.

This document walks through those 11 commits chronologically — documenting the **Hypothesis**, **Implementation**, **Rationale**, and **Lessons Learned** from each iteration.

---

## 1. `cd71280` — UI Thread Safety & Audio Thread Heap Cleanups
*   **Commit Message**: `fix(wasapiexclusiveout): resolve VST3 audio host and visualizer stuttering`

### Hypothesis
The visualizer stutter is caused by:
1. Mutex contention on the audio thread when GUI timer threads inspect VST parameter changes.
2. High overhead from heap allocations (`new ParameterChangesHelper()`) inside `VstPlugin::Process()` at 192 kHz (where callbacks fire every ~1.3ms).
3. Under-allocated planar channel buffers causing dynamic vector reallocations during high sample rate streaming.

### Implementation
- Replaced blocking `std::lock_guard<std::mutex>` inside `WindowProc` (GUI thread) and `VstPlugin::Process` with non-blocking `std::unique_lock<std::mutex>(..., std::defer_lock)` using `try_lock()`.
- Changed dynamic allocation of parameter change helpers: only allocate if `pendingParamChanges` is non-empty.
- Upgraded the Win32 `WM_TIMER` update frequency from 30ms (~33 FPS) to 15ms (~66 FPS) for smooth visual updates.
- Pre-allocated planar channel vectors with a minimum capacity of 16,384 samples.
- Synthesized valid VST `ProcessContext` fields (`projectTimeMusic` and `barPositionMusic`).

### Rationale & Result
Reduced audio thread lock contention, but MiniMeter still stuttered periodically under high sample rates. The underlying timing jitter remained unresolved.

---

## 2. `b607a26` — Configurable Fixed-Block VST Chunking
*   **Commit Message**: `feat(wasapiexclusiveout): add configurable fixed-block chunking for VST host processing`

### Hypothesis
WASAPI Exclusive mode passes arbitrary buffer frame sizes depending on player upsampling and audio output parameters (e.g., 441, 1056, or 1337 frames). Many VSTs (including MiniMeter's FFT windowing) expect fixed power-of-two block sizes (512, 1024, 2048) and perform poorly or stutter when handed variable block sizes.

### Implementation
- Added a configuration preference `PREF_VST_BLOCK_SIZE` (`vst_block_size`) in settings with choices: `"512"`, `"1024"`, `"2048"`, `"4096"`, and `"Passthrough"`.
- Updated `VstChain::Process` to slice larger incoming audio buffers into chunks of `targetBlockSize` before feeding `VstPlugin::Process`.

### Rationale & Result
Allowed VSTs to operate on predictable block boundaries, but introduced sub-buffer timing misalignments when feeding WASAPI directly without pacing.

---

## 3. `d686e45` — MMCSS Real-Time Priority & Event-Driven WASAPI
*   **Commit Message**: `fix(wasapiexclusiveout): add MMCSS Pro Audio thread priority and event-driven WASAPI pacing`

### Hypothesis
Thread scheduling jitter in standard Windows user threads causes the playback loop to awaken late, starving both WASAPI's hardware buffer and the VST processing cycle.

### Implementation
- Linked `avrt.lib` and called `AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex)` during audio client initialization to boost thread priority to real-time MMCSS.
- Switched WASAPI initialization to Event-Driven mode (`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`).
- Added kernel event creation (`CreateEvent`) and attached it via `IAudioClient::SetEventHandle`.

### Rationale & Result
Significantly improved audio stability against CPU spikes, but opened a new problem: how to cleanly pace VST execution when upsampling creates high sample counts per hardware DAC interrupt.

---

## 4. `3cb1d38` — Introducing FIFO Pacing
*   **Commit Message**: `fix(wasapiexclusiveout): add resample FIFO pacing for post-resampled VST delivery`

### Hypothesis
When upsampling (e.g. 44.1 kHz $\rightarrow$ 352.8 kHz), the resampler produces large bursts of samples. Feeding these directly to the VST and WASAPI creates buffer spikes. Intermediate FIFO buffers (`resampleFifo` and `vstBlockBuffer`) will normalize delivery into uniform VST block sizes.

### Implementation
- Added `resampleFifo` vector to `WasapiExclusiveOut`.
- Resampled incoming audio directly into `resampleFifo`.
- Populated `vstBlockBuffer` with exactly 1 block from `resampleFifo` per WASAPI `Play()` iteration.

### Rationale & Result
**Failure Mode**: The FIFO grew monotonically when the input sample rate didn't perfectly match output consumption, eventually causing memory leaks, latency accumulation, and audio crashes.

---

## 5. `9bf08fa` — Throttling the Resampler FIFO
*   **Commit Message**: `fix(wasapiexclusiveout): throttle resampleFifo to prevent buffer accumulation, crash, and audio stuttering`

### Hypothesis
The FIFO overflowed because the resampler was fed indiscriminately. Throttling input processing based on remaining WASAPI hardware buffer padding will prevent accumulation.

### Implementation
- Added checks against `GetCurrentPadding()` before invoking `soxr_process`.
- Calculated required `sleepMs` if `resampleFifo` held more than 2 target blocks or hardware padding was insufficient.

### Rationale & Result
Prevented unbounded memory growth, but introduced buffer starvation: the playback engine misunderstood the return codes and halted stream delivery prematurely.

---

## 6. `dba2110` — Flow Control & Consumption Signals
*   **Commit Message**: `fix(wasapiexclusiveout): return BufferWritten only when input buffer is consumed to prevent buffer leak and audio stall`

### Hypothesis
The engine's output manager assumes returning `OutputState::BufferWritten` means the input `IBuffer` has been completely processed. If returned early while holding unconsumed data in the FIFO, buffers leak and streams stall.

### Implementation
- Refactored `Play()` to track whether the incoming `IBuffer` was fully consumed by `soxr`.
- Returned `OutputState::BufferWritten` *only* after `provider->OnBufferProcessed(buffer)` was dispatched.

### Rationale & Result
Fixed engine stream halts, but exposed severe audio crackling due to partial hardware buffer fills.

---

## 7. `194d96f` — Chunked WASAPI Hardware Buffer Filling
*   **Commit Message**: `fix(wasapiexclusiveout): fill available WASAPI hardware buffer using chunked VST blocks to eliminate crackling`

### Hypothesis
Writing only one VST block per `Play()` call left WASAPI's hardware ring buffer under-filled, resulting in buffer underruns (crackling).

### Implementation
- Wrapped the VST block extraction and WASAPI `GetBuffer`/`ReleaseBuffer` calls in a `while` loop:
  ```cpp
  while (totalWrittenFrames < availableFrames && !this->resampleFifo.empty()) { ... }
  ```

### Rationale & Result
**System Breakdown**: The combination of FIFO buffering, resampler throttling, chunked hardware filling, and state checking created an over-engineered feedback loop. Audio cuts, buffer stalls, and latency drift plagued high sample rate playback.

---

## 8. `169e515` — The Great Revert (Back to Synchronous Pipeline)
*   **Commit Message**: `fix(wasapiexclusiveout): restore synchronous buffer processing to eliminate audio cuts and buffer stalls`

### Rationale & Turning Point
> *Recognizing when an architecture is fundamentally flawed.*

The asynchronous FIFO pipeline introduced in commits 4–7 created a complex state machine that fought WASAPI's native pull model. 

### Implementation
- **Nuked** `resampleFifo` and `vstBlockBuffer`.
- Restored direct synchronous flow:  
  $$\text{Input Buffer} \longrightarrow \text{Resampler} \longrightarrow \text{VST Block Chunking} \longrightarrow \text{WASAPI Render Buffer}$$
- Retained fixed-block chunking *inside* `VstChain::Process()` rather than in external FIFOs.

### Result
Audio playback instantly stabilized! No more audio cuts, stalls, or memory growth. But MiniMeter's visualizer *still* exhibited subtle micro-stuttering.

---

## 9. `e96bf8f` — Fixing VST `maxSamplesPerBlock` Re-allocation
*   **Commit Message**: `fix(vsthost): configure VST maxSamplesPerBlock to active target block size to fix MiniMeter 16400-sample allocation`

### Hypothesis
MiniMeter and other visualizer plugins were allocating internal buffers for 16,400 samples because `VstPlugin::SetSampleRateAndBlockSize` hardcoded a safety fallback buffer of `16384` samples into `ProcessSetup::maxSamplesPerBlock`.

### Implementation
- Removed the `16384` hardcoded clamp.
- Configured `setup.maxSamplesPerBlock = activeBlockSize` (matching the user preference, e.g., 1024).
- Added checks to prevent calling `setupProcessing()` when the sample rate and block size remained unchanged.

### Rationale & Result
Reduced MiniMeter's internal DSP overhead significantly, fixing memory footprint issues. However, visualizer animations still had jitter.

---

## 10. `def92db` — Deterministic Stream Timeline Synthesis
*   **Commit Message**: `fix(wasapiexclusiveout): pace VST execution to hardware DAC and synthesize deterministic stream timeline to prevent Catchup stutters`

### Root Cause Discovered!
Visualizer VSTs rely on `Vst::ProcessContext::systemTime` and `projectTimeSamples` to position visual elements (e.g. FFT windows, waveform rolls).

Previously, `systemTime` was set via `std::chrono::steady_clock::now()` on every audio callback:
- At 192 kHz with 256-sample blocks, callbacks fire every **1.33 ms**.
- Windows thread scheduling noise (±0.5 ms) introduced up to **35% timestamp jitter** relative to `totalSamplesProcessed`.
- MiniMeter interpreted this clock jitter as irregular playback speed, causing visual "catchup" stutters.

### Implementation
1. **Hardware Pacing**: Loop through VST block processing while blocking on the DAC hardware handle (`WaitForSingleObject(hAudioEvent)`).
2. **Deterministic Timeline Synthesis**: Replaced wall-clock timestamps with an idealized, sample-accurate clock:
   ```cpp
   // Calculate precise stream timestamp based strictly on sample count
   int64_t idealSystemTimeNs = streamStartSystemTime + 
       (int64_t)((double)totalSamplesProcessed * 1000000000.0 / currentSampleRate);
   context.systemTime = idealSystemTimeNs;
   ```

### Rationale & Result
Visualizers became butter-smooth! Micro-stuttering vanished completely under normal conditions. However, track switches or initial playback starts occasionally triggered rapid visual flickering.

---

## 11. `36e4679` — Loosening Pre-buffering Drift Thresholds
*   **Commit Message**: `fix(wasapiexclusiveout): loosen VST systemTime drift threshold to allow natural hardware pre-buffering`

### Hypothesis
When playback starts, WASAPI pre-buffers up to 2 seconds of audio into hardware memory ahead of real time. Because `idealSystemTimeNs` advanced faster than real OS time during pre-buffering, the drift detection guard:
```cpp
if (std::abs(osTimeNs - idealSystemTimeNs) > 50000000) { /* resync */ }
```
erroneously flagged the pre-buffer as "macro drift" and constantly reset the stream anchor (`streamStartSystemTime`).

### Implementation
Updated the resync threshold to asymmetric bounds:
```cpp
// Allow idealSystemTimeNs to be up to 2000ms AHEAD (natural hardware pre-buffering),
// but resync if it falls >50ms BEHIND (actual audio starvation).
if (osTimeNs - idealSystemTimeNs > 50000000 || idealSystemTimeNs - osTimeNs > 2000000000) {
    streamStartSystemTime = osTimeNs - (int64_t)((double)totalSamplesProcessed * 1000000000.0 / currentSampleRate);
    idealSystemTimeNs = osTimeNs;
}
```

### Rationale & Result
Allowed WASAPI's hardware ring-buffer to pre-fill naturally without triggering fake clock resyncs. Visualizers remained silky-smooth from the exact millisecond playback starts.

---

## Summary of Key Architectural Insights

| Component | Initial State | Final State |
| :--- | :--- | :--- |
| **Thread Priority** | Normal user thread | MMCSS Real-Time (`"Pro Audio"`) |
| **WASAPI Wait** | Polled (`Sleep(1)`) | Hardware DAC Interrupt (`hAudioEvent`) |
| **Audio Pipeline** | Complex FIFO Resample Queue | Direct Synchronous Flow + In-Place Chunking |
| **VST Block Size** | Unconstrained Variable Sizes | Configurable Fixed Power-of-Two (e.g. 1024) |
| **VST Clock (`systemTime`)** | Wall-clock (`steady_clock::now()`) | Monotonic Sample-Deterministic Timeline |
| **Clock Drift Guard** | Symmetric 50ms Window | Asymmetric Pre-buffering Window (-50ms / +2000ms) |

---

## Conclusion

The 11-commit arc represents a classic real-time systems debugging journey:
1. **Initial assumptions** blamed UI mutexes and dynamic allocations.
2. **Intermediate solutions** over-engineered an asynchronous FIFO model that fought hardware constraints.
3. **The breakthrough** came from stripping away unnecessary queues, returning to synchronous execution, and recognizing that VST visualizers need **deterministic hardware sample clocks**, not OS wall-clock timestamps.
