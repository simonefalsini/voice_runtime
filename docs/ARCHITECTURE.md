# Voice Runtime — Architettura

## Obiettivo

Runtime audio conversazionale full-duplex in C++20, header-only, progettato per:

- massimizzare il parallelismo con un thread per nodo
- evitare allocazioni dinamiche nel path real-time
- supportare la sostituzione trasparente di ogni backend (STT, LLM, TTS, DSP)
- essere simulabile e profilabile in modo deterministico
- funzionare su Linux, macOS, Windows, Android, iOS e architetture custom (DGX Spark, ARM)

---

## Pipeline lineare unica

```
MicrophoneNode
    -> micRawQueue
    -> [AudioFormatAdapterNode: MicAdapter]  (solo se micRawFormat != pipelineFormat)
    -> micQueue
    -> VADNode (attivo in ascolto / pass-through durante TTS)
    -> vadGatedQueue
    -> AecDspNode (pass-through in ascolto / attivo durante TTS)
    -> cleanAudioQueue
    -> [AudioFormatAdapterNode: SttAdapter]  (solo se pipelineFormat != sttInputFormat)
    -> sttAdaptedQueue
    -> SpeechToTextNode (sempre attivo)
    -> sttTextQueue
    -> ClassifierBargeInNode (pass-through in ascolto / attivo durante TTS)
    -> classifierOutQueue
    -> LanguageModelNode (OpenAI-compatible API)
    -> llmOutputQueue
    -> InterpreterNode (decodifica output LLM, estrae testo leggibile)
    -> ttsInputQueue
    -> TextToSpeechNode (gestisce TtsStateSignal)
    -> speakerQueue  -> AudioOutputNode
    -> ttsReferenceQueue -> AecDspNode
```

### Modalità operative dei nodi

I nodi commutano dinamicamente tra **attivo** e **pass-through** in base
a `TtsStateSignal`, un `std::atomic<bool>` condiviso gestito dal nodo TTS:

| Nodo | TTS inattivo (ascolto) | TTS attivo (risposta) |
|------|------------------------|----------------------|
| **VAD** | ✅ Attivo — gata il silenzio | ⏩ Pass-through |
| **AEC** | ⏩ Pass-through | ✅ Attivo — AEC+NS, warm-up gate |
| **STT** | ✅ Attivo | ✅ Attivo (processa anche silenzio) |
| **Classifier** | ⏩ Pass-through — forwarda testo | ✅ Attivo — classifica, barge-in |
| **LLM** | ✅ Attivo | ✅ Attivo |
| **Interpreter** | ✅ Attivo | ✅ Attivo |
| **TTS** | 🔄 In attesa | ✅ Attivo — genera audio |

---

## Principi fondamentali

- Le callback di input acquisiscono un buffer dal pool e lo inseriscono nella coda.
- La callback di output (`MiniaudioOutputNode`) consuma direttamente dalla `speakerQueue` in modalità callback-driven per prevenire l'inversione di priorità e la starvation del thread indotta dai picchi CPU di sintesi del TTS (ONNX Runtime).
- Ogni nodo possiede un thread dedicato, code bounded, e non accede allo stato
  interno degli altri nodi.
- Nessuna allocazione dinamica nel path real-time: i buffer audio sono preallocati
  all'avvio tramite `SharedBufferPool<AudioFrame>`.
- Le code bounded introducono back-pressure naturale: il sistema converge verso
  una configurazione stabile senza buffer infiniti.

---

## File header

```
include/voice_runtime/
    AudioTypes.h          tipi base, AudioFrame, VadEvent, BargeInEvent, InterruptSignal, TtsStateSignal
    ActiveNode.h          IActiveNode, ActiveNodeBase (thread naming, priorita' RT)
    BoundedQueue.h        BoundedQueue<T> con DropCallback, QueueSnapshot, tryPop()
    SharedBufferPool.h    pool preallocato con acquireWithTimeout()
    TextBuffer.h          RollingTextBuffer (rolling/unbounded), DiskBackedTextBuffer
    Interfaces.h          tutte le interfacce dei nodi
    RuntimeConfig.h       configurazione completa del runtime
    AudioFormatAdapter.h  conversione formato/samplerate/canali (buffer preallocati)
    MetricsReporter.h     thread dedicato per metriche periodiche su stdout
    VoiceRuntime.h        orchestratore principale, state machine, wiring

nodes/
    WebRtcDspNode.h       nodo AEC basato su WebRTC APM (con pass-through stub)

tests/
    SimulatedNodes.h      implementazioni simulate di tutti i nodi
    simulation_main.cpp   simulazione pipeline completa
    simulation_main_webrtc_dsp.cpp   simulazione con WebRTC DSP
```

---

## Nodi attivi

Ogni nodo implementa `IActiveNode`:

```cpp
bool        initialize();
void        start();
void        stop();
const char* name() const;
```

`ActiveNodeBase` fornisce l'implementazione di `start()`/`stop()` con:

- naming del thread per piattaforma (`pthread_setname_np` su Linux/macOS/Android,
  `SetThreadDescription` su Windows)
- priorita' real-time opzionale tramite `startWithPriority(int)`:
  - Linux: `SCHED_FIFO`
  - Windows: `THREAD_PRIORITY_TIME_CRITICAL`
  - Android/iOS: ignorato (richiederebbe entitlement o root)

### Interfacce disponibili

| Interfaccia | Descrizione |
|---|---|
| `IMicrophoneNode` | cattura audio raw dal dispositivo |
| `IAudioFormatAdapterNode` | conversione formato, sample rate, canali |
| `IVadNode` | rilevamento attivita' vocale, emette `VadEvent`, pass-through durante TTS |
| `IAecDspNode` | cancellazione eco, DSP, pass-through in ascolto |
| `ISpeechToTextNode` | trascrizione audio → testo |
| `IClassifierBargeInNode` | classificazione intent + barge-in durante TTS, pass-through in ascolto |
| `ILanguageModelNode` | generazione testo, API OpenAI-compatible |
| `IInterpreterNode` | decodifica output LLM, estrae testo leggibile per TTS |
| `ITextToSpeechNode` | sintesi vocale testo → audio, gestisce `TtsStateSignal` |
| `IAudioOutputNode` | riproduzione audio sul dispositivo |

---

## TtsStateSignal

Segnale atomico condiviso (`std::atomic<bool>`) che governa il comportamento
dei nodi nella pipeline. Gestito dal nodo TTS:

- **setActive(true)**: chiamato quando il TTS inizia a generare audio
- **setActive(false)**: chiamato quando il TTS termina o viene interrotto

Consumato da: VAD, AEC, Classifier/BargeIn.

```cpp
struct TtsStateSignal {
    std::atomic<bool> active{false};
    void setActive(bool v) noexcept;
    bool isActive() const noexcept;
};
```

---

## ClassifierBargeInNode

Sostituisce il precedente `BargeInContextNode`. Comportamento dipendente dal TTS:

### TTS inattivo (ascolto)
- Pass-through: forwarda tutto il testo da STT a LLM
- Nessuna classificazione, nessun barge-in
- Thread tastiera ignora input

### TTS attivo (risposta)
- Classifica il testo: keywords "stop"/"fermati"/etc. → barge-in
- Commenti dell'utente → `overlapBuffer` (RollingTextBuffer configurabile)
- Non forwarda a LLM durante il TTS
- Tasto 'b' attiva barge-in manuale
- Alla transizione TTS ON→OFF: flusha overlapBuffer verso LLM

Il classificatore e' sostituibile tramite l'interfaccia `IClassifierBargeInNode`.

### Overlap Buffer

Buffer per commenti utente durante il TTS. Due modalita':

- **Rolling** (default): dimensione massima 32KB, testo piu' vecchio sovrascitto
- **Unbounded**: mantiene tutto il testo (`overlapBufferUnbounded = true`)

---

## InterpreterNode

Nodo tra LLM e TTS che decodifica l'output del modello ed estrae solo il testo
da leggere ad alta voce. Filtra metadata, comandi e istruzioni interne.

Nella simulazione opera in pure pass-through. In produzione implementera' il
parsing del formato di output dell'LLM.

---

## AEC Warm-up

Quando il TTS si attiva, l'AEC necessita di ~100-200ms per costruire il modello
dell'eco. Durante questo periodo il nodo **non emette frame** verso l'STT,
garantendo che l'eco residuo non venga mai trascritto.

Configurabile via `aecWarmupGracePeriodMs` (default 200ms).

---

## AudioFormatAdapterNode

Converte automaticamente tra formati audio diversi. Operazioni supportate:

- resampling lineare (sufficiente per test; da sostituire con `r8brain` in produzione)
- downmix canali (stereo → mono)
- conversione formato campione (Int16 ↔ Float32)
- adattamento frame size

I buffer di conversione sono **preallocati** come membri della classe per evitare
allocazioni dinamiche nel path real-time.

Se `sourceFormat == targetFormat` il nodo entra in modalita' **pass-through zero-copy**.

---

## BoundedQueue

`BoundedQueue<T>` supporta tre politiche di overflow:

| Politica | Comportamento |
|---|---|
| `BlockProducer` | il producer si blocca fino a spazio disponibile |
| `DropNewest` | l'item in arrivo viene scartato |
| `DropOldest` | l'item piu' vecchio in coda viene scartato |

Metodi principali:

- `push(T&&)` — inserisce con politica di overflow
- `pop(T&)` — bloccante fino a disponibilita'
- `tryPop(T&)` — non bloccante, ritorna false se vuoto
- `stop()` — sblocca tutti i thread in attesa
- `clear()` — svuota la coda
- `snapshot()` — snapshot atomico delle metriche

`tryPop()` e' fondamentale per il drain non-bloccante della render queue nell'AEC.

---

## SharedBufferPool

Pool di buffer preallocati con `std::shared_ptr` e custom deleter che cattura
`std::shared_ptr<State>` invece di `this`. Lo `State` rimane vivo fino alla
distruzione dell'ultimo handle pendente.

`acquireWithTimeout()` incrementa `stats.timeouts` (non `stats.dropped`) in caso
di timeout, separando i timeout del pool dai veri drop delle code.

---

## TextBuffer

### RollingTextBuffer

Finestra in memoria con due modalita':
- **Rolling** (default): dimensione fissa `maxBytes`, sovrascrive dall'inizio
- **Unbounded**: `maxBytes = SIZE_MAX`, nessun testo viene mai cancellato

Thread-safe. Usato sia per il memory window dell'LLM che per l'overlap buffer.

### DiskBackedTextBuffer

File aperto nel costruttore, chiuso nel distruttore. Flush periodico configurabile.
Mantiene in RAM solo una finestra recente.

---

## RuntimeState — macchina a stati

```
Idle --[initialize+start]--> Running --[stop]--> Stopping --> Stopped
```

---

## Sequenza di shutdown

```
1. MetricsReporter::stop()
2. stop() su tutte le code          <- sblocca producer e consumer bloccati
3. stop() sui nodi (dalla sorgente al fondo della pipeline)
4. clear() su tutte le code         <- rilascia AudioFrameHandle prima dei pool
5. audioPool_.stop()
6. llmDiskBuffer_.flush()
7. RuntimeState -> Stopped
```

---

## Code nella pipeline

| Coda | Tipo | Da → A |
|------|------|--------|
| `micRawQueue` | Audio | Mic → MicAdapter |
| `micQueue` | Audio | MicAdapter → VAD |
| `vadGatedQueue` | Audio | VAD → AEC |
| `cleanQueue` | Audio | AEC → STT |
| `sttAdaptedQueue` | Audio | SttAdapter → STT |
| `sttTextQueue` | Testo | STT → Classifier |
| `classifierOutQueue` | Testo | Classifier → LLM |
| `llmOutputQueue` | Testo | LLM → Interpreter |
| `ttsInputQueue` | Testo | Interpreter → TTS |
| `speakerQueue` | Audio | TTS → AudioOutput |
| `ttsRefQueue` | Audio | TTS → AEC (reference) |
| `vadEventQueue` | Events | VAD → esterno |
| `bargeInQueue` | Events | Classifier → esterno |

---

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/voice_runtime_simulation
./build/voice_runtime_webrtc_simulation
```

Dipendenze: C++20, pthreads (Unix). Nessuna dipendenza esterna nella build base.

### Build con WebRTC APM

```bash
cmake -S . -B build-webrtc \
  -DVOICE_RUNTIME_ENABLE_WEBRTC_APM=ON \
  -DVOICE_RUNTIME_WEBRTC_ROOT=/path/to/webrtc/src \
  -DVOICE_RUNTIME_WEBRTC_LIB=/path/to/libwebrtc.a
cmake --build build-webrtc -j
```
