#include <Arduino.h>
#include "Audio.h"
#include "AudioFilterMoogLadderLinear.h"

void AudioFilterMoogLadderLinear::update(void)
{
  audio_block_t *in  = receiveReadOnly(0);
  audio_block_t *mcf = receiveReadOnly(1);  // cutoff mod
  audio_block_t *mrs = receiveReadOnly(2);  // resonance mod
  if (!in) { if (mcf) release(mcf); if (mrs) release(mrs); return; }
  audio_block_t *out = allocate();
  if (!out) { release(in); if (mcf) release(mcf); if (mrs) release(mrs); return; }

  const float fs   = _fs;
  const float aCut = cutoffAlpha();

  float s1=_s1, s2=_s2, s3=_s3, s4=_s4;
  float y1=_y1, y2=_y2, y3=_y3, y4=_y4;

  // per-block
  const float dcAlpha     = 1.0f - expf(-2.0f * PI * 5.0f   / fs);
  const float envAttack   = 1.0f - expf(-2.0f * PI * 300.0f / fs);
  const float envRelease  = 1.0f - expf(-2.0f * PI * 10.0f  / fs);
  const bool hasCutMod = (mcf != nullptr) && (_modOct != 0.0f);
  const bool hasResMod = (mrs != nullptr) && (_resModDepth != 0.0f);


  for (int i=0; i<AUDIO_BLOCK_SAMPLES; ++i) {
    const float x = in->data[i] * (1.0f/32768.0f);
    // inside loop
    _fc += aCut * (_fcTarget - _fc);
    float fcBase = _fc;
    float fcInst = hasCutMod ? (fcBase * exp2f( (mcf->data[i] * (1.0f/32768.0f)) * _modOct )) : fcBase;
    if (fcInst < 5.0f)      fcInst = 5.0f;
    if (fcInst > 0.33f*fs)  fcInst = 0.33f*fs;

    const float g  = tanf(PI * fcInst / fs);
    const float gg = g / (1.0f + g);

    // DC/eEnv
    _dc += dcAlpha * (y4 - _dc);
    const float y4_ac = y4 - _dc;
    float targetEnv = fabsf(y4_ac);
    _env += (targetEnv > _env ? envAttack : envRelease) * (targetEnv - _env);

    // resonance (mod before clamp)
    float kBase = _k;
    if (hasResMod) {
      kBase += (mrs->data[i] * (1.0f/32768.0f)) * _resModDepth;
    }
    if (kBase < 0.0f) kBase = 0.0f;

    const float E0   = 0.22f;
    const float beta = 4.0f;
    float over = _env - E0; if (over < 0.0f) over = 0.0f;
    const float kSafe = kBase / (1.0f + beta * over * over);

    const float x_fb = x - kSafe * y4_ac;


    // GS with relaxation; Moog couplings = pure cascade
    const float omega = 0.63f;
    for (int it=0; it<3; ++it) {
      float u1 = x_fb;
      float v1 = (u1 - s1) * gg;  float y1n = v1 + s1;
      y1 = (1.0f - omega)*y1 + omega*y1n;

      float u2 = y1;
      float v2 = (u2 - s2) * gg;  float y2n = v2 + s2;
      y2 = (1.0f - omega)*y2 + omega*y2n;

      float u3 = y2;
      float v3 = (u3 - s3) * gg;  float y3n = v3 + s3;
      y3 = (1.0f - omega)*y3 + omega*y3n;

      float u4 = y3;
      float v4 = (u4 - s4) * gg;  float y4n = v4 + s4;
      y4 = (1.0f - omega)*y4 + omega*y4n;
    }

    // ZDF commit
    {
      float u1 = x_fb;  float v1 = (u1 - s1) * gg;  float y1f = v1 + s1;  s1 = y1f + v1;  y1 = y1f;
      float u2 = y1;    float v2 = (u2 - s2) * gg;  float y2f = v2 + s2;  s2 = y2f + v2;  y2 = y2f;
      float u3 = y2;    float v3 = (u3 - s3) * gg;  float y3f = v3 + s3;  s3 = y3f + v3;  y3 = y3f;
      float u4 = y3;    float v4 = (u4 - s4) * gg;  float y4f = v4 + s4;  s4 = y4f + v4;  y4 = y4f;
    }

    float o = y4; if (o>1.0f) o=1.0f; if (o<-1.0f) o=-1.0f;
    out->data[i] = (int16_t)(o * 32767.0f);
  }

  _s1=s1; _s2=s2; _s3=s3; _s4=s4;
  _y1=y1; _y2=y2; _y3=y3; _y4=y4;

  transmit(out);
  release(out);
  release(in);
  if (mcf) release(mcf);
  if (mrs) release(mrs);
}
