#pragma once
#include <Audio.h>
#include <arm_math.h>  // fabsf

// ============================================================================
// AudioScopeTap: lightweight oscilloscope "tap"
// ----------------------------------------------------------------------------
// - Always-on: no begin()/end(), no AudioRecordQueue
// - Acts as a normal AudioStream sink (1 input, 0 outputs), so the graph
//   is reliably "pulled" without affecting your existing outputs.
// - Writes incoming audio to a circular buffer in update().
// - UI calls snapshot() to copy recent samples for drawing.
// - No extra AudioMemory held beyond normal block flow.
// ============================================================================

class AudioScopeTap : public AudioStream {
public:
    // Use a power-of-two for cheap wrapping. 1024 is enough for a 128px OLED.
    static constexpr uint16_t RING_LEN = 1024;

    AudioScopeTap() : AudioStream(1, _inputQueue) {
        _writeIdx = 0;
        _wrapped  = false;
        _peak     = 0.0f;
    }

    // Copy the most recent 'count' samples into dst (<= RING_LEN).
    // Returns the actual number of samples copied.
    uint16_t snapshot(int16_t* dst, uint16_t count) {
        if (count > RING_LEN) count = RING_LEN;

        const uint16_t w = _writeIdx;  // volatile snapshot
        const bool     ready = _wrapped;

        if (!ready) {
            // Ring hasn’t wrapped yet — copy up to 'w'
            const uint16_t n = (count > w) ? w : count;
            if (n) memcpy(dst, _ring, n * sizeof(int16_t));
            return n;
        }

        // Ring is full: copy the last 'count' samples ending at w-1
        uint16_t n = count;
        uint16_t start = (w + RING_LEN - n) & (RING_LEN - 1);

        if (start + n <= RING_LEN) {
            memcpy(dst, &_ring[start], n * sizeof(int16_t));
        } else {
            const uint16_t first = RING_LEN - start;
            memcpy(dst, &_ring[start], first * sizeof(int16_t));
            memcpy(dst + first, &_ring[0], (n - first) * sizeof(int16_t));
        }
        return n;
    }

    // Optional simple peak meter helper.
    float readPeakAndClear() {
        noInterrupts();
        float p = _peak;
        _peak = 0.0f;
        interrupts();
        return p;
    }

protected:
    // Teensy Audio callback — runs at the audio block rate
    void update() override {
        audio_block_t* block = receiveReadOnly(0);
        if (!block) return;

        const int16_t* src = block->data;
        for (uint16_t i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const int16_t s = src[i];
            _ring[_writeIdx] = s;
            _writeIdx = (_writeIdx + 1) & (RING_LEN - 1);

            // cheap peak track (float for convenience)
            const float af = fabsf((float)s) * (1.0f / 32768.0f);
            if (af > _peak) _peak = af;
        }

        if (!_wrapped && _writeIdx == 0) _wrapped = true; // ring completed at least once
        release(block);
    }

private:
    audio_block_t* _inputQueue[1];
    volatile uint16_t _writeIdx;
    volatile bool     _wrapped;
    volatile float    _peak;
    int16_t           _ring[RING_LEN];
};
  