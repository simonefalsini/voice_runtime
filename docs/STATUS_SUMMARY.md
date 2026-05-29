# Voice Runtime — Riepilogo dello Stato Attuale

Questo documento fornisce un riepilogo dello stato attuale dello sviluppo del runtime vocale conversazionale e definisce i punti di partenza per i prossimi task.

---

## 1. Stato dei Componenti Principali

### A. Integrazione DSP/AEC e VAD (WebRTC)
- **Implementazione**: Completata in `nodes/WebRtcDspNode.h` come nodo unico che agisce sia da `IAecDspNode` che da `IVadNode` (Configurazione B dell'architettura).
- **Processamento**: Esegue cancellazione dell'eco acustico (AEC3), soppressione del rumore (NS) e rilevamento dell'attività vocale (VAD) integrato basato su WebRTC.
- **Correzione Concorrenza (Metal/GGML)**: Risolto il potenziale crash di concorrenza tra l'inferenza VAD (Silero su CPU) e STT (Qwen3 su Metal GPU) tramite il mutex globale `g_ggml_mutex` con try-lock non bloccante in `SileroVadNode.h`.

### B. Miniaudio Output Node (Riproduzione Audio)
- **Risoluzione Volume Drop & Stuttering**: Identificato e risolto il problema per cui l'audio TTS calava a zero o saltava alla fine delle frasi. I picchi di CPU indotti dalla sintesi ONNX (Kokoro) sui thread normal-priority causavano starvation del worker thread di output.
- **Nuova Architettura Callback-Driven**: Il nodo `MiniaudioOutputNode` è stato riscritto per consumare i frame audio direttamente dalla `speakerQueue` (`in_`) all'interno della callback real-time di miniaudio. Il vecchio ring buffer, il relativo mutex e il worker thread associato sono stati eliminati.
- **Pacing e Conversione**: Gestione nativa del formato Int16 e Float32 e pacing tramite buffer residui direttamente nella callback con priorità real-time del sistema operativo.

### C. Build e Gestione Dipendenze (`deps/`)
- **Stato delle patch**: Aggiornate e rigenerate tramite `python3 generate_patches.py`. Le patch contengono le modifiche locali per `espeak-ng`, `kokoro.cpp` (supporto per `ONNXRUNTIME_ROOT` custom) e `qwen3-asr.cpp` (compatibilità MSVC su Windows, mmap Win32, e linking Accelerate su Apple).
- **Supporto Piattaforma**: Build statiche supportate e validate per Windows, macOS (OSX) e iOS.

---

## 2. Test e Validazione

I seguenti test sono compilabili in modalità reale (`VOICE_RUNTIME_ENABLE_WEBRTC_APM=ON`) ed eseguibili con successo:

1. **Test AEC Unit**:
   ```bash
   ./build-real/test_webrtc_aec_unit
   ```
   *Verifica*: Conferma che l'eco del TTS viene rimosso dal microfono e che il VAD non fa scattare l'STT erroneamente. (`PASS`).

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

---

## 3. Configurazione per lo Sviluppo Futuro

### Comandi Utili per il Build

1. **Compilazione delle Dipendenze**:
   ```bash
   python3 deps/build_deps.py --platform osx
   ```
2. **Configurazione e Build del Progetto**:
   ```bash
   cmake -B build-real -S . -DVOICE_RUNTIME_ENABLE_WEBRTC_APM=ON
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
   - In `AudioFormatAdapterNode` viene eseguito un resampling lineare semplice. Per la produzione si raccomanda l'integrazione di una libreria di resampling professionale (es. `r8brain`) per preservare la fedeltà audio durante la conversione del sample rate.
4. **Analisi e Riduzione della Latenza**:
   - Profilare la pipeline end-to-end per identificare eventuali colli di bottiglia e ridurre al minimo la latenza tra il completamento del parlato dell'utente e l'avvio della risposta TTS.
