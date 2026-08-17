/*
 * vibe_ai.h -- the on-board AI pipeline: feature extraction + random forest.
 * This EXACT file is used both in the PC validation test and in the Arduino
 * sketch, so what we prove on the PC is what runs on the board.
 *
 * Contract (must match step4_build.py):
 *   FS = 1000 Hz, WIN = 1024 samples per window.
 *   Per axis: remove mean; rms; kurtosis (m4/m2^2 - 3); crest = peak/rms;
 *   FFT with Hann window; band energies normalised by total:
 *     e1x [0.7-1.3 f_r), e2x [1.7-2.3 f_r), e3x [2.7-3.3 f_r), ehigh [150-500 Hz)
 *   Feature order: X(rms,kurt,crest,e1,e2,e3,ehigh), then Y..., then Z...
 */
#ifndef VIBE_AI_H
#define VIBE_AI_H

#include <math.h>
#include <stdint.h>
#include "model_data.h"

#define AI_FS   1000
#define AI_WIN  1024
#define AI_HALF (AI_WIN / 2)

/* scratch buffers for the FFT (static: ~8 KB, allocated once) */
static float fft_re[AI_WIN];
static float fft_im[AI_WIN];

/* ------------------------------------------------------------------ */
/* iterative radix-2 FFT, same as everywhere else in this project      */
/* ------------------------------------------------------------------ */
static void ai_fft(float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i]; re[i] = re[j]; re[j] = t;
      t = im[i]; im[i] = im[j]; im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2.0f * (float)M_PI / (float)len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cwr = 1.0f, cwi = 0.0f;
      for (int k = 0; k < len / 2; k++) {
        float ur = re[i + k], ui = im[i + k];
        float vr = re[i + k + len / 2] * cwr - im[i + k + len / 2] * cwi;
        float vi = re[i + k + len / 2] * cwi + im[i + k + len / 2] * cwr;
        re[i + k] = ur + vr;           im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
        float nwr = cwr * wr - cwi * wi;
        cwi = cwr * wi + cwi * wr; cwr = nwr;
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* features for ONE axis; sig is the raw window for that axis (in g)   */
/* out[0..6] = rms, kurt, crest, e1x, e2x, e3x, ehigh                  */
/* ------------------------------------------------------------------ */
static void ai_axis_features(const float *sig, float f_r, float *out) {
  /* mean removal + time stats in double for accuracy */
  double mean = 0.0;
  for (int i = 0; i < AI_WIN; i++) mean += sig[i];
  mean /= AI_WIN;

  double m2 = 0.0, m4 = 0.0, peak = 0.0;
  for (int i = 0; i < AI_WIN; i++) {
    double d = sig[i] - mean;
    double d2 = d * d;
    m2 += d2; m4 += d2 * d2;
    double a = fabs(d); if (a > peak) peak = a;
  }
  m2 /= AI_WIN; m4 /= AI_WIN;
  double rms = sqrt(m2);
  out[0] = (float)rms;
  out[1] = (m2 > 0) ? (float)(m4 / (m2 * m2) - 3.0) : 0.0f;
  out[2] = (rms > 0) ? (float)(peak / rms) : 0.0f;

  /* windowed FFT */
  for (int i = 0; i < AI_WIN; i++) {
    float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (AI_WIN - 1));
    fft_re[i] = (float)((sig[i] - mean)) * w;
    fft_im[i] = 0.0f;
  }
  ai_fft(fft_re, fft_im, AI_WIN);

  /* band energies over bins 0..N/2 (freq = k * FS / WIN) */
  double total = 0.0, e1 = 0.0, e2 = 0.0, e3 = 0.0, ehi = 0.0;
  const float df = (float)AI_FS / (float)AI_WIN;
  for (int k = 0; k <= AI_HALF; k++) {
    double p = (double)fft_re[k] * fft_re[k] + (double)fft_im[k] * fft_im[k];
    total += p;
    float f = k * df;
    if (f >= 0.7f * f_r && f < 1.3f * f_r) e1 += p;
    if (f >= 1.7f * f_r && f < 2.3f * f_r) e2 += p;
    if (f >= 2.7f * f_r && f < 3.3f * f_r) e3 += p;
    if (f >= 150.0f    && f < 500.0f)      ehi += p;
  }
  total += 1e-12;
  out[3] = (float)(e1 / total);
  out[4] = (float)(e2 / total);
  out[5] = (float)(e3 / total);
  out[6] = (float)(ehi / total);
}

/* xyz: interleaved window buffers x[AI_WIN], y[AI_WIN], z[AI_WIN]      */
static void ai_extract_features(const float *x, const float *y,
                                const float *z, float rpm, float *feats) {
  float f_r = rpm / 60.0f;
  ai_axis_features(x, f_r, feats + 0);
  ai_axis_features(y, f_r, feats + 7);
  ai_axis_features(z, f_r, feats + 14);
}

/* ------------------------------------------------------------------ */
/* random forest inference: average the leaf probability vectors        */
/* returns predicted class index; probs_out gets the averaged vector    */
/* ------------------------------------------------------------------ */
static int ai_predict(const float *feats, float *probs_out) {
  float acc[N_CLASSES];
  for (int c = 0; c < N_CLASSES; c++) acc[c] = 0.0f;
  for (int t = 0; t < N_TREES; t++) {
    int n = TREE_OFFSET[t];
    while (NODE_FEAT[n] >= 0) {
      n = (feats[NODE_FEAT[n]] <= NODE_THR[n]) ? NODE_LEFT[n] : NODE_RIGHT[n];
    }
    const float *lp = &LEAF_PROB[NODE_LEFT[n] * N_CLASSES];
    for (int c = 0; c < N_CLASSES; c++) acc[c] += lp[c];
  }
  int best = 0;
  for (int c = 0; c < N_CLASSES; c++) {
    acc[c] /= N_TREES;
    if (probs_out) probs_out[c] = acc[c];
    if (acc[c] > acc[best]) best = c;
  }
  return best;
}

#endif /* VIBE_AI_H */
