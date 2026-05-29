// ---------------------------------------------------------------------------
// espeak_speechplayer_stub.c
//
// Stub implementations for espeak-ng's optional Klatt synthesizer dependency.
// espeak-ng's sPlayer.c references these symbols but they are only needed
// for hardware speech synthesis, not for G2P/phoneme conversion.
// ---------------------------------------------------------------------------

#include <stddef.h>

typedef void* speechPlayer_handle_t;
typedef struct { int dummy; } speechPlayer_frame_t;

speechPlayer_handle_t speechPlayer_initialize(int sampleRate) {
    (void)sampleRate;
    return NULL;
}

int speechPlayer_queueFrame(speechPlayer_handle_t handle,
                            speechPlayer_frame_t* frame,
                            int samples,
                            int fade) {
    (void)handle; (void)frame; (void)samples; (void)fade;
    return 0;
}

int speechPlayer_synthesize(speechPlayer_handle_t handle, int samples, short* buf) {
    (void)handle; (void)samples; (void)buf;
    return 0;
}

void speechPlayer_terminate(speechPlayer_handle_t handle) {
    (void)handle;
}
