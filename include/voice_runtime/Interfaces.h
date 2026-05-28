#pragma once

#include <memory>
#include <string>
#include "ActiveNode.h"
#include "AudioTypes.h"
#include "BoundedQueue.h"
#include "TextBuffer.h"

namespace voice_runtime {

// ---------------------------------------------------------------------------
// Queue type aliases
// ---------------------------------------------------------------------------

using AudioFrameQueue = BoundedQueue<AudioFrameHandle>;
using TextQueue       = BoundedQueue<TextChunk>;
using VadEventQueue   = BoundedQueue<VadEvent>;
using BargeInQueue    = BoundedQueue<BargeInEvent>;

// ---------------------------------------------------------------------------
// IMicrophoneNode
// ---------------------------------------------------------------------------

class IMicrophoneNode : public virtual IActiveNode {
public:
    virtual void setOutputQueue(AudioFrameQueue* micOut) = 0;
};

// ---------------------------------------------------------------------------
// IAudioOutputNode
// ---------------------------------------------------------------------------

class IAudioOutputNode : public virtual IActiveNode {
public:
    virtual void setInputQueue(AudioFrameQueue* speakerIn) = 0;
};

// ---------------------------------------------------------------------------
// IAudioFormatAdapterNode
//   Inserito nella pipeline esclusivamente quando i formati differiscono.
//   Se sourceFormat == targetFormat il nodo opera in pass-through zero-copy.
// ---------------------------------------------------------------------------

class IAudioFormatAdapterNode : public virtual IActiveNode {
public:
    virtual void setInputQueue(AudioFrameQueue* in)   = 0;
    virtual void setOutputQueue(AudioFrameQueue* out) = 0;

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
    virtual void setInputQueue(AudioFrameQueue* in)          = 0;
    virtual void setOutputQueue(AudioFrameQueue* gatedOut)   = 0;
    virtual void setEventQueue(VadEventQueue* events)        = 0;

    // Livello di confidenza minimo per considerare un frame come speech [0,1]
    virtual void setSpeechThreshold(float threshold)         = 0;
};

// ---------------------------------------------------------------------------
// IAecDspNode — Acoustic Echo Cancellation / DSP
// ---------------------------------------------------------------------------

class IAecDspNode : public virtual IActiveNode {
public:
    virtual void setCaptureInputQueue(AudioFrameQueue* micIn)    = 0;
    virtual void setRenderInputQueue(AudioFrameQueue* ttsRefIn)  = 0;
    virtual void setOutputQueue(AudioFrameQueue* cleanOut)       = 0;

    // Opzionale: alcuni DSP/AEC possono produrre direttamente eventi VAD
    // dopo AEC/NS/AGC. Default no-op per mantenere compatibili i nodi esistenti.
    virtual void setVadEventQueue(VadEventQueue* events)         { (void)events; }
    virtual void setSpeechThreshold(float threshold)             { (void)threshold; }
    virtual bool isSpeaking() const                              { return false; }
};

// ---------------------------------------------------------------------------
// ISpeechToTextNode
// ---------------------------------------------------------------------------

class ISpeechToTextNode : public virtual IActiveNode {
public:
    virtual void setInputQueue(AudioFrameQueue* cleanAudioIn) = 0;
    virtual void setOutputQueue(TextQueue* textOut)           = 0;
};

// ---------------------------------------------------------------------------
// IBargeInContextNode
//   Analizza il testo STT in ingresso e decide se triggare un interrupt.
//   Supporta anche interrupt manuale esterno (es. tastiera).
// ---------------------------------------------------------------------------

class IBargeInContextNode : public virtual IActiveNode {
public:
    virtual void setTextInputQueue(TextQueue* sttText)           = 0;
    virtual void setTextOutputQueue(TextQueue* llmText)          = 0;
    virtual void setBargeInEventQueue(BargeInQueue* events)      = 0;
    virtual void setTtsInterruptSignal(InterruptSignal* signal)  = 0;
    virtual void setLlmInterruptSignal(InterruptSignal* signal)  = 0;

    // Trigger manuale (tastiera, UI, test)
    virtual void triggerManualBargeIn()                          = 0;
};

// ---------------------------------------------------------------------------
// ILanguageModelNode
// ---------------------------------------------------------------------------

class ILanguageModelNode : public virtual IActiveNode {
public:
    virtual void setInputQueue(TextQueue* textIn)                     = 0;
    virtual void setOutputQueue(TextQueue* textOut)                   = 0;
    virtual void setPersistentInputBuffer(DiskBackedTextBuffer* buf)  = 0;
    virtual void setInterruptSignal(InterruptSignal* signal)          = 0;
};

// ---------------------------------------------------------------------------
// ITextToSpeechNode
// ---------------------------------------------------------------------------

class ITextToSpeechNode : public virtual IActiveNode {
public:
    virtual void setInputQueue(TextQueue* textIn)                          = 0;
    virtual void setSpeakerOutputQueue(AudioFrameQueue* speakerOut)        = 0;
    virtual void setAecReferenceOutputQueue(AudioFrameQueue* aecRefOut)    = 0;
    virtual void setInterruptSignal(InterruptSignal* signal)               = 0;
};

} // namespace voice_runtime
