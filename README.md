# voice_runtime

Header-only prototype for a multithreaded C++ full-duplex voice assistant runtime.

It models:

- preallocated audio buffers
- bounded queues
- one active thread per runtime node
- microphone -> AEC -> STT -> LLM -> TTS -> output pipeline
- TTS reference stream routed back to AEC
- disk-backed LLM input buffering
- throughput simulation

Build:

```bash
cmake -S . -B build
cmake --build build
./build/voice_runtime_simulation
```

# Introduzione

L’evoluzione dei modelli linguistici, dei sistemi di sintesi vocale e delle tecnologie di riconoscimento del parlato sta portando alla nascita di una nuova generazione di sistemi conversazionali real-time. Tuttavia, la maggior parte delle architetture esistenti è progettata come una semplice concatenazione di componenti indipendenti, senza affrontare in maniera rigorosa i problemi strutturali legati a:

* latenza,
* parallelismo,
* sincronizzazione audio,
* gestione del throughput,
* controllo della memoria,
* stabilità temporale del sistema,
* echo cancellation full-duplex,
* back-pressure tra i nodi,
* persistenza del contesto conversazionale.

Questo progetto nasce con l’obiettivo di definire una infrastruttura software modulare, altamente parallela e cross-platform per la costruzione di assistenti vocali real-time di nuova generazione.

L’architettura proposta non rappresenta semplicemente un wrapper attorno a STT, LLM e TTS, ma un vero e proprio runtime conversazionale full-duplex, progettato per operare in tempo reale con vincoli deterministici di memoria, buffering e sincronizzazione.

---

# Visione del progetto

L’obiettivo è costruire una piattaforma capace di supportare sistemi vocali conversazionali continui, nei quali:

* l’utente può parlare mentre il sistema risponde,
* il sistema continua ad ascoltare durante il TTS,
* il flusso audio viene elaborato senza interruzioni,
* ogni componente lavora in parallelo,
* il throughput si stabilizza automaticamente,
* la memoria non cresce indefinitamente,
* il sistema può essere simulato e profilato in maniera deterministica.

Il progetto introduce quindi una architettura orientata ai flussi (“stream-oriented”), nella quale ogni nodo della pipeline rappresenta un componente indipendente eseguito su thread dedicati.

---

# Obiettivi principali

## 1. Architettura full-duplex real-time

Il sistema deve permettere comunicazioni vocali simultanee:

* acquisizione microfono continua,
* sintesi vocale continua,
* cancellazione dell’eco in tempo reale,
* riconoscimento vocale streaming,
* generazione linguistica incrementale.

L’utente non deve percepire una modalità half-duplex “premi e parla”, ma una conversazione naturale e continua.

---

## 2. Separazione completa dei nodi

Ogni elemento della pipeline viene modellato come nodo indipendente:

```text
Microphone
TTS
DSP / AEC
STT
LLM
Storage
Audio Output
```

Ogni nodo:

* possiede thread dedicati,
* usa code bounded,
* comunica solo tramite buffer,
* non accede direttamente allo stato interno degli altri nodi.

Questo approccio consente:

* scalabilità,
* isolamento,
* profiling,
* sostituzione trasparente dei backend,
* simulazione indipendente.

---

# 3. Gestione deterministica della memoria

Uno degli obiettivi centrali del progetto è evitare allocazioni continue durante il runtime.

Per questo motivo:

* i buffer audio vengono preallocati all’avvio,
* il sistema usa pool di buffer condivisi,
* le code sono bounded,
* il numero massimo di buffer tende a stabilizzarsi automaticamente,
* il throughput massimo del sistema emerge naturalmente durante il warm-up iniziale.

Questo approccio riduce:

* frammentazione,
* jitter,
* pause da allocatore,
* spike di latenza,
* crescita incontrollata della memoria.

---

# 4. Back-pressure e stabilizzazione del throughput

L’architettura è progettata per funzionare come sistema dinamico stabilizzato.

Ogni nodo possiede una capacità limitata e introduce naturalmente:

* back-pressure,
* throttling,
* accumulo controllato,
* sincronizzazione implicita.

In questo modo il sistema converge verso una configurazione stabile anche in condizioni di carico elevato.

---

# 5. Gestione intelligente del collo di bottiglia LLM

Nel mondo reale il nodo più lento della pipeline è quasi sempre il Large Language Model.

STT e TTS possono facilmente operare più velocemente del parlato umano, mentre l’LLM introduce latenza computazionale significativa.

Per questo motivo il progetto introduce:

* ring buffer testuali,
* memoria conversazionale limitata,
* spool persistente su disco,
* accumulo progressivo del testo,
* segmentazione intelligente del contesto.

L’LLM diventa quindi un nodo asincrono che può elaborare quantità molto grandi di testo senza imporre limiti artificiali di RAM.

---

# 6. Persistenza su disco del contesto conversazionale

Il sistema considera il disco come estensione naturale della memoria.

Questo consente:

* trascrizioni lunghe,
* conversazioni persistenti,
* recupero storico,
* replay,
* debugging,
* analisi offline,
* ricostruzione temporale dei flussi.

La pipeline non dipende quindi esclusivamente dalla RAM disponibile.

---

# 7. Simulazione completa del runtime

L’architettura include un framework di simulazione che permette di:

* testare il throughput,
* verificare la stabilizzazione dei buffer,
* misurare latenze,
* simulare nodi lenti,
* verificare il comportamento sotto carico,
* individuare deadlock,
* validare ownership e lifetime dei buffer.

Ogni componente reale può essere sostituito con un simulatore sintetico.

---

# 8. Modularità dei backend

Ogni nodo è definito tramite interfacce astratte.

Questo consente di sostituire liberamente:

## STT

* Whisper.cpp
* Vosk
* Deepgram
* Azure Speech
* Google STT

## LLM

* OpenAI
* Claude
* Gemini
* llama.cpp
* Ollama
* vLLM

## TTS

* Piper
* XTTS
* ElevenLabs
* Kokoro
* Azure TTS

## DSP / AEC

* WebRTC APM
* RNNoise
* DSP proprietari

---

# 9. Compatibilità multipiattaforma

L’architettura è progettata per funzionare su:

* Windows,
* macOS,
* Linux,
* Android,
* iOS.

L’astrazione dell’audio permette di integrare backend specifici:

```text
WASAPI
CoreAudio
ALSA
PipeWire
AAudio
AudioUnit
```

senza modificare la logica del runtime.

---

# 10. Fondamenti architetturali

Il progetto si basa su alcuni principi fondamentali:

## Pipeline a flussi continui

I dati scorrono continuamente tra nodi indipendenti.

---

## Ownership esplicita dei buffer

I buffer non vengono copiati inutilmente.

---

## Nessuna allocazione nel path real-time

Il runtime operativo evita allocazioni dinamiche.

---

## Bounded queues

Ogni coda possiede capacità limitata.

---

## Thread isolation

Ogni nodo è isolato dagli altri.

---

## Determinismo

Il sistema deve poter essere simulato e profilato.

---

# Conclusione

Questo progetto rappresenta la base per una nuova categoria di runtime conversazionali real-time ad alte prestazioni, progettati non solo per integrare tecnologie AI moderne, ma per orchestrare in maniera rigorosa e deterministica flussi audio, testuali e computazionali complessi.

L’obiettivo finale non è semplicemente creare un assistente vocale, ma costruire una infrastruttura modulare, stabile e scalabile capace di supportare sistemi conversazionali continui, naturali e persistenti su qualsiasi piattaforma hardware e software.
