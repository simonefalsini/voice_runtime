# Voice Runtime — Architettura

## Obiettivo

Runtime audio conversazionale full-duplex in C++17, header-only, progettato per:

- massimizzare il parallelismo con un thread per nodo
- evitare allocazioni dinamiche nel path real-time
- supportare la sostituzione trasparente di ogni backend (STT, LLM, TTS, DSP)
- essere simulabile e profilabile in modo deterministico
- funzionare su Linux, macOS, Windows, Android, iOS e architetture custom (DGX Spark, ARM)

---

## Pipeline logica

```
MicrophoneNode
    -> micRawQueue
    -> [AudioFormatAdapterNode: MicAdapter]  (solo se micRawFormat != pipelineFormat)
    -> micQueue
    -> VADNode (Silero VAD / SimulatedVadNode)
    -> vadGatedQueue
    -> AecDspNode (WebRTC APM / RNNoise)
    -> cleanAudioQueue
    -> [AudioFormatAdapterNode: SttAdapter]  (solo se pipelineFormat != sttInputFormat)
    -> sttAdaptedQueue
    -> SpeechToTextNode (Qwen3-ASR / Whisper)
    -> sttTextQueue
    -> BargeInContextNode
    -> llmTextQueue
    -> LanguageModelNode (OpenAI-compatible API)
    -> llmTextQueue
    -> TextToSpeechNode (Kokoro)
    -> speakerQueue  -> AudioOutputNode
    -> ttsReferenceQueue -> AecDspNode
```

Il nodo `AudioFormatAdapterNode` viene istanziato da `VoiceRuntime` automaticamente
se e solo se i formati differiscono. Se i formati coincidono il nodo non viene
creato e la coda intermedia non viene usata (zero overhead).

---

## Principi fondamentali

- Le callback dei dispositivi audio non eseguono lavoro pesante: acquisiscono un
  buffer dal pool e lo inseriscono nella coda. Il lavoro e' svolto dai nodi attivi.
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
    AudioTypes.h          tipi base, AudioFrame, VadEvent, BargeInEvent, InterruptSignal
    ActiveNode.h          IActiveNode, ActiveNodeBase (thread naming, priorita' RT)
    BoundedQueue.h        BoundedQueue<T> con DropCallback e QueueSnapshot
    SharedBufferPool.h    pool preallocato con acquireWithTimeout()
    TextBuffer.h          RollingTextBuffer, DiskBackedTextBuffer (handle persistente)
    Interfaces.h          tutte le interfacce dei nodi
    RuntimeConfig.h       configurazione completa del runtime
    AudioFormatAdapter.h  conversione formato/samplerate/canali
    MetricsReporter.h     thread dedicato per metriche periodiche su stdout
    VoiceRuntime.h        orchestratore principale, state machine, wiring

tests/
    SimulatedNodes.h      implementazioni simulate di tutti i nodi
    simulation_main.cpp   programma di test
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
| `IVadNode` | rilevamento attivita' vocale, emette `VadEvent` |
| `IAecDspNode` | cancellazione eco, DSP |
| `ISpeechToTextNode` | trascrizione audio → testo |
| `IBargeInContextNode` | analisi contesto per interrupt, tastiera |
| `ILanguageModelNode` | generazione testo, API OpenAI-compatible |
| `ITextToSpeechNode` | sintesi vocale testo → audio |
| `IAudioOutputNode` | riproduzione audio sul dispositivo |

---

## AudioFormatAdapterNode

Converte automaticamente tra formati audio diversi. Operazioni supportate:

- resampling lineare (sufficiente per test; da sostituire con `r8brain` in produzione)
- downmix canali (stereo → mono)
- conversione formato campione (Int16 ↔ Float32)
- adattamento frame size

Se `sourceFormat == targetFormat` il nodo entra in modalita' **pass-through zero-copy**:
l'`AudioFrameHandle` viene passato direttamente senza allocazioni ne' copie.

`VoiceRuntime` istanzia i due adapter (mic e STT) automaticamente in `initialize()`
confrontando i formati configurati. Il chiamante non deve occuparsene.

---

## VADNode

Silero VAD (implementazione reale) o `SimulatedVadNode` (test).

- Opera su finestre audio; emette `VadEvent::SpeechStart` e `VadEvent::SpeechEnd`
- Durante i periodi di silenzio i frame non vengono propagati a valle (gating)
- La soglia di confidenza e' configurabile via `setSpeechThreshold(float)`
- Gli eventi vengono accodati in `VadEventQueue` per l'osservazione esterna

Il nodo VAD viene iniettato in `VoiceRuntime` tramite `setVadNode()` separatamente
dagli altri nodi, per consentire la sostituzione con implementazioni basate su
GGML/ONNX senza modificare il costruttore del runtime.

---

## BargeInContextNode

Analizza il testo STT in transito verso l'LLM e decide se interrompere la
generazione/sintesi in corso.

Due sorgenti di interrupt:

1. **Keyboard** — tasto `b` rilevato da un thread di polling stdin dedicato
2. **ContextDetected** — parole chiave nel testo STT (configurabili)

Quando l'interrupt e' attivato:
- `InterruptSignal::request()` viene chiamato su `ttsInterrupt_` e `llmInterrupt_`
- Il nodo TTS controlla il segnale ad ogni frame generato e svuota il batch corrente
- Il nodo LLM controlla il segnale ogni 50ms durante la latenza simulata e
  annulla la request in corso
- L'evento viene accodato in `BargeInQueue` per l'osservazione esterna

`InterruptSignal` e' un wrapper su `std::atomic<bool>` con semantica
`memory_order_release`/`acquire`.

---

## BoundedQueue

`BoundedQueue<T>` supporta tre politiche di overflow:

| Politica | Comportamento |
|---|---|
| `BlockProducer` | il producer si blocca fino a spazio disponibile |
| `DropNewest` | l'item in arrivo viene scartato |
| `DropOldest` | l'item piu' vecchio in coda viene scartato |

Novita' rispetto alla versione precedente:

- **DropCallback**: funzione opzionale chiamata fuori dal lock con l'item scartato,
  per logging o metriche esterne
- **Drop fuori dal lock**: la distruzione dell'item scartato (e del relativo
  `shared_ptr`) avviene dopo il rilascio del mutex, eliminando il rischio di
  contention con `releaseToState()` del pool
- **`QueueSnapshot`**: snapshot atomico di tutte le metriche per il `MetricsReporter`
- Il nome della coda e' passato al costruttore e appare nelle metriche

Default per le code audio: `DropOldest` (frame piu' vecchi sacrificabili).
Default per le code testo: `DropOldest` (ma l'input LLM e' anche su disco).

---

## SharedBufferPool

Pool di buffer preallocati con `std::shared_ptr` e custom deleter che cattura
`std::shared_ptr<State>` invece di `this`. Lo `State` rimane vivo fino alla
distruzione dell'ultimo handle pendente, eliminando il problema del lifetime
durante lo shutdown.

Metodi disponibili:

```cpp
Handle acquireBlocking();                              // bloccante senza timeout
Handle acquireWithTimeout(chrono::milliseconds);       // bloccante con timeout
Handle tryAcquire();                                   // non bloccante
void   stop();
```

`acquireWithTimeout()` e' il metodo raccomandato nei nodi simulati: evita deadlock
silenziosi se il pool e' esaurito e il sistema e' in shutdown.

---

## TextBuffer

### RollingTextBuffer

Finestra in memoria di dimensione fissa. Quando supera `maxBytes` sovrascrive
dall'inizio (FIFO). Thread-safe.

### DiskBackedTextBuffer

- Il file viene aperto nel costruttore (`std::ios::trunc`) e chiuso nel distruttore.
  **Nessuna apertura/chiusura per ogni `append`** (fix rispetto alla versione
  precedente che eseguiva syscall `open`/`close` ad ogni scrittura).
- Flush esplicito ogni N scritture (configurabile via `setWritesBeforeFlush()`).
- Mantiene in RAM solo una finestra recente tramite `RollingTextBuffer` interno.
- `flush()` esplicito chiamato da `VoiceRuntime::stop()` prima della distruzione.

---

## RuntimeState — macchina a stati

```
Idle --[initialize+start]--> Running --[stop]--> Stopping --> Stopped
```

Implementata con `std::atomic<int>` e `compare_exchange_strong`. Le transizioni
non valide sono no-op. Questo sostituisce la doppia guardia `running_`/`stoppedOnce_`
della versione precedente che aveva una condizione logica invertita.

---

## Sequenza di shutdown

```
1. MetricsReporter::stop()
2. stop() su tutte le code          <- sblocca producer bloccati su push()
                                        e consumer bloccati su pop()
3. stop() sui nodi (in ordine inverso rispetto all'avvio)
4. clear() su tutte le code         <- rilascia AudioFrameHandle prima
                                        che i pool dei nodi vengano distrutti
5. audioPool_.stop()
6. llmDiskBuffer_.flush()
7. RuntimeState -> Stopped
```

I nodi simulati con pool interno implementano `wake()` chiamando `pool_.stop()`,
cosi' un thread bloccato in `acquireBlocking()` viene svegliato correttamente.

---

## MetricsReporter

Thread dedicato che stampa una tabella su stdout ogni N millisecondi
(configurabile in `RuntimeConfig::metricsIntervalMs`).

Output di esempio:

```
[t=   4.0s] Queue                    size    cap     prod     cons     drop    hwm
           -----------------------------------------------------------------
           micRaw                      0     64      396      396        0      1
           micAdapted                  0    128      396      396        0      1
           vadGated                    0    128      208      208        0      1
           cleanAudio                  0    128      208      208        0      1
           sttAdapted                  0     64        0        0        0      0
           sttText                     0    512        4        4        0      1
           llmText                     0    256        8        8        0      1
           speaker                     0    128       59       59        0      1
           ttsRef                      1    128       59       58        0      1
           vadEvents                   1     64        1        0        0      1
           bargeIn                     0     16        0        0        0      0
           pool=512/512 free | llmMem=138 B | ttsInt=idle | llmInt=idle
```

- Il timestamp `t` e' relativo all'avvio del reporter (secondi dall'inizio)
- Il separatore e' ASCII puro (`-`) per compatibilita' con terminali senza UTF-8
  (Windows console legacy, terminali embedded)
- La riga extra e' prodotta da una callback configurabile (`setExtraLineCallback`)

---

## SimulatedVadNode

Macchina a stati Silence ↔ Speaking con durate random:

| Stato | Durata |
|---|---|
| Silence | 500..2000 ms (uniforme) |
| Speaking | 1000..4000 ms (uniforme) |

Durante Silence i frame non vengono propagati a valle. Il seed del generatore
e' configurabile per simulazioni riproducibili.

I timestamp negli eventi e nei log sono **relativi all'avvio del nodo** (`+Xs`),
non all'epoch di `steady_clock` (che su kernel Linux recenti vale l'uptime della
macchina e produce valori come `1729434s`).

---

## SimulatedBargeInNode

- Thread A (`runLoop`): riceve testo STT, cerca parole chiave, forwarda all'LLM
- Thread B (`kbLoop`): polling stdin non-bloccante ogni 50ms

Parole chiave default: `stop`, `interrompi`, `aspetta`, `fermati`, `basta`, `silenzio`

Tastiera: tasto `b` (seguito da Enter su terminali con line buffering).

---

## Formati audio e adapter

Esempio di configurazione con mic nativo 48kHz stereo e pipeline interna 16kHz mono:

```cpp
cfg.micRawFormat.sampleRate   = 48000;
cfg.micRawFormat.channels     = 2;
cfg.micRawFormat.sampleFormat = SampleFormat::Int16;

cfg.pipelineFormat.sampleRate = 16000;
cfg.pipelineFormat.channels   = 1;
cfg.pipelineFormat.sampleFormat = SampleFormat::Int16;

cfg.enableMicAdapter = true;   // VoiceRuntime crea MicAdapter automaticamente
```

Se `micRawFormat == pipelineFormat` il nodo non viene creato e `micRawQueue_`
non viene utilizzata.

---

## Backends previsti per Step 2

### DSP / AEC
- WebRTC APM (AEC3) — richiede 16kHz mono Int16, frame 10ms
- RNNoise — richiede 48kHz mono Float32, frame 10ms (480 campioni);
  necessita adapter prima/dopo se la pipeline e' a 16kHz

### STT
- Qwen3-ASR-1.7B via GGML/GGUF
- Il nodo accumula frame interni fino alla finestra del modello con VAD-gating

### LLM
- Interfaccia generica OpenAI-compatible (HTTP/HTTPS locale)
- Streaming SSE con `TextChunk::isFinal = false` per token intermedi
- Cancellazione tramite `InterruptSignal`

### TTS
- Kokoro via GGML/GGUF
- Input: chunk di testo a granularita' di frase
- Output: frame audio a 24kHz (adapter necessario verso pipeline 16kHz)

### Formato GGML/GGUF
- Backend primario su tutte le piattaforme target
- ONNX Runtime come backend alternativo dove disponibile (x86/ARM con acceleratori)

---

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/voice_runtime_simulation
```

Dipendenze: C++17, pthreads (Unix). Nessuna dipendenza esterna nello Step 1.
