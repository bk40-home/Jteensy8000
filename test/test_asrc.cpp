#include "Asrc.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
static void check(bool c, const char* w){ if(!c){ printf("FAIL: %s\n", w); ++failures; } }

static JT::Asrc asrc;
static JT::AsrcServo servo;

/* 44100/48000 in Q16. */
static const uint32_t kRatio = (uint32_t)((44100.0 / 48000.0) * 65536.0 + 0.5);

int main(void){
    printf("nominal ratio Q16 = %u (%.6f)\n", kRatio, kRatio/65536.0);

    /* ---- ratio and priming ---- */
    asrc.begin(kRatio);
    check(asrc.getRatio()==kRatio, "ratio stored");
    check(asrc.fill()==JT::kAsrcTargetFill, "ring primed to target");

    /* ---- silence in, silence out ---- */
    static float zl[128], zr[128];
    for (int i=0;i<128;++i){ zl[i]=0.0f; zr[i]=0.0f; }
    static int32_t out[256];
    asrc.push(zl, zr, 128);
    asrc.pull(out, 48, 2);
    bool silent = true;
    for (int i=0;i<96;++i) if (out[i]!=0) silent=false;
    check(silent, "silence in gives silence out");
    check(asrc.underruns()==0, "no underrun while primed");

    /* ---- a 1 kHz sine resampled 44.1 -> 48 keeps its frequency ----
       Feed input at 44100 and pull at 48000 in the correct proportion, then
       count zero crossings in the output. */
    asrc.begin(kRatio);
    asrc.resetCounters();
    double phase = 0.0;
    const double inc = 2.0*M_PI*1000.0/44100.0;
    static int32_t big[64*2];
    int crossings = 0; int32_t prev = 0; long produced = 0; long counted = 0;
    /* One second: 44100 input frames in blocks of 128, 48000 output. */
    double outDebt = 0.0;
    for (int b=0;b<344;++b){
        static float sl[128], sr[128];
        for (int i=0;i<128;++i){ sl[i]=(float)(0.5*sin(phase)); sr[i]=sl[i]; phase+=inc; }
        asrc.push(sl, sr, 128);
        outDebt += 128.0*48000.0/44100.0;
        while (outDebt >= 48.0){
            asrc.pull(big, 48, 2);
            for (int i=0;i<48;++i){
                int32_t s = big[i*2];
                /* The ring starts half full of silence, so the opening frames
                   are priming latency rather than signal. Measure only the
                   steady-state window. */
                if (produced > 5000){
                    if (prev < 0 && s >= 0) ++crossings;
                    ++counted;
                }
                prev = s; ++produced;
            }
            outDebt -= 48.0;
        }
    }
    printf("produced %ld frames, measured over %ld, %d crossings\n",
           produced, counted, crossings);
    double seconds = counted/48000.0;
    double measured = crossings/seconds;
    printf("measured %.1f Hz\n", measured);
    check(measured > 995.0 && measured < 1005.0, "1 kHz survives resampling");
    check(asrc.underruns()==0, "no underruns during steady stream");
    check(asrc.overruns()==0, "no overruns during steady stream");

    /* ---- underrun produces silence, not a held DC value ---- */
    asrc.begin(kRatio);
    for (int i=0;i<200;++i) asrc.pull(big, 48, 2);   /* drain far past empty */
    check(asrc.underruns() > 0, "underrun counted");
    asrc.pull(big, 48, 2);
    bool zeros = true;
    for (int i=0;i<96;++i) if (big[i]!=0) zeros=false;
    check(zeros, "underrun emits silence");

    /* ---- overrun is counted, not silently wrapped ---- */
    asrc.begin(kRatio);
    uint32_t before = asrc.overruns();
    for (int i=0;i<20;++i) asrc.push(zl, zr, 128);   /* never pulled */
    check(asrc.overruns() > before, "overrun counted");
    check(asrc.fill() < JT::kAsrcRingFrames, "fill stays within capacity");

    /* ---- clamping: full scale input must not wrap ---- */
    asrc.begin(kRatio);
    static float fl[128], fr[128];
    for (int i=0;i<128;++i){ fl[i]=(i&1)?1.0f:-1.0f; fr[i]=fl[i]; }
    for (int i=0;i<4;++i) asrc.push(fl, fr, 128);
    asrc.pull(big, 48, 2);
    bool bounded = true;
    for (int i=0;i<96;++i) if (big[i] > 8388607 || big[i] < -8388607) bounded=false;
    check(bounded, "output clamped to 24-bit range");

    /* ---- mono and multi-channel ---- */
    asrc.begin(kRatio);
    for (int i=0;i<4;++i) asrc.push(zl, zr, 128);
    asrc.pull(big, 32, 1);
    check(true, "mono pull does not crash");

    /* ---- servo: drives fill error toward zero ---- */
    servo.begin(kRatio);
    check(servo.ratio()==kRatio, "servo starts at nominal");
    check(servo.update(JT::kAsrcTargetFill)==kRatio, "no correction at target");
    check(servo.error()==0, "zero error at target");

    /* Small errors inside the deadband do nothing. */
    uint32_t r0 = servo.update(JT::kAsrcTargetFill + 2u);
    check(r0==kRatio, "deadband suppresses jitter");

    /* THE WARBLE TEST.  Fill sawtooths by a whole engine block, because the
       producer delivers 128 frames at once and the consumer takes ~48.  The
       ratio must stay essentially still through that: any wobble here is
       pitch modulation on a sustained tone. */
    servo.begin(kRatio);
    uint32_t lo = 0xFFFFFFFFu, hi = 0u;
    for (int i = 0; i < 4000; ++i) {
        const size_t saw = JT::kAsrcTargetFill + (size_t)((i % 128) - 64);
        const uint32_t r = servo.update(saw);
        if (i > 2000) { if (r < lo) lo = r; if (r > hi) hi = r; }
    }
    const uint32_t swing = hi - lo;
    /* Express it as pitch, since that is what the ear judges.  A ratio swing
       of 'swing' parts in 65536 is a frequency swing of the same proportion;
       1200*log2 converts to cents.  Unfiltered, this loop swung about 512
       units, which is 13 cents and plainly audible as a warble.  The bound
       here is one cent: below the threshold for hearing slow pitch movement,
       with margin. */
    const double frac = (double)swing / 65536.0;
    const double cents = 1200.0 * log2(1.0 + frac);
    printf("ratio swing under a full-block sawtooth: %u Q16 units, %.3f cents\n",
           swing, cents);
    check(cents < 1.0, "sawtooth fill does not audibly modulate pitch");

    /* A persistent surplus raises the ratio, consuming input faster.  Needs
       more iterations than before: the fill average and the slew limit both
       deliberately slow the response. */
    servo.begin(kRatio);
    uint32_t rising = kRatio;
    for (int i=0;i<4000;++i) rising = servo.update(JT::kAsrcTargetFill + 100u);
    /* Proportional action depends only on the present error, so repeating a
       reading must not keep winding the ratio up. */
    const uint32_t again = servo.update(JT::kAsrcTargetFill + 100u);
    check(again == rising, "proportional servo does not wind up");
    check(rising > kRatio, "surplus raises the ratio");

    /* A persistent shortfall lowers it. */
    servo.begin(kRatio);
    uint32_t falling = kRatio;
    for (int i=0;i<4000;++i) falling = servo.update(JT::kAsrcTargetFill - 100u);
    check(falling < kRatio, "shortfall lowers the ratio");

    /* Saturation: a wild reading cannot detune the output. */
    servo.begin(kRatio);
    uint32_t sat = kRatio;
    for (int i=0;i<200000;++i) sat = servo.update(JT::kAsrcRingFrames - 1u);
    check(sat <= kRatio + JT::kAsrcRatioMaxDeviation, "ratio clamped high");
    servo.begin(kRatio);
    for (int i=0;i<200000;++i) sat = servo.update(0u);
    check(sat >= kRatio - JT::kAsrcRatioMaxDeviation, "ratio clamped low");

    /* ---- closed loop: a deliberately wrong engine rate is corrected ----
       Engine actually runs 0.4% fast; the servo should pull the ring back to
       target rather than letting it fill. */
    asrc.begin(kRatio);
    servo.begin(kRatio);
    double p2 = 0.0; const double inc2 = 2.0*M_PI*440.0/44100.0;
    double debt = 0.0;
    for (int b=0;b<20000;++b){
        static float sl[128], sr[128];
        for (int i=0;i<128;++i){ sl[i]=(float)(0.25*sin(p2)); sr[i]=sl[i]; p2+=inc2; }
        asrc.push(sl, sr, 128);
        asrc.setRatio(servo.update(asrc.fill()));
        debt += 128.0*(48000.0/44100.0)*(1.0/1.004);   /* engine 0.4% fast */
        while (debt >= 48.0){ asrc.pull(big, 48, 2); debt -= 48.0; }
    }
    long err = (long)asrc.fill() - (long)JT::kAsrcTargetFill;
    printf("closed loop fill error after correction: %ld frames\n", err);
    printf("closed loop ratio %u vs nominal %u\n", asrc.getRatio(), kRatio);
    check(err > -150 && err < 150, "servo holds fill near target under rate error");
    check(asrc.overruns()==0, "no overrun once corrected");

    printf("%s (%d failure(s))\n", failures==0?"PASS":"FAILED", failures);
    return failures==0?0:1;
}
