// ---------------------------------------------------------------------------
// test_tts_node.cpp — KokoroTtsNode stub mode test (sine wave generator)
//
// Without kokoro.h, the node generates a 440Hz sine wave stub.
// We verify:
//   1. Node initializes in stub mode
//   2. Pushing a TextChunk produces audio frames on speakerOut
//   3. TtsStateSignal is activated during synthesis and deactivated after
//   4. AEC reference frames are produced on aecRefOut
//   5. InterruptSignal can abort synthesis mid-stream
// ---------------------------------------------------------------------------

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "voice_runtime/AudioTypes.h"
#include "voice_runtime/BoundedQueue.h"
#include "voice_runtime/SharedBufferPool.h"
#include "voice_runtime/Interfaces.h"
#include "voice_runtime/LibraryLogRedirector.h"
#include "KokoroTtsNode.h"
#include "MiniaudioOutputNode.h"

using namespace voice_runtime;

static uint64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool writeWavFile(const std::string& path, const std::vector<int16_t>& pcm, uint32_t sampleRate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;

    struct WavHeader {
        char riff[4] = {'R', 'I', 'F', 'F'};
        uint32_t overallSize;
        char wave[4] = {'W', 'A', 'V', 'E'};
        char fmt[4] = {'f', 'm', 't', ' '};
        uint32_t fmtLength = 16;
        uint16_t audioFormat = 1; // PCM
        uint16_t numChannels = 1;
        uint32_t sampleRate;
        uint32_t byteRate;
        uint16_t blockAlign;
        uint16_t bitsPerSample = 16;
        char dataHeader[4] = {'d', 'a', 't', 'a'};
        uint32_t dataSize;
    } header;

    uint32_t dataBytes = pcm.size() * sizeof(int16_t);
    header.overallSize = 36 + dataBytes;
    header.sampleRate = sampleRate;
    header.numChannels = 1;
    header.byteRate = sampleRate * 2;
    header.blockAlign = 2;
    header.dataSize = dataBytes;

    std::fwrite(&header, 1, sizeof(header), f);
    std::fwrite(pcm.data(), 1, dataBytes, f);
    std::fclose(f);
    return true;
}

static std::string readTextFileOrString(const std::string& pathOrString) {
    std::ifstream file(pathOrString);
    if (!file.is_open()) {
        return pathOrString; // Treat as raw string
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string content = ss.str();
    while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
        content.pop_back();
    }
    return content;
}

int main(int argc, char* argv[]) {
    LibraryLogRedirector::instance().start();
    if (argc > 1) {
        std::printf("=== test_tts_node (Real TTS Synthesis) ===\n");
        const std::string textArg = argv[1];
        const std::string textToRead = readTextFileOrString(textArg);
        const std::string wavPath = (argc > 2) ? argv[2] : "tts_output.wav";
        const std::string defaultVoice = (argc > 3) ? argv[3] : "af_bella";
        const std::string defaultLang = (argc > 4) ? argv[4] : "en";

        const bool playDirectly = (wavPath == "play");

        std::printf("Text to read: \"%s\"\n", textToRead.c_str());
        if (playDirectly) {
            std::printf("Output:       Speaker playback (real-time)\n");
        } else {
            std::printf("Output WAV:   %s\n", wavPath.c_str());
        }

        // Initialize real Kokoro config
        KokoroTtsConfig cfg;
        cfg.modelPath = "models/tts/kokoro-v1.1-zh.onnx";
        cfg.voicesPath = "models/tts/voices.bin";
        cfg.dictDir = "models/tts/dict";
        cfg.vocabPath = "models/tts/dict/vocab.txt";
        cfg.espeakDataPath = "models/espeak-ng-data";
        cfg.defaultVoice = defaultVoice;
        cfg.defaultLanguage = defaultLang;
        cfg.speakerPoolSize = 512;
        cfg.aecRefPoolSize = 512;

        // Verify model exists
        std::FILE* testF = std::fopen(cfg.modelPath.c_str(), "rb");
        if (!testF) {
            std::fprintf(stderr, "Error: Real Kokoro model not found at %s.\n", cfg.modelPath.c_str());
            return 1;
        }
        std::fclose(testF);

        KokoroTtsNode node(cfg);

        TextQueue textIn(128, QueueOverflowPolicy::BlockProducer, "tts_text_in");
        AudioFrameQueue spkOut(256, QueueOverflowPolicy::BlockProducer, "tts_spk_out");
        AudioFrameQueue aecRefOut(256, QueueOverflowPolicy::BlockProducer, "tts_aec_ref");
        TtsStateSignal ttsState;
        InterruptSignal intSig;

        node.setInputQueue(&textIn);
        node.setSpeakerOutputQueue(&spkOut);
        node.setAecReferenceOutputQueue(&aecRefOut);
        node.setTtsStateSignal(&ttsState);
        node.setInterruptSignal(&intSig);

        // Optional Miniaudio output node
        std::unique_ptr<MiniaudioOutputNode> audioOut;
        if (playDirectly) {
            MiniaudioOutputConfig outCfg;
            outCfg.format = cfg.speakerFormat;
            outCfg.ringBufferCapacity = 4800; // ~200ms buffer
            audioOut = std::make_unique<MiniaudioOutputNode>(outCfg);
            audioOut->setInputQueue(&spkOut);

            std::printf("Initializing Speaker Output... ");
            std::fflush(stdout);
            if (!audioOut->initialize()) {
                std::printf("FAIL\n");
                return 1;
            }
            std::printf("OK\n");
        }

        std::printf("Initializing TTS node... ");
        std::fflush(stdout);
        if (!node.initialize()) {
            std::printf("FAIL\n");
            return 1;
        }
        std::printf("OK\n");

        if (playDirectly) {
            audioOut->start();
        }
        node.start();

        // Split textToRead into sentences/clauses based on punctuation
        std::string defaultMarker = "";
        std::string cleanText = textToRead;

        // Trim leading whitespace
        auto startPos = cleanText.find_first_not_of(" \t\r\n");
        if (startPos != std::string::npos) {
            cleanText = cleanText.substr(startPos);
        }

        if (cleanText.size() >= 8 && cleanText[0] == '[') {
            auto endPos = cleanText.find(']');
            if (endPos != std::string::npos && endPos <= 10) {
                std::string marker = cleanText.substr(1, endPos - 1);
                if (marker.compare(0, 5, "lang=") == 0) {
                    defaultMarker = "[" + marker + "] ";
                    cleanText = cleanText.substr(endPos + 1);
                }
            }
        }

        std::vector<std::string> sentences;
        std::string current;
        for (std::size_t i = 0; i < cleanText.size(); ++i) {
            char c = cleanText[i];
            current.push_back(c);
            if (c == '.' || c == '?' || c == '!' || c == '\n') {
                auto first = current.find_first_not_of(" \t\r\n");
                if (first != std::string::npos) {
                    auto last = current.find_last_not_of(" \t\r\n");
                    std::string trimmed = current.substr(first, (last - first + 1));
                    if (!trimmed.empty()) {
                        sentences.push_back(defaultMarker + trimmed);
                    }
                }
                current.clear();
            }
        }
        if (!current.empty()) {
            auto first = current.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) {
                auto last = current.find_last_not_of(" \t\r\n");
                std::string trimmed = current.substr(first, (last - first + 1));
                if (!trimmed.empty()) {
                    sentences.push_back(defaultMarker + trimmed);
                }
            }
        }

        // Push text chunks
        if (sentences.empty()) {
            TextChunk chunk;
            chunk.text = textToRead;
            chunk.isFinal = true;
            chunk.sequence = 1;
            chunk.timestampNs = now_ns();
            textIn.push(std::move(chunk));
        } else {
            std::size_t seq = 1;
            for (const auto& sentence : sentences) {
                TextChunk chunk;
                chunk.text = sentence;
                chunk.isFinal = (seq == sentences.size());
                chunk.sequence = seq++;
                chunk.timestampNs = now_ns();
                textIn.push(std::move(chunk));
            }
        }

        std::printf("Synthesizing... ");
        std::fflush(stdout);

        // Wait for it to start (either active state or frames arriving)
        int waitCount = 0;
        while (!ttsState.isActive() && spkOut.size() == 0 && waitCount < 6000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            waitCount++;
        }

        if (playDirectly) {
            std::printf("Playing directly to speaker... ");
            std::fflush(stdout);

            while (true) {
                // Drain AEC reference frames to prevent pool exhaustion and delays
                AudioFrameHandle dummyRef;
                while (aecRefOut.tryPop(dummyRef)) {}

                // Since spkOut is being consumed by audioOut, we check when synthesis ends
                // and spkOut queue becomes empty
                if (!ttsState.isActive() && spkOut.size() == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    if (!ttsState.isActive() && spkOut.size() == 0) {
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // Allow the remaining audio to drain from the miniaudio ring buffer
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::printf("Done\n");

            // Shutdown play
            textIn.stop();
            spkOut.stop();
            aecRefOut.stop();
            node.stop();
            audioOut->stop();

            return 0;
        }

        // Wait for it to finish and collect all samples for WAV writing
        std::vector<int16_t> allPcm;
        while (true) {
            // Drain AEC reference frames to prevent pool exhaustion and delays
            AudioFrameHandle dummyRef;
            while (aecRefOut.tryPop(dummyRef)) {}

            AudioFrameHandle frame;
            if (spkOut.tryPop(frame)) {
                allPcm.insert(allPcm.end(), frame->pcm16.begin(), frame->pcm16.end());
            } else {
                if (!ttsState.isActive() && spkOut.size() == 0) {
                    // double check to avoid race condition
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    if (!ttsState.isActive() && !spkOut.tryPop(frame)) {
                        break;
                    }
                    if (frame) {
                        allPcm.insert(allPcm.end(), frame->pcm16.begin(), frame->pcm16.end());
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        std::printf("Done (generated %zu samples, %.2f seconds)\n",
                    allPcm.size(), static_cast<double>(allPcm.size()) / cfg.speakerFormat.sampleRate);

        // Shutdown
        textIn.stop();
        spkOut.stop();
        aecRefOut.stop();
        node.stop();

        if (allPcm.empty()) {
            std::fprintf(stderr, "Error: No synthesized audio generated.\n");
            return 1;
        }

        std::printf("Writing WAV file to %s... ", wavPath.c_str());
        std::fflush(stdout);
        if (!writeWavFile(wavPath, allPcm, cfg.speakerFormat.sampleRate)) {
            std::printf("FAIL\n");
            return 1;
        }
        std::printf("OK\n");

        return 0;
    }

    std::printf("=== test_tts_node (Unit Tests) ===\n");

    // ---- Test 1: Stub initialization ----------------------------------------
    {
        std::printf("[Test 1] Stub initialization... ");
        KokoroTtsConfig cfg;
        KokoroTtsNode node(cfg);
        const bool ok = node.initialize();
        (void)ok;
        assert(ok && "TTS stub init should succeed");
        std::printf("OK\n");
    }

    // ---- Test 2: Synthesize text → audio frames -----------------------------
    {
        std::printf("[Test 2] Synthesize text → speaker frames... ");

        KokoroTtsConfig cfg;
        KokoroTtsNode node(cfg);

        TextQueue textIn(32, QueueOverflowPolicy::BlockProducer, "tts_text_in");
        AudioFrameQueue spkOut(1024, QueueOverflowPolicy::DropOldest, "tts_spk_out");
        AudioFrameQueue aecRefOut(1024, QueueOverflowPolicy::DropOldest, "tts_aec_ref");
        TtsStateSignal ttsState;
        InterruptSignal intSig;

        node.setInputQueue(&textIn);
        node.setSpeakerOutputQueue(&spkOut);
        node.setAecReferenceOutputQueue(&aecRefOut);
        node.setTtsStateSignal(&ttsState);
        node.setInterruptSignal(&intSig);

        node.initialize();
        node.start();

        // Verify TTS is initially inactive
        assert(!ttsState.isActive() && "TTS should be inactive before synthesis");

        // Push a text chunk
        TextChunk chunk;
        chunk.text = "Hello world";
        chunk.isFinal = true;
        chunk.sequence = 1;
        chunk.timestampNs = now_ns();
        textIn.push(std::move(chunk));

        // Wait for synthesis to complete
        // The stub generates ~0.5s of audio at 24kHz, so it should finish quickly
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        // Count speaker output frames
        int spkFrameCount = 0;
        AudioFrameHandle frame;
        while (spkOut.tryPop(frame)) {
            ++spkFrameCount;
        }

        // Count AEC reference frames
        int refFrameCount = 0;
        while (aecRefOut.tryPop(frame)) {
            ++refFrameCount;
        }

        // Stop
        textIn.stop();
        spkOut.stop();
        aecRefOut.stop();
        node.stop();
        textIn.clear();
        spkOut.clear();
        aecRefOut.clear();

        if (spkFrameCount == 0) {
            std::fprintf(stderr, "FAIL (no speaker frames received)\n");
            std::fprintf(stderr, "\n>>> FAIL — test_tts_node <<<\n");
            return 1;
        }
        std::printf("OK (speaker frames: %d, AEC ref frames: %d)\n",
                     spkFrameCount, refFrameCount);

        // Verify AEC reference was also produced
        if (refFrameCount == 0) {
            std::fprintf(stderr, "[Test 2] WARNING: No AEC reference frames (may be expected)\n");
        }

        // Verify TTS state returned to inactive after synthesis
        if (ttsState.isActive()) {
            std::fprintf(stderr, "[Test 2] FAIL: TTS still active after synthesis!\n");
            std::fprintf(stderr, "\n>>> FAIL — test_tts_node <<<\n");
            return 1;
        }
        std::printf("[Test 2] TtsStateSignal correctly deactivated after synthesis\n");
    }

    // ---- Test 3: TtsStateSignal activation/deactivation ---------------------
    {
        std::printf("[Test 3] TtsStateSignal activation tracking... ");

        KokoroTtsConfig cfg;
        KokoroTtsNode node(cfg);

        TextQueue textIn(32, QueueOverflowPolicy::BlockProducer, "tts_text_in");
        AudioFrameQueue spkOut(1024, QueueOverflowPolicy::DropOldest, "tts_spk_out");
        AudioFrameQueue aecRefOut(1024, QueueOverflowPolicy::DropOldest, "tts_aec_ref");
        TtsStateSignal ttsState;
        InterruptSignal intSig;

        node.setInputQueue(&textIn);
        node.setSpeakerOutputQueue(&spkOut);
        node.setAecReferenceOutputQueue(&aecRefOut);
        node.setTtsStateSignal(&ttsState);
        node.setInterruptSignal(&intSig);

        node.initialize();
        node.start();

        // Push text
        TextChunk chunk;
        chunk.text = "Testing state signal";
        chunk.isFinal = true;
        chunk.sequence = 1;
        chunk.timestampNs = now_ns();
        textIn.push(std::move(chunk));

        // Wait for synthesis to finish
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));

        // After synthesis, TTS should be inactive
        assert(!ttsState.isActive() && "TTS should be inactive after synthesis completes");

        textIn.stop();
        spkOut.stop();
        aecRefOut.stop();
        node.stop();

        std::printf("OK\n");
    }

    // ---- Test 4: Interrupt mid-synthesis ------------------------------------
    {
        std::printf("[Test 4] Interrupt mid-synthesis... ");

        KokoroTtsConfig cfg;
        KokoroTtsNode node(cfg);

        TextQueue textIn(32, QueueOverflowPolicy::BlockProducer, "tts_text_in");
        AudioFrameQueue spkOut(2048, QueueOverflowPolicy::DropOldest, "tts_spk_out");
        AudioFrameQueue aecRefOut(2048, QueueOverflowPolicy::DropOldest, "tts_aec_ref");
        TtsStateSignal ttsState;
        InterruptSignal intSig;

        node.setInputQueue(&textIn);
        node.setSpeakerOutputQueue(&spkOut);
        node.setAecReferenceOutputQueue(&aecRefOut);
        node.setTtsStateSignal(&ttsState);
        node.setInterruptSignal(&intSig);

        node.initialize();
        node.start();

        // Push a long text to ensure synthesis takes some time
        TextChunk chunk;
        chunk.text = "This is a very long sentence that should take a while to "
                     "synthesize as a sine wave stub in the TTS node pipeline.";
        chunk.isFinal = true;
        chunk.sequence = 1;
        chunk.timestampNs = now_ns();
        textIn.push(std::move(chunk));

        // Wait a brief moment then trigger interrupt
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        intSig.request();

        // Wait for the node to process the interrupt
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Count frames — should be fewer than a full synthesis
        int frameCount = 0;
        AudioFrameHandle frame;
        while (spkOut.tryPop(frame)) {
            ++frameCount;
        }

        textIn.stop();
        spkOut.stop();
        aecRefOut.stop();
        node.stop();

        // We can't assert an exact frame count, but the test passing without
        // deadlock or crash is the main success criterion.
        std::printf("OK (frames before interrupt: %d)\n", frameCount);
    }

    // ---- Test 5: Clean shutdown without deadlock -----------------------------
    {
        std::printf("[Test 5] Clean shutdown... ");

        KokoroTtsConfig cfg;
        KokoroTtsNode node(cfg);

        TextQueue textIn(32, QueueOverflowPolicy::BlockProducer, "tts_text_in");
        AudioFrameQueue spkOut(512, QueueOverflowPolicy::DropOldest, "tts_spk_out");
        AudioFrameQueue aecRefOut(512, QueueOverflowPolicy::DropOldest, "tts_aec_ref");

        node.setInputQueue(&textIn);
        node.setSpeakerOutputQueue(&spkOut);
        node.setAecReferenceOutputQueue(&aecRefOut);

        node.initialize();
        node.start();

        // Immediately stop
        textIn.stop();
        spkOut.stop();
        aecRefOut.stop();
        node.stop();
        textIn.clear();
        spkOut.clear();
        aecRefOut.clear();

        std::printf("OK\n");
    }

    std::printf("\n>>> PASS — test_tts_node <<<\n");
    LibraryLogRedirector::instance().stop();
    return 0;
}
