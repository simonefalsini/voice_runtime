# voice_runtime

Runtime audio conversazionale full-duplex in C++20, header-only.

## Caratteristiche

- Pipeline Microphone → AEC → STT → LLM → TTS → Output con un thread per nodo
- VAD (Voice Activity Detection) come nodo esplicito con gating del flusso audio
- AEC3 (WebRTC) con drain doppio della render queue e warm-up gate 200ms
- Adattamento automatico del formato audio (sample rate, canali, bit depth)
- Barge-in: interruzione TTS/LLM da tastiera o da analisi del contesto testuale
- Code bounded con tre politiche di overflow (BlockProducer, DropNewest, DropOldest)
- Contatori di coda (`produced`/`consumed`/`dropped`) come `std::atomic<uint64_t>` — lettura senza mutex
- Pool di buffer preallocati senza allocazioni nel path real-time
- Nodi audio nativi Windows (WASAPI): `WasapiMicNode`, `WasapiOutputNode`
- Metriche periodiche su stdout con tabella per coda
- State machine esplicita per il ciclo di vita del runtime
- Shutdown deterministico senza deadlock

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/voice_runtime_simulation
```

Dipendenze: C++20, pthreads (Unix). Nessuna dipendenza esterna nella build base.

### Build con WebRTC APM + nodi WASAPI (Windows)

```powershell
cmake -B build-real -S . `
  -DCMAKE_BUILD_TYPE=Release `
  -DVOICE_RUNTIME_ENABLE_WEBRTC_APM=ON `
  -DVOICE_RUNTIME_WEBRTC_ROOT=C:/path/to/webrtc/src `
  -DVOICE_RUNTIME_WEBRTC_LIB=C:/path/to/webrtc.lib `
  -DVOICE_RUNTIME_ENABLE_WASAPI=ON
cmake --build build-real -j4
```

### Test nodi WASAPI (solo Windows)

```powershell
.\build-real\test_wasapi_mic.exe      # cattura 5s, verifica >= 450 frame
.\build-real\test_wasapi_output.exe   # riproduce tono 440Hz, verifica dropped == 0
```

## Controlli interattivi durante la simulazione

| Tasto | Azione |
|---|---|
| `b` + Enter | barge-in manuale (interrompe TTS e LLM) |
| Ctrl+C | termina il processo |

## Struttura

```
include/voice_runtime/
    AudioTypes.h          tipi fondamentali (AudioFrame, VadEvent, InterruptSignal, ...)
    ActiveNode.h          base class con thread naming e priorita' real-time
    BoundedQueue.h        coda bounded, contatori atomici, DropCallback senza re-lock
    SharedBufferPool.h    pool preallocato thread-safe con timeout
    TextBuffer.h          RollingTextBuffer, DiskBackedTextBuffer
    Interfaces.h          interfacce di tutti i nodi
    RuntimeConfig.h       configurazione del runtime
    AudioFormatAdapter.h  conversione formato audio (pass-through se non necessario)
    MetricsReporter.h     reporter periodico su stdout
    VoiceRuntime.h        orchestratore principale

nodes/
    WebRtcDspNode.h       nodo AEC3+NS+VAD basato su WebRTC APM (double drain, zero-alloc RT)
    KokoroTtsNode.h       TTS Kokoro con downsampler 24->16kHz integrato per AEC reference
    WasapiMicNode.h       cattura audio Windows nativa (WASAPI, event-driven)   [WIN32 only]
    WasapiOutputNode.h    riproduzione audio Windows nativa (WASAPI, event-driven) [WIN32 only]

tests/
    SimulatedNodes.h           nodi simulati per test senza dipendenze esterne
    simulation_main.cpp        simulazione completa con VAD, adapter, barge-in
    test_aec_transcription.cpp test full-duplex AEC + STT (aecRefQueue capacita' 32 frame)
    test_wasapi_mic.cpp        test cattura mic WASAPI (5s, verifica >= 450 frame)
    test_wasapi_output.cpp     test riproduzione WASAPI (440Hz, verifica dropped == 0)
```

## Pipeline

```
Mic (48kHz stereo)
    -> MicAdapter (48kHz stereo -> 16kHz mono)
    -> VAD (gating silence/speech)
    -> AEC (cancellazione eco)
    -> STT (audio -> testo)
    -> BargeIn (analisi contesto, interrupt)
    -> LLM (generazione risposta)
    -> TTS (testo -> audio)
    -> AudioOutput
         |
         +-> AEC reference
```

Gli adapter vengono istanziati automaticamente solo se i formati differiscono.

## Roadmap

- **Step 1** (completato): simulatore puro, architettura corretta, tutti i nodi
- **Step 2** (completato): WebRTC APM (AEC3+NS+VAD), Kokoro TTS, Qwen3-ASR, nodi WASAPI Windows, fix 7 bug AEC critici
- **Step 3**: InterpreterNode semantico (attualmente pass-through), resampling professionale (`r8brain`), benchmark latenza end-to-end

Vedi `docs/ARCHITECTURE.md` per la documentazione tecnica completa.
