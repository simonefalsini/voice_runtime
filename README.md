# voice_runtime

Runtime audio conversazionale full-duplex in C++17, header-only.

## Caratteristiche

- Pipeline Microphone → AEC → STT → LLM → TTS → Output con un thread per nodo
- VAD (Voice Activity Detection) come nodo esplicito con gating del flusso audio
- Adattamento automatico del formato audio (sample rate, canali, bit depth)
- Barge-in: interruzione TTS/LLM da tastiera o da analisi del contesto testuale
- Code bounded con tre politiche di overflow (BlockProducer, DropNewest, DropOldest)
- Pool di buffer preallocati senza allocazioni nel path real-time
- Metriche periodiche su stdout con tabella per coda
- State machine esplicita per il ciclo di vita del runtime
- Shutdown deterministico senza deadlock

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/voice_runtime_simulation
```

Dipendenze: C++17, pthreads (Unix). Nessuna dipendenza esterna.

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
    BoundedQueue.h        coda bounded con DropCallback e snapshot metriche
    SharedBufferPool.h    pool preallocato thread-safe con timeout
    TextBuffer.h          RollingTextBuffer, DiskBackedTextBuffer
    Interfaces.h          interfacce di tutti i nodi
    RuntimeConfig.h       configurazione del runtime
    AudioFormatAdapter.h  conversione formato audio (pass-through se non necessario)
    MetricsReporter.h     reporter periodico su stdout
    VoiceRuntime.h        orchestratore principale

tests/
    SimulatedNodes.h      nodi simulati per test senza dipendenze esterne
    simulation_main.cpp   simulazione completa con VAD, adapter, barge-in
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
- **Step 2**: integrazione WebRTC APM, RNNoise, Kokoro TTS, Qwen3-ASR, LLM OpenAI-compatible
- **Step 3**: test con file WAV, benchmark latenza end-to-end

Vedi `docs/ARCHITECTURE.md` per la documentazione tecnica completa.
