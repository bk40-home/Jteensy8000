// Host-only stub of the OpenAudio base class — JUST enough surface to
// syntax-check platform/AudioSynthBlockF32.cpp off-target.  Never shipped.
#pragma once
struct audio_block_f32_t { float data[128]; };
class AudioStream_F32 {
public:
    virtual ~AudioStream_F32() {}
    virtual void update() = 0;
protected:
    AudioStream_F32(int, audio_block_f32_t**) {}
    static audio_block_f32_t* allocate_f32() { return nullptr; }
    static void transmit(audio_block_f32_t*, int) {}
    static void release(audio_block_f32_t*) {}
};
