# Voice Runtime Architecture

## Obiettivo

Questa architettura modella un runtime audio conversazionale full-duplex in C++.
Ogni nodo critico gira in un thread indipendente per massimizzare il parallelismo e rendere misurabile il throughput.

La pipeline logica e' questa:

```text
MicrophoneNode
    -> mic audio queue
    -> AEC/DSP Node
    -> clean audio queue
    -> STT Node
    -> transcribed text queue
    -> LLM Node
    -> generated text queue
    -> TTS Node
    -> speaker audio queue
    -> AudioOutputNode

TTS Node
    -> AEC reference queue
    -> AEC/DSP Node
```

## Principio fondamentale

Le callback dei dispositivi o dei backend non devono fare lavoro pesante.
Devono solo acquisire o produrre un buffer e inserirlo in una struttura bounded.
Il lavoro effettivo viene svolto da nodi attivi (`IActiveNode`) che possiedono un thread.

## Nodi attivi

Ogni componente implementa `IActiveNode`:

- `initialize()`
- `start()`
- `stop()`
- `name()`

Le classi base fornite sono:

- `ActiveNodeBase`
- `IMicrophoneNode`
- `IAecDspNode`
- `ISpeechToTextNode`
- `ILanguageModelNode`
- `ITextToSpeechNode`
- `IAudioOutputNode`

Ogni implementazione concreta puo' essere sostituita con backend reali:

- audio input/output: WASAPI, CoreAudio, ALSA, PipeWire, AAudio, AVAudioEngine
- DSP/AEC: WebRTC APM / AEC3
- STT: Whisper.cpp, Vosk, servizi cloud
- LLM: OpenAI, llama.cpp, Ollama, server custom
- TTS: Piper, Kokoro, XTTS, Apple, Android, servizi cloud

## Buffer audio

I frame audio sono rappresentati da `AudioFrame` e trasportati come `AudioFrameHandle`, cioe' `std::shared_ptr<AudioFrame>`.

Il pacchetto contiene un `SharedBufferPool<T>` che prealloca i buffer all'avvio e li ricicla tramite custom deleter di `std::shared_ptr`.

Questo evita allocazioni continue nel ciclo realtime.

## Code bounded

`BoundedQueue<T>` supporta tre politiche:

- `BlockProducer`
- `DropNewest`
- `DropOldest`

Per l'audio realtime, di solito `BlockProducer` e' sicura nei test, ma in produzione puo' essere utile `DropOldest` su flussi non critici.

Per il testo, `DropOldest` puo' essere accettabile per finestre conversazionali, ma l'input dell'LLM viene anche scritto su disco.

## Stabilizzazione del sistema

Il sistema non usa buffer infiniti.
Ogni coda ha una capacita' definita.
Durante l'avvio del running il sistema raggiunge un equilibrio misurabile tramite:

- elementi prodotti
- elementi consumati
- elementi droppati
- high watermark
- ricicli del pool

Il test `voice_runtime_simulation` stampa queste metriche al termine della simulazione.

## LLM come collo di bottiglia

Il nodo LLM e' progettato per essere il punto piu' lento della pipeline.
Per questo riceve testo da una coda bounded e lo registra anche in `DiskBackedTextBuffer`.

La memoria mantiene solo una finestra recente, mentre il testo completo puo' essere spooled su disco.
Questo permette di gestire casi in cui l'utente parla a lungo e l'LLM e' piu' lento del flusso STT.

## Persistenza del testo

Sono disponibili due strutture:

- `RollingTextBuffer`: mantiene una finestra in memoria sovrascrivendo l'inizio quando supera `maxBytes`.
- `DiskBackedTextBuffer`: scrive tutto su disco e conserva in RAM solo una finestra recente.

## Simulazione

Il pacchetto include nodi simulati:

- `SimulatedMicrophoneNode`
- `SimulatedAecNode`
- `SimulatedSttNode`
- `SimulatedLlmNode`
- `SimulatedTtsNode`
- `SimulatedAudioOutputNode`

Il test serve per studiare:

- throughput
- back-pressure
- dimensionamento delle code
- stabilizzazione dei pool
- comportamento quando l'LLM e' piu' lento

## Build

```bash
cmake -S . -B build
cmake --build build
./build/voice_runtime_simulation
```

## File principali

```text
include/voice_runtime/AudioTypes.h
include/voice_runtime/ActiveNode.h
include/voice_runtime/BoundedQueue.h
include/voice_runtime/SharedBufferPool.h
include/voice_runtime/TextBuffer.h
include/voice_runtime/Interfaces.h
include/voice_runtime/RuntimeConfig.h
include/voice_runtime/VoiceRuntime.h
tests/SimulatedNodes.h
tests/simulation_main.cpp
CMakeLists.txt
```

## Nota progettuale importante

In una implementazione reale il nodo TTS deve inviare ogni frame sia alla coda speaker sia alla coda reference dell'AEC.
La reference dell'AEC deve ricevere il frame il prima possibile, idealmente prima o contestualmente alla riproduzione sul dispositivo audio.

## Correzione lifetime buffer e shutdown

La versione aggiornata evita il deadlock osservato a fine `main()` eliminando il custom deleter che catturava `this` dentro `SharedBufferPool::acquireBlocking()`.

Il problema era architetturale: i nodi simulati possiedono pool interni, ma le code del runtime possono ancora contenere `std::shared_ptr<AudioFrame>` verso buffer prodotti da quei pool. Se il nodo viene distrutto prima che le code vengano svuotate, il deleter del buffer può richiamare `SharedBufferPool::release()` su un oggetto già in distruzione.

La nuova implementazione separa:

```text
SharedBufferPool<T> object
    -> std::shared_ptr<State>
        -> buffers
        -> free list
        -> mutex
        -> condition_variable
        -> stats

AudioFrameHandle
    -> custom deleter che cattura std::shared_ptr<State>, non this
```

In questo modo lo `State` rimane vivo fino alla distruzione dell'ultimo handle pendente. Il pool può essere distrutto senza invalidare i buffer ancora presenti nelle code.

La sequenza di shutdown del runtime ora chiude prima tutte le code, poi ferma i nodi, poi svuota le code:

```text
stop all queues
stop all active nodes
clear all queues
stop shared runtime pools
```

Questo sblocca sia i producer bloccati su `push()` sia i consumer bloccati su `pop()`. Inoltre i nodi simulati che possiedono un pool interno implementano `wake()` chiamando `pool_.stop()`, così anche un thread bloccato in `acquireBlocking()` viene svegliato durante lo shutdown.

## Test di regressione

La simulazione è stata ricompilata ed eseguita. Il processo termina correttamente dopo `return 0`, senza blocco nel distruttore del pool.
