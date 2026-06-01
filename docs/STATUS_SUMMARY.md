# Voice Runtime — Riepilogo dello Stato Attuale

Questo documento fornisce un riepilogo dello stato attuale dello sviluppo del runtime vocale conversazionale e definisce i punti di partenza per i prossimi task.

---

## 1. Stato dei Componenti Principali

### A. Integrazione DSP/AEC e VAD (WebRTC) — 7 Bug Risolti
- **Implementazione**: Completata in `nodes/WebRtcDspNode.h` come nodo unico che agisce sia da `IAecDspNode` che da `IVadNode` (Configurazione B dell'architettura).
- **Processamento**: Esegue cancellazione dell'eco acustico (AEC3), soppressione del rumore (NS) e rilevamento dell'attività vocale (VAD) integrato basato su WebRTC.
- **Correzione Concorrenza (Metal/GGML)**: Risolto il potenziale crash di concorrenza tra l'inferenza VAD (Silero su CPU) e STT (Qwen3 su Metal GPU) tramite il mutex globale `g_ggml_mutex` con try-lock non bloccante in `SileroVadNode.h`.

#### Bug AEC critici risolti (2026-05-29)

| Bug | Descrizione | Fix |
|-----|-------------|-----|
| **Bug 1** | `drainRenderQueue()` deve mantenere render e capture in lockstep temporale | Usa `tryPop()` non bloccante, massimo un frame TTS per frame mic |
| **Bug 2** | Drain avveniva solo dopo il pop bloccante, mai prima | Aggiunto drain **prima** del `pop()` e **dopo** |
| **Bug 3** | `aecRefQueue` da 256 frame (2.56s pre-fill) — offset non compensabile da AEC3 | Capacità ridotta a **32 frame** (320ms) |
| **Bug 4** | `TtsStateSignal::setActive(true)` potenzialmente dopo il primo push | Contratto documentato con commento esplicito; codice già corretto |
| **Bug 5** | Nessuna validazione del formato AEC reference — mismatch silenzioso | Aggiunto `validateRenderFormat()` + `std::abort()` su mismatch |
| **Bug 6** | `processRenderFrame()` e `processCaptureFrame()` allocavano `std::vector` ogni 10ms | Rimpiazzati con membri preallocati `renderDestI16_` / `captureDestI16_` |
| **Bug 7** | `apm_->set_stream_delay_ms()` chiamato ad ogni frame capture (100x/s) | Spostato in `initialize()` — chiamato una volta sola |

### B. BoundedQueue — Ottimizzazioni Strutturali (Fix 4.4, 4.6)

- **Contatori atomici (Fix 4.6)**: `produced`, `consumed`, `dropped` sono ora `std::atomic<uint64_t>` separati dal mutex della coda. `MetricsReporter` li legge senza mai acquisire il lock — zero contesa sui thread RT.
- **DropCallback senza re-lock (Fix 4.4)**: `invokeDropCallback()` controlla `hasDropCallback_` (flag atomico) prima di invocare, senza ri-acquisire il mutex. Precondizione documentata: `setDropCallback()` va chiamato prima di `start()`.

### C. Nodi Audio Windows (WASAPI)

Due nuovi nodi header-only Windows-native creati in `nodes/`:

- **`WasapiMicNode.h`**: cattura WASAPI event-driven, SHARED/EXCLUSIVE, DC removal, resampling lineare, accumulo frame a `frameMs` ms.
- **`WasapiOutputNode.h`**: render WASAPI event-driven, SHARED/EXCLUSIVE, pre-buffer silenzio, underrun prevention, conversione Int16↔Float32, resampling.

Test dedicati: `tests/test_wasapi_mic.cpp`, `tests/test_wasapi_output.cpp`.
Supporto CMake: opzione `VOICE_RUNTIME_ENABLE_WASAPI` (default OFF).

### D. Miniaudio Output Node (Riproduzione Audio)
- **Risoluzione Volume Drop & Stuttering**: Identificato e risolto il problema per cui l'audio TTS calava a zero o saltava alla fine delle frasi. I picchi di CPU indotti dalla sintesi ONNX (Kokoro) sui thread normal-priority causavano starvation del worker thread di output.
- **Nuova Architettura Callback-Driven**: Il nodo `MiniaudioOutputNode` è stato riscritto per consumare i frame audio direttamente dalla `speakerQueue` (`in_`) all'interno della callback real-time di miniaudio. Il vecchio ring buffer, il relativo mutex e il worker thread associato sono stati eliminati.
- **Pacing e Conversione**: Gestione nativa del formato Int16 e Float32 e pacing tramite buffer residui direttamente nella callback con priorità real-time del sistema operativo.

### E. Build e Gestione Dipendenze (`deps/`)
- **Stato delle patch**: Aggiornate e rigenerate tramite `python3 generate_patches.py`. Le patch contengono le modifiche locali per `espeak-ng`, `kokoro.cpp` (supporto per `ONNXRUNTIME_ROOT` custom) e `qwen3-asr.cpp` (compatibilità MSVC su Windows, mmap Win32, e linking Accelerate su Apple).
- **Supporto Piattaforma**: Build statiche supportate e validate per Windows, macOS (OSX) e iOS.

---

## 2. Test e Validazione

## Follow-up: Qwen3-TTS Performance Optimization & Voice Consistency

### 1. Fix Greedy Decoding Repetition Loops & Temperature Settings
- **Problem**: Changing the default temperature to `0.0f` (greedy decoding) caused the autoregressive generation loop to get trapped in infinite repetition loops for longer sentences. The model failed to generate the `codec_eos_id` (End of Sequence) and generated garbage tokens until it hit the hard limit (`max_audio_tokens = 4096`), inflating synthesis time to **322.5 seconds** (5.4 minutes) for a 197-character sentence.
- **Solution**: Set the default temperature in [Qwen3TtsNode.h](file:///Users/simone/voice_runtime/nodes/Qwen3TtsNode.h) to `0.5f`. This breaks the repetition loop, letting the model successfully generate the End-of-Sequence token and finish quickly, while preserving high speech quality.

### 2. Automatic Voice Consistency Fallback
- **Problem**: When no `--tts-voice` is specified, the model generated speech stochastically, which led to voice style and gender characteristics changing randomly on every sentence.
- **Solution**: Implemented a fallback loader inside `Qwen3TtsNode::initialize`. If no explicit voice path is passed, the node searches for a default voice file in:
  1. `models/tts/default_voice.wav`
  2. `deps/qwen3-tts.cpp/examples/readme_clone_input.wav`
- Under `deps/download_models.py`, we automatically copy the cloned repository's sample reference voice to `models/tts/default_voice.wav` during setup.
- This ensures that a single, consistent speaker embedding is extracted once at startup and used for the entire session, keeping the voice completely constant.

### 3. Rebuild and Verification
- Executed compilation and ran the simulation:
  ```bash
  /Applications/CMake.app/Contents/bin/cmake --build build -j
  build/test_aec_transcription tests/data/user_reading.txt tests/data/tts_playback.txt 45 --tts qwen3
  ```
- **Results**:
  - The default reference voice `deps/qwen3-tts.cpp/examples/readme_clone_input.wav` was loaded automatically and successfully:
    `[Qwen3TTS] Loading reference voice: deps/qwen3-tts.cpp/examples/readme_clone_input.wav`
    `[Qwen3TTS] Speaker embedding successfully extracted (1024 floats)`
  - Synthesis timings showed a **25x speedup** on the 197-character sentence, dropping from **322,534 ms** down to **13,041 ms** (~1.1x real-time speed):
    - Sentence 1 (44 chars): `Total Synth: 3219.8 ms`
    - Sentence 2 (47 chars): `Total Synth: 2782.2 ms`
    - Sentence 3 (197 chars): `Total Synth: 13041.9 ms`
  - The voice remained completely consistent and constant across all playback sentences.
  - The pipeline shut down cleanly after 45 seconds without deadlocks or resource leaks.

---

I seguenti test sono compilabili in modalità reale (`VOICE_RUNTIME_ENABLE_WEBRTC_APM=ON`) ed eseguibili con successo:

1. **Test AEC Unit**:
   ```bash
   ./build-real/test_webrtc_aec_unit
   ```
   *Verifica*: Conferma che l'eco del TTS viene rimosso dal microfono e che il VAD non fa scattare l'STT erroneamente. (`PASS`).
   *Indicatori post-fix*: `aecRefQueue.stats().highWatermark <= 32`; log `erle=` crescente dopo 500ms.

2. **Test Singolo TTS**:
   ```bash
   ./build-real/test_tts_node tests/data/tts_playback.txt play
   ```
   *Verifica*: L'audio riprodotto sui dispositivi fisici è pulito, continuo, privo di click all'avvio e non presenta drop di volume a fine frase.

3. **Test Trascrizione Full-Duplex**:
   ```bash
   ./build-real/test_aec_transcription tests/data/user_reading.txt tests/data/tts_playback.txt 30
   ```
   *Verifica*: Avvia la pipeline completa, sintetizza il testo del playback, cancella l'eco dal microfono e trascrive la voce dell'utente. Lo spegnimento è pulito senza deadlock o perdite nei pool preallocati.

4. **Test WASAPI Mic** *(Windows, `VOICE_RUNTIME_ENABLE_WASAPI=ON`)*:
   ```powershell
   .\build-real\test_wasapi_mic.exe
   ```
   *Verifica*: Cattura 5 secondi dal microfono default, stampa RMS ogni 10 frame, verifica >= 450 frame prodotti (90% di 500 attesi @ 10ms). `[PASS]` se soglia soddisfatta.

5. **Test WASAPI Output** *(Windows, `VOICE_RUNTIME_ENABLE_WASAPI=ON`)*:
   ```powershell
   .\build-real\test_wasapi_output.exe
   ```
   *Verifica*: Genera 2 secondi di tono 440Hz a 24kHz Int16, lo push sulla speakerQueue, attende drain. `[PASS]` se `dropped == 0`.

---

## 3. Configurazione per lo Sviluppo Futuro

### Comandi Utili per il Build

1. **Compilazione delle Dipendenze**:
   ```bash
   python3 deps/build_deps.py --platform osx
   ```
2. **Build base (Unix/macOS)**:
   ```bash
   cmake -B build-real -S . -DVOICE_RUNTIME_ENABLE_WEBRTC_APM=ON
   cmake --build build-real -j4
   ```
3. **Build con WASAPI (Windows)**:
   ```powershell
   cmake -B build-real -S . `
     -DVOICE_RUNTIME_ENABLE_WEBRTC_APM=ON `
     -DVOICE_RUNTIME_ENABLE_WASAPI=ON
   cmake --build build-real -j4
   ```

### Posizione dei Modelli Richiesti
- **VAD Silero**: `models/vad/ggml-silero-v6.2.0.bin`
- **STT Qwen3**: `models/stt/` (file `.gguf` del modello e del proiettore)
- **TTS Kokoro**: `models/tts/kokoro-v1.1-zh.onnx` e `models/tts/voices.bin`

---

## 4. Prossimi Task / Lavoro Futuro

1. **Asset di Dizionario Kokoro Real**:
   - Attualmente la sintesi Kokoro nel test AEC utilizza un fallback stub in assenza dei dizionari completi. È necessario configurare il download degli asset completi di dizionario (`vocab.txt` e `jieba_dict`) sotto `models/tts/dict/` per supportare la sintesi di testo arbitrario complessa in produzione.
2. **InterpreterNode di Produzione**:
   - `InterpreterNode` al momento funziona come pass-through stub. Occorre implementare il parsing semantico reale per estrarre il testo leggibile dall'output strutturato dell'LLM (filtrando tag, comandi e JSON strutturati).
3. **Resampling di Alta Qualità**:
   - `AudioFormatAdapterNode` e i nuovi nodi WASAPI eseguono resampling lineare. Per la produzione si raccomanda l'integrazione di `r8brain` per preservare la fedeltà audio. Il resampling lineare è documentato come provvisorio in tutti i punti in cui viene usato.
4. **Analisi e Riduzione della Latenza**:
   - Profilare la pipeline end-to-end per identificare eventuali colli di bottiglia e ridurre al minimo la latenza tra il completamento del parlato dell'utente e l'avvio della risposta TTS.
5. **WASAPI EXCLUSIVE Mode — Validazione su Hardware Reale**:
   - La modalità EXCLUSIVE è implementata e opzionale via config, ma non ancora testata su hardware consumer. Richiede che il sample rate del device corrisponda esattamente al formato configurato (nessun resampling automatico in EXCLUSIVE).
