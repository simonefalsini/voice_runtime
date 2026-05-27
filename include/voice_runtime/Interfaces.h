#pragma once

#include <memory>
#include <string>
#include "ActiveNode.h"
#include "AudioTypes.h"
#include "BoundedQueue.h"
#include "TextBuffer.h"

namespace voice_runtime {

using AudioFrameQueue = BoundedQueue<AudioFrameHandle>;
using TextQueue = BoundedQueue<TextChunk>;

class IMicrophoneNode : public virtual IActiveNode {
public:
    virtual void setOutputQueue(AudioFrameQueue* micOut) = 0;
};

class IAudioOutputNode : public virtual IActiveNode {
public:
    virtual void setInputQueue(AudioFrameQueue* speakerIn) = 0;
};

class IAecDspNode : public virtual IActiveNode {
public:
    virtual void setCaptureInputQueue(AudioFrameQueue* micIn) = 0;
    virtual void setRenderInputQueue(AudioFrameQueue* ttsRefIn) = 0;
    virtual void setOutputQueue(AudioFrameQueue* cleanOut) = 0;
};

class ISpeechToTextNode : public virtual IActiveNode {
public:
    virtual void setInputQueue(AudioFrameQueue* cleanAudioIn) = 0;
    virtual void setOutputQueue(TextQueue* textOut) = 0;
};

class ILanguageModelNode : public virtual IActiveNode {
public:
    virtual void setInputQueue(TextQueue* textIn) = 0;
    virtual void setOutputQueue(TextQueue* textOut) = 0;
    virtual void setPersistentInputBuffer(DiskBackedTextBuffer* buffer) = 0;
};

class ITextToSpeechNode : public virtual IActiveNode {
public:
    virtual void setInputQueue(TextQueue* textIn) = 0;
    virtual void setSpeakerOutputQueue(AudioFrameQueue* speakerOut) = 0;
    virtual void setAecReferenceOutputQueue(AudioFrameQueue* aecRefOut) = 0;
};

} // namespace voice_runtime
