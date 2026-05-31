# Guida allo Sviluppo e Debug con IDE Cross-Platform

Questa guida descrive come configurare l'ambiente di sviluppo e debug per `voice_runtime` su ciascuna piattaforma supportata usando le IDE più adatte, sfruttando la configurazione centralizzata tramite CMake.

---

## 1. Configurazione Centralizzata (`CMakePresets.json`)

Al fine di standardizzare la generazione dei progetti tra i vari IDE, il repository include un file `CMakePresets.json` nella radice del progetto. I preset disponibili sono:
- `windows-vs`: Visual Studio per Windows (x64)
- `macos-xcode`: Xcode su macOS per build nativa
- `ios-xcode`: Xcode su macOS per compilazione ed esecuzione su iOS
- `linux-native`: Ninja/Makefiles per sviluppo nativo su Linux
- `android-ndk-arm64`: Cross-compilazione Android NDK su Linux (richiede l'ambiente `$ANDROID_NDK_HOME`)

---

## 2. Windows (Visual Studio)

Visual Studio (2019 / 2022) supporta nativamente i progetti CMake tramite la funzione "Apri cartella".

### Configurazione:
1. Apri Visual Studio.
2. Seleziona **File** -> **Apri** -> **Cartella...** e seleziona la radice di `voice_runtime`.
3. Visual Studio rileverà automaticamente il file `CMakePresets.json` e caricherà il preset `windows-vs`.
4. Nel menu a discesa in alto per le configurazioni, assicurati che sia selezionato il preset `windows-vs` in modalità `Debug`.
5. Fai clic destro su `CMakeLists.txt` e scegli **Genera cache** (oppure attendi la generazione automatica).
6. Seleziona l'eseguibile di destinazione (es. `voice_runtime_real_pipeline.exe` o `test_webrtc_aec_unit.exe`) come target di avvio e premi **F5** per avviare il debug.

---

## 3. macOS & iOS (Xcode)

Su macOS, per lo sviluppo nativo e il debug su dispositivi fisici o simulatori iOS, lo strumento principale è **Xcode**.

### Configurazione macOS:
1. Dalla radice del progetto, genera il progetto Xcode nativo usando il preset preconfigurato:
   ```bash
   cmake --preset macos-xcode
   ```
   *In alternativa, senza i preset:*
   ```bash
   cmake -S . -B build-xcode-mac -G Xcode -DVOICE_RUNTIME_ENABLE_WEBRTC_APM=ON
   ```
2. Apri il file generato `build-xcode-mac/voice_runtime.xcodeproj` in Xcode:
   ```bash
   open build-xcode-mac/voice_runtime.xcodeproj
   ```
3. In Xcode, seleziona lo schema desiderato (es. `voice_runtime_real_pipeline` o `test_webrtc_aec_unit`).
4. Imposta lo schema di build in modalità **Debug** (cliccando sullo schema in alto -> *Edit Scheme...* -> *Run* -> *Build Configuration: Debug*).
5. Premi **Cmd + R** per avviare la compilazione e il debug con LLDB integrato.

### Configurazione iOS:
1. Genera il progetto Xcode per iOS cross-compilation:
   ```bash
   cmake --preset ios-xcode
   ```
   *In alternativa, senza i preset:*
   ```bash
   cmake -S . -B build-xcode-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DVOICE_RUNTIME_ENABLE_WEBRTC_APM=ON
   ```
2. Apri il progetto generato:
   ```bash
   open build-xcode-ios/voice_runtime.xcodeproj
   ```
3. Seleziona la destinazione (dispositivo iOS fisico collegato o simulatore).
4. Nel pannello di configurazione del target, imposta il tuo team di sviluppo per il codice di firma (*Signing & Capabilities*).
5. Clicca su **Run** per avviare la build e il debug con LLDB sul dispositivo iOS.

---

## 4. Android (sotto Linux)

Per sviluppare e fare il debug della codebase C++ per Android sotto Linux, ci sono tre opzioni principali.

### Opzione A: Android Studio (Consigliata per integrazione App)
Android Studio supporta direttamente l'apertura e il debug di file C++ accoppiati a una build Gradle.

1. Se stai sviluppando un'applicazione Android (Java/Kotlin + C++), definisci il wrapper nativo all'interno del file `build.gradle` (o `build.gradle.kts`) del modulo dell'app:
   ```groovy
   android {
       ...
       defaultConfig {
           externalNativeBuild {
               cmake {
                   cppFlags "-std=c++20"
                   arguments "-DVOICE_RUNTIME_ENABLE_WEBRTC_APM=ON"
               }
           }
       }
       externalNativeBuild {
           cmake {
               path "../CMakeLists.txt" // Percorso del CMakeLists.txt di voice_runtime
               version "3.22.1"
           }
       }
   }
   ```
2. Apri il progetto in Android Studio. L'IDE sincronizzerà Gradle e caricherà la struttura ad albero CMake.
3. Sarà possibile inserire breakpoint direttamente nei file `.cpp` (es. `WebRtcDspNode.h` o `VoiceRuntime.h`).
4. Esegui l'app in modalità Debug (**Shift + F9**) su un emulatore o dispositivo fisico. Sotto la scheda *Debug*, seleziona il debugger *Dual* o *Native* per agganciare LLDB.

### Opzione B: CLion (Consigliata per debug puro C++ NDK)
JetBrains CLion è un IDE dedicato per C/C++ ed ha un supporto eccezionale per lo sviluppo Android NDK cross-compilato.

1. Assicurati che l'ambiente abbia configurato l'SDK Android e il NDK. Definisci la variabile d'ambiente:
   ```bash
   export ANDROID_NDK_HOME=/path/to/android-sdk/ndk/25.x.xxxx (o simile)
   ```
2. Apri la cartella di `voice_runtime` in CLion.
3. CLion rileverà automaticamente `CMakePresets.json`. Vai su **Settings** -> **Build, Execution, Deployment** -> **CMake**.
4. Vedrai il profilo `android-ndk-arm64`. Abilitalo.
5. CLion configurerà la cache usando il toolchain NDK.
6. Per il debug di un eseguibile nativo (es. un test command-line) su un dispositivo Android:
   - Configura una destinazione del tipo *Remote Debug* o *Android Native Application*.
   - CLion caricherà l'eseguibile sul dispositivo tramite ADB in `/data/local/tmp`, avvierà `lldb-server` e si collegherà automaticamente consentendoti il debug a livello di codice sorgente tramite breakpoints grafici.

### Opzione C: VS Code (Alternativa Leggera)
1. Installa le estensioni **CMake Tools**, **C/C++** e **Android NDK Debugger** in VS Code.
2. Imposta il preset attivo in VS Code su `android-ndk-arm64` (cliccando sulla barra di stato di CMake Tools).
3. Configura il file `.vscode/launch.json` inserendo una configurazione di tipo `lldb` per avviare il debug remoto adb:
   ```json
   {
       "version": "0.2.0",
       "configurations": [
           {
               "name": "Android Native Debug",
               "type": "lldb",
               "request": "launch",
               "initCommands": [
                   "platform select remote-android",
                   "platform connect connect://localhost:5039"
               ],
               "program": "${workspaceRoot}/build-android-arm64/test_webrtc_aec_unit",
               "sourceMap": {
                   ".": "${workspaceRoot}"
               }
           }
       ]
   }
   ```
4. Fai il push dell'eseguibile e di `lldb-server` sul device ed esegui il debug.

---

## 5. Sintesi comandi CLI per la generazione rapida

Se preferisci generare i progetti da terminale prima di aprirli negli IDE:

| Piattaforma Target | IDE Consigliata | Comando di Generazione | Cartella di Output |
| :--- | :--- | :--- | :--- |
| **Windows** | Visual Studio 2022 | `cmake --preset windows-vs` | `build-vs/` |
| **macOS** | Xcode | `cmake --preset macos-xcode` | `build-xcode-mac/` |
| **iOS** | Xcode | `cmake --preset ios-xcode` | `build-xcode-ios/` |
| **Linux (Host)** | CLion / VS Code | `cmake --preset linux-native` | `build-linux-native/` |
| **Android** | Android Studio / CLion | `cmake --preset android-ndk-arm64` | `build-android-arm64/` |
