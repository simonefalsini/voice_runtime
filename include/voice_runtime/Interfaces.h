#pragma once

#include "ActiveNode.h"
#include "AudioTypes.h"
#include "BoundedQueue.h"
#include "TextBuffer.h"
#include <memory>
#include <mutex>
#include <string>

namespace voice_runtime {

inline std::mutex g_ggml_mutex;

// ---------------------------------------------------------------------------
// Queue type aliases
// ---------------------------------------------------------------------------

using AudioFrameQueue = BoundedQueue<AudioFrameHandle>;
using TextQueue = BoundedQueue<TextChunk>;
using VadEventQueue = BoundedQueue<VadEvent>;
using BargeInQueue = BoundedQueue<BargeInEvent>;

// ---------------------------------------------------------------------------
// IMicrophoneNode
// ---------------------------------------------------------------------------

class IMicrophoneNode : public virtual IActiveNode {
public:
  virtual void setOutputQueue(AudioFrameQueue *micOut) = 0;
};

// ---------------------------------------------------------------------------
// IAudioOutputNode
// ---------------------------------------------------------------------------

class IAudioOutputNode : public virtual IActiveNode {
public:
  virtual void setInputQueue(AudioFrameQueue *speakerIn) = 0;
};

// ---------------------------------------------------------------------------
// IAudioFormatAdapterNode
//   Inserito nella pipeline esclusivamente quando i formati differiscono.
//   Se sourceFormat == targetFormat il nodo opera in pass-through zero-copy.
// ---------------------------------------------------------------------------

class IAudioFormatAdapterNode : public virtual IActiveNode {
public:
  virtual void setInputQueue(AudioFrameQueue *in) = 0;
  virtual void setOutputQueue(AudioFrameQueue *out) = 0;

  // Ritorna true se il nodo è in modalità pass-through (nessuna conversione)
  virtual bool isPassThrough() const = 0;
};

// ---------------------------------------------------------------------------
// IVadNode — Voice Activity Detection
//   Output: frame gated (solo durante speech)
//   Events: SpeechStart / SpeechEnd
// ---------------------------------------------------------------------------

class IVadNode : public virtual IActiveNode {
public:
  virtual void setInputQueue(AudioFrameQueue *in) = 0;
  virtual void setOutputQueue(AudioFrameQueue *gatedOut) = 0;
  virtual void setEventQueue(VadEventQueue *events) = 0;

  // Livello di confidenza minimo per considerare un frame come speech [0,1]
  virtual void setSpeechThreshold(float threshold) = 0;

  // Quando il TTS è attivo, il VAD opera in pass-through (no gating)
  virtual void setTtsStateSignal(const TtsStateSignal *signal) { (void)signal; }
  // Quando il ECS è attivo, il VAD opera in pass-through (no gating)
  virtual void setACRStateSignal(const TtsStateSignal *signal) { (void)signal; }
};

// ---------------------------------------------------------------------------
// IAecDspNode — Acoustic Echo Cancellation / DSP
// ---------------------------------------------------------------------------

class IAecDspNode : public virtual IActiveNode {
public:
  virtual void setCaptureInputQueue(AudioFrameQueue *micIn) = 0;
  virtual void setRenderInputQueue(AudioFrameQueue *ttsRefIn) = 0;
  virtual void setOutputQueue(AudioFrameQueue *cleanOut) = 0;

  // Opzionale: alcuni DSP/AEC possono produrre direttamente eventi VAD
  // dopo AEC/NS/AGC. Default no-op per mantenere compatibili i nodi esistenti.
  virtual void setVadEventQueue(VadEventQueue *events) { (void)events; }
  virtual void setSpeechThreshold(float threshold) { (void)threshold; }
  virtual bool isSpeaking() const { return false; }

  // Quando il TTS è attivo, l'AEC processa; quando inattivo, pass-through
  virtual void setTtsStateSignal(const TtsStateSignal *signal) { (void)signal; }
  // Quando il AEC è attivo, il VAD esterno deve essere inattivo, pass-through
  virtual void setACRStateSignal(TtsStateSignal *signal) { (void)signal; }
};

// ---------------------------------------------------------------------------
// ISpeechToTextNode
// ---------------------------------------------------------------------------

class ISpeechToTextNode : public virtual IActiveNode {
public:
  virtual void setInputQueue(AudioFrameQueue *cleanAudioIn) = 0;
  virtual void setOutputQueue(TextQueue *textOut) = 0;
  virtual void setInterruptSignal(InterruptSignal *signal) { (void)signal; }
};

// ---------------------------------------------------------------------------
// IClassifierBargeInNode
//   Quando TTS inattivo: pass-through (forwarda testo a LLM, nessun barge-in)
//   Quando TTS attivo: classifica intent (stop→barge-in, commento→buffer)
// ---------------------------------------------------------------------------

class IClassifierBargeInNode : public virtual IActiveNode {
public:
  virtual void setTextInputQueue(TextQueue *sttText) = 0;
  virtual void setTextOutputQueue(TextQueue *llmText) = 0;
  virtual void setBargeInEventQueue(BargeInQueue *events) = 0;
  virtual void setTtsInterruptSignal(InterruptSignal *signal) = 0;
  virtual void setLlmInterruptSignal(InterruptSignal *signal) = 0;
  virtual void setTtsStateSignal(const TtsStateSignal *signal) = 0;
  virtual void setOverlapBuffer(RollingTextBuffer *buf) = 0;

  // Barge-in manuale (tastiera, UI) — attivo solo durante TTS
  virtual void triggerManualBargeIn() = 0;
};

// ---------------------------------------------------------------------------
// ILanguageModelNode
// ---------------------------------------------------------------------------

class ILanguageModelNode : public virtual IActiveNode {
public:
  virtual void setInputQueue(TextQueue *textIn) = 0;
  virtual void setOutputQueue(TextQueue *textOut) = 0;
  virtual void setPersistentInputBuffer(DiskBackedTextBuffer *buf) = 0;
  virtual void setInterruptSignal(InterruptSignal *signal) = 0;
};

// ---------------------------------------------------------------------------
// IInterpreterNode
//   Decodifica output LLM ed estrae solo il testo da leggere ad alta voce.
//   Filtra metadata, comandi, istruzioni interne.
// ---------------------------------------------------------------------------

class IInterpreterNode : public virtual IActiveNode {
public:
  virtual void setInputQueue(TextQueue *llmOutput) = 0;
  virtual void setOutputQueue(TextQueue *ttsInput) = 0;
};

// ---------------------------------------------------------------------------
// ITextToSpeechNode
// ---------------------------------------------------------------------------

class ITextToSpeechNode : public virtual IActiveNode {
public:
  virtual void setInputQueue(TextQueue *textIn) = 0;
  virtual void setSpeakerOutputQueue(AudioFrameQueue *speakerOut) = 0;
  virtual void setAecReferenceOutputQueue(AudioFrameQueue *aecRefOut) = 0;
  virtual void setInterruptSignal(InterruptSignal *signal) = 0;
  virtual void setTtsStateSignal(TtsStateSignal *signal) = 0;
};

} // namespace voice_runtime
