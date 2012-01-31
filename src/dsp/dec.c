// Copyright 2010 Google Inc. All Rights Reserved.
//
// This code is licensed under the same terms as WebM:
//  Software License Agreement:  http://www.webmproject.org/license/software/
//  Additional IP Rights Grant:  http://www.webmproject.org/license/additional/
// -----------------------------------------------------------------------------
//
// Speed-critical decoding functions.
//
// Author: Skal (pascal.massimino@gmail.com)

#include "./dsp.h"
#include "../dec/vp8i.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

//------------------------------------------------------------------------------
// run-time tables (~4k)

static uint8_t abs0[255 + 255 + 1];     // abs(i)
static uint8_t abs1[255 + 255 + 1];     // abs(i)>>1
static int8_t sclip1[1020 + 1020 + 1];  // clips [-1020, 1020] to [-128, 127]
static int8_t sclip2[112 + 112 + 1];    // clips [-112, 112] to [-16, 15]
static uint8_t clip1[255 + 510 + 1];    // clips [-255,510] to [0,255]

// We declare this variable 'volatile' to prevent instruction reordering
// and make sure it's set to true _last_ (so as to be thread-safe)
static volatile int tables_ok = 0;

static void DspInitTables(void) {
  if (!tables_ok) {
    int i;
    for (i = -255; i <= 255; ++i) {
      abs0[255 + i] = (i < 0) ? -i : i;
      abs1[255 + i] = abs0[255 + i] >> 1;
    }
    for (i = -1020; i <= 1020; ++i) {
      sclip1[1020 + i] = (i < -128) ? -128 : (i > 127) ? 127 : i;
    }
    for (i = -112; i <= 112; ++i) {
      sclip2[112 + i] = (i < -16) ? -16 : (i > 15) ? 15 : i;
    }
    for (i = -255; i <= 255 + 255; ++i) {
      clip1[255 + i] = (i < 0) ? 0 : (i > 255) ? 255 : i;
    }
    tables_ok = 1;
  }
}

static WEBP_INLINE uint8_t clip_8b(int v) {
  return (!(v & ~0xff)) ? v : (v < 0) ? 0 : 255;
}

//------------------------------------------------------------------------------
// Transforms (Paragraph 14.4)

#define STORE(x, y, v) \
  dst[x + y * BPS] = clip_8b(dst[x + y * BPS] + ((v) >> 3))

static const int kC1 = 20091 + (1 << 16);
static const int kC2 = 35468;
#define MUL(a, b) (((a) * (b)) >> 16)

static void TransformOne(const int16_t* in, uint8_t* dst) {
  int C[4 * 4], *tmp;
  int i;
  tmp = C;
  for (i = 0; i < 4; ++i) {    // vertical pass
    const int a = in[0] + in[8];    // [-4096, 4094]
    const int b = in[0] - in[8];    // [-4095, 4095]
    const int c = MUL(in[4], kC2) - MUL(in[12], kC1);   // [-3783, 3783]
    const int d = MUL(in[4], kC1) + MUL(in[12], kC2);   // [-3785, 3781]
    tmp[0] = a + d;   // [-7881, 7875]
    tmp[1] = b + c;   // [-7878, 7878]
    tmp[2] = b - c;   // [-7878, 7878]
    tmp[3] = a - d;   // [-7877, 7879]
    tmp += 4;
    in++;
  }
  // Each pass is expanding the dynamic range by ~3.85 (upper bound).
  // The exact value is (2. + (kC1 + kC2) / 65536).
  // After the second pass, maximum interval is [-3794, 3794], assuming
  // an input in [-2048, 2047] interval. We then need to add a dst value
  // in the [0, 255] range.
  // In the worst case scenario, the input to clip_8b() can be as large as
  // [-60713, 60968].
  tmp = C;
  for (i = 0; i < 4; ++i) {    // horizontal pass
    const int dc = tmp[0] + 4;
    const int a =  dc +  tmp[8];
    const int b =  dc -  tmp[8];
    const int c = MUL(tmp[4], kC2) - MUL(tmp[12], kC1);
    const int d = MUL(tmp[4], kC1) + MUL(tmp[12], kC2);
    STORE(0, 0, a + d);
    STORE(1, 0, b + c);
    STORE(2, 0, b - c);
    STORE(3, 0, a - d);
    tmp++;
    dst += BPS;
  }
}
#undef MUL

static void TransformTwo(const int16_t* in, uint8_t* dst, int do_two) {
  TransformOne(in, dst);
  if (do_two) {
    TransformOne(in + 16, dst + 4);
  }
}

static void TransformUV(const int16_t* in, uint8_t* dst) {
  VP8Transform(in + 0 * 16, dst, 1);
  VP8Transform(in + 2 * 16, dst + 4 * BPS, 1);
}

static void TransformDC(const int16_t *in, uint8_t* dst) {
  const int DC = in[0] + 4;
  int i, j;
  for (j = 0; j < 4; ++j) {
    for (i = 0; i < 4; ++i) {
      STORE(i, j, DC);
    }
  }
}

static void TransformDCUV(const int16_t* in, uint8_t* dst) {
  if (in[0 * 16]) TransformDC(in + 0 * 16, dst);
  if (in[1 * 16]) TransformDC(in + 1 * 16, dst + 4);
  if (in[2 * 16]) TransformDC(in + 2 * 16, dst + 4 * BPS);
  if (in[3 * 16]) TransformDC(in + 3 * 16, dst + 4 * BPS + 4);
}

#undef STORE

//------------------------------------------------------------------------------
// Paragraph 14.3

static void TransformWHT(const int16_t* in, int16_t* out) {
  int tmp[16];
  int i;
  for (i = 0; i < 4; ++i) {
    const int a0 = in[0 + i] + in[12 + i];
    const int a1 = in[4 + i] + in[ 8 + i];
    const int a2 = in[4 + i] - in[ 8 + i];
    const int a3 = in[0 + i] - in[12 + i];
    tmp[0  + i] = a0 + a1;
    tmp[8  + i] = a0 - a1;
    tmp[4  + i] = a3 + a2;
    tmp[12 + i] = a3 - a2;
  }
  for (i = 0; i < 4; ++i) {
    const int dc = tmp[0 + i * 4] + 3;    // w/ rounder
    const int a0 = dc             + tmp[3 + i * 4];
    const int a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
    const int a2 = tmp[1 + i * 4] - tmp[2 + i * 4];
    const int a3 = dc             - tmp[3 + i * 4];
    out[ 0] = (a0 + a1) >> 3;
    out[16] = (a3 + a2) >> 3;
    out[32] = (a0 - a1) >> 3;
    out[48] = (a3 - a2) >> 3;
    out += 64;
  }
}

void (*VP8TransformWHT)(const int16_t* in, int16_t* out) = TransformWHT;

//------------------------------------------------------------------------------
// Intra predictions

#define DST(x, y) dst[(x) + (y) * BPS]

static WEBP_INLINE void TrueMotion(uint8_t *dst, int size) {
  const uint8_t* top = dst - BPS;
  const uint8_t* const clip0 = clip1 + 255 - top[-1];
  int y;
  for (y = 0; y < size; ++y) {
    const uint8_t* const clip = clip0 + dst[-1];
    int x;
    for (x = 0; x < size; ++x) {
      dst[x] = clip[top[x]];
    }
    dst += BPS;
  }
}
static void TM4(uint8_t *dst)   { TrueMotion(dst, 4); }
static void TM8uv(uint8_t *dst) { TrueMotion(dst, 8); }
static void TM16(uint8_t *dst)  { TrueMotion(dst, 16); }

//------------------------------------------------------------------------------
// 16x16

static void VE16(uint8_t *dst) {     // vertical
  int j;
  for (j = 0; j < 16; ++j) {
    memcpy(dst + j * BPS, dst - BPS, 16);
  }
}

static void HE16(uint8_t *dst) {     // horizontal
  int j;
  for (j = 16; j > 0; --j) {
    memset(dst, dst[-1], 16);
    dst += BPS;
  }
}

static WEBP_INLINE void Put16(int v, uint8_t* dst) {
  int j;
  for (j = 0; j < 16; ++j) {
    memset(dst + j * BPS, v, 16);
  }
}

static void DC16(uint8_t *dst) {    // DC
  int DC = 16;
  int j;
  for (j = 0; j < 16; ++j) {
    DC += dst[-1 + j * BPS] + dst[j - BPS];
  }
  Put16(DC >> 5, dst);
}

static void DC16NoTop(uint8_t *dst) {   // DC with top samples not available
  int DC = 8;
  int j;
  for (j = 0; j < 16; ++j) {
    DC += dst[-1 + j * BPS];
  }
  Put16(DC >> 4, dst);
}

static void DC16NoLeft(uint8_t *dst) {  // DC with left samples not available
  int DC = 8;
  int i;
  for (i = 0; i < 16; ++i) {
    DC += dst[i - BPS];
  }
  Put16(DC >> 4, dst);
}

static void DC16NoTopLeft(uint8_t *dst) {  // DC with no top and left samples
  Put16(0x80, dst);
}

//------------------------------------------------------------------------------
// 4x4

#define AVG3(a, b, c) (((a) + 2 * (b) + (c) + 2) >> 2)
#define AVG2(a, b) (((a) + (b) + 1) >> 1)

static void VE4(uint8_t *dst) {    // vertical
  const uint8_t* top = dst - BPS;
  const uint8_t vals[4] = {
    AVG3(top[-1], top[0], top[1]),
    AVG3(top[ 0], top[1], top[2]),
    AVG3(top[ 1], top[2], top[3]),
    AVG3(top[ 2], top[3], top[4])
  };
  int i;
  for (i = 0; i < 4; ++i) {
    memcpy(dst + i * BPS, vals, sizeof(vals));
  }
}

static void HE4(uint8_t *dst) {    // horizontal
  const int A = dst[-1 - BPS];
  const int B = dst[-1];
  const int C = dst[-1 + BPS];
  const int D = dst[-1 + 2 * BPS];
  const int E = dst[-1 + 3 * BPS];
  *(uint32_t*)(dst + 0 * BPS) = 0x01010101U * AVG3(A, B, C);
  *(uint32_t*)(dst + 1 * BPS) = 0x01010101U * AVG3(B, C, D);
  *(uint32_t*)(dst + 2 * BPS) = 0x01010101U * AVG3(C, D, E);
  *(uint32_t*)(dst + 3 * BPS) = 0x01010101U * AVG3(D, E, E);
}

static void DC4(uint8_t *dst) {   // DC
  uint32_t dc = 4;
  int i;
  for (i = 0; i < 4; ++i) dc += dst[i - BPS] + dst[-1 + i * BPS];
  dc >>= 3;
  for (i = 0; i < 4; ++i) memset(dst + i * BPS, dc, 4);
}

static void RD4(uint8_t *dst) {   // Down-right
  const int I = dst[-1 + 0 * BPS];
  const int J = dst[-1 + 1 * BPS];
  const int K = dst[-1 + 2 * BPS];
  const int L = dst[-1 + 3 * BPS];
  const int X = dst[-1 - BPS];
  const int A = dst[0 - BPS];
  const int B = dst[1 - BPS];
  const int C = dst[2 - BPS];
  const int D = dst[3 - BPS];
  DST(0, 3)                                     = AVG3(J, K, L);
  DST(0, 2) = DST(1, 3)                         = AVG3(I, J, K);
  DST(0, 1) = DST(1, 2) = DST(2, 3)             = AVG3(X, I, J);
  DST(0, 0) = DST(1, 1) = DST(2, 2) = DST(3, 3) = AVG3(A, X, I);
  DST(1, 0) = DST(2, 1) = DST(3, 2)             = AVG3(B, A, X);
  DST(2, 0) = DST(3, 1)                         = AVG3(C, B, A);
  DST(3, 0)                                     = AVG3(D, C, B);
}

static void LD4(uint8_t *dst) {   // Down-Left
  const int A = dst[0 - BPS];
  const int B = dst[1 - BPS];
  const int C = dst[2 - BPS];
  const int D = dst[3 - BPS];
  const int E = dst[4 - BPS];
  const int F = dst[5 - BPS];
  const int G = dst[6 - BPS];
  const int H = dst[7 - BPS];
  DST(0, 0)                                     = AVG3(A, B, C);
  DST(1, 0) = DST(0, 1)                         = AVG3(B, C, D);
  DST(2, 0) = DST(1, 1) = DST(0, 2)             = AVG3(C, D, E);
  DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = AVG3(D, E, F);
  DST(3, 1) = DST(2, 2) = DST(1, 3)             = AVG3(E, F, G);
  DST(3, 2) = DST(2, 3)                         = AVG3(F, G, H);
  DST(3, 3)                                     = AVG3(G, H, H);
}

static void VR4(uint8_t *dst) {   // Vertical-Right
  const int I = dst[-1 + 0 * BPS];
  const int J = dst[-1 + 1 * BPS];
  const int K = dst[-1 + 2 * BPS];
  const int X = dst[-1 - BPS];
  const int A = dst[0 - BPS];
  const int B = dst[1 - BPS];
  const int C = dst[2 - BPS];
  const int D = dst[3 - BPS];
  DST(0, 0) = DST(1, 2) = AVG2(X, A);
  DST(1, 0) = DST(2, 2) = AVG2(A, B);
  DST(2, 0) = DST(3, 2) = AVG2(B, C);
  DST(3, 0)             = AVG2(C, D);

  DST(0, 3) =             AVG3(K, J, I);
  DST(0, 2) =             AVG3(J, I, X);
  DST(0, 1) = DST(1, 3) = AVG3(I, X, A);
  DST(1, 1) = DST(2, 3) = AVG3(X, A, B);
  DST(2, 1) = DST(3, 3) = AVG3(A, B, C);
  DST(3, 1) =             AVG3(B, C, D);
}

static void VL4(uint8_t *dst) {   // Vertical-Left
  const int A = dst[0 - BPS];
  const int B = dst[1 - BPS];
  const int C = dst[2 - BPS];
  const int D = dst[3 - BPS];
  const int E = dst[4 - BPS];
  const int F = dst[5 - BPS];
  const int G = dst[6 - BPS];
  const int H = dst[7 - BPS];
  DST(0, 0) =             AVG2(A, B);
  DST(1, 0) = DST(0, 2) = AVG2(B, C);
  DST(2, 0) = DST(1, 2) = AVG2(C, D);
  DST(3, 0) = DST(2, 2) = AVG2(D, E);

  DST(0, 1) =             AVG3(A, B, C);
  DST(1, 1) = DST(0, 3) = AVG3(B, C, D);
  DST(2, 1) = DST(1, 3) = AVG3(C, D, E);
  DST(3, 1) = DST(2, 3) = AVG3(D, E, F);
              DST(3, 2) = AVG3(E, F, G);
              DST(3, 3) = AVG3(F, G, H);
}

static void HU4(uint8_t *dst) {   // Horizontal-Up
  const int I = dst[-1 + 0 * BPS];
  const int J = dst[-1 + 1 * BPS];
  const int K = dst[-1 + 2 * BPS];
  const int L = dst[-1 + 3 * BPS];
  DST(0, 0) =             AVG2(I, J);
  DST(2, 0) = DST(0, 1) = AVG2(J, K);
  DST(2, 1) = DST(0, 2) = AVG2(K, L);
  DST(1, 0) =             AVG3(I, J, K);
  DST(3, 0) = DST(1, 1) = AVG3(J, K, L);
  DST(3, 1) = DST(1, 2) = AVG3(K, L, L);
  DST(3, 2) = DST(2, 2) =
    DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = L;
}

static void HD4(uint8_t *dst) {  // Horizontal-Down
  const int I = dst[-1 + 0 * BPS];
  const int J = dst[-1 + 1 * BPS];
  const int K = dst[-1 + 2 * BPS];
  const int L = dst[-1 + 3 * BPS];
  const int X = dst[-1 - BPS];
  const int A = dst[0 - BPS];
  const int B = dst[1 - BPS];
  const int C = dst[2 - BPS];

  DST(0, 0) = DST(2, 1) = AVG2(I, X);
  DST(0, 1) = DST(2, 2) = AVG2(J, I);
  DST(0, 2) = DST(2, 3) = AVG2(K, J);
  DST(0, 3)             = AVG2(L, K);

  DST(3, 0)             = AVG3(A, B, C);
  DST(2, 0)             = AVG3(X, A, B);
  DST(1, 0) = DST(3, 1) = AVG3(I, X, A);
  DST(1, 1) = DST(3, 2) = AVG3(J, I, X);
  DST(1, 2) = DST(3, 3) = AVG3(K, J, I);
  DST(1, 3)             = AVG3(L, K, J);
}

#undef DST
#undef AVG3
#undef AVG2

//------------------------------------------------------------------------------
// Chroma

static void VE8uv(uint8_t *dst) {    // vertical
  int j;
  for (j = 0; j < 8; ++j) {
    memcpy(dst + j * BPS, dst - BPS, 8);
  }
}

static void HE8uv(uint8_t *dst) {    // horizontal
  int j;
  for (j = 0; j < 8; ++j) {
    memset(dst, dst[-1], 8);
    dst += BPS;
  }
}

// helper for chroma-DC predictions
static WEBP_INLINE void Put8x8uv(uint64_t v, uint8_t* dst) {
  int j;
  for (j = 0; j < 8; ++j) {
    *(uint64_t*)(dst + j * BPS) = v;
  }
}

static void DC8uv(uint8_t *dst) {     // DC
  int dc0 = 8;
  int i;
  for (i = 0; i < 8; ++i) {
    dc0 += dst[i - BPS] + dst[-1 + i * BPS];
  }
  Put8x8uv((uint64_t)((dc0 >> 4) * 0x0101010101010101ULL), dst);
}

static void DC8uvNoLeft(uint8_t *dst) {   // DC with no left samples
  int dc0 = 4;
  int i;
  for (i = 0; i < 8; ++i) {
    dc0 += dst[i - BPS];
  }
  Put8x8uv((uint64_t)((dc0 >> 3) * 0x0101010101010101ULL), dst);
}

static void DC8uvNoTop(uint8_t *dst) {  // DC with no top samples
  int dc0 = 4;
  int i;
  for (i = 0; i < 8; ++i) {
    dc0 += dst[-1 + i * BPS];
  }
  Put8x8uv((uint64_t)((dc0 >> 3) * 0x0101010101010101ULL), dst);
}

static void DC8uvNoTopLeft(uint8_t *dst) {    // DC with nothing
  Put8x8uv(0x8080808080808080ULL, dst);
}

//------------------------------------------------------------------------------
// default C implementations

const VP8PredFunc VP8PredLuma4[NUM_BMODES] = {
  DC4, TM4, VE4, HE4, RD4, VR4, LD4, VL4, HD4, HU4
};

const VP8PredFunc VP8PredLuma16[NUM_B_DC_MODES] = {
  DC16, TM16, VE16, HE16,
  DC16NoTop, DC16NoLeft, DC16NoTopLeft
};

const VP8PredFunc VP8PredChroma8[NUM_B_DC_MODES] = {
  DC8uv, TM8uv, VE8uv, HE8uv,
  DC8uvNoTop, DC8uvNoLeft, DC8uvNoTopLeft
};

//------------------------------------------------------------------------------
// Edge filtering functions

// 4 pixels in, 2 pixels out
static WEBP_INLINE void do_filter2(uint8_t* p, int step) {
  const int p1 = p[-2*step], p0 = p[-step], q0 = p[0], q1 = p[step];
  const int a = 3 * (q0 - p0) + sclip1[1020 + p1 - q1];
  const int a1 = sclip2[112 + ((a + 4) >> 3)];
  const int a2 = sclip2[112 + ((a + 3) >> 3)];
  p[-step] = clip1[255 + p0 + a2];
  p[    0] = clip1[255 + q0 - a1];
}

// 4 pixels in, 4 pixels out
static WEBP_INLINE void do_filter4(uint8_t* p, int step) {
  const int p1 = p[-2*step], p0 = p[-step], q0 = p[0], q1 = p[step];
  const int a = 3 * (q0 - p0);
  const int a1 = sclip2[112 + ((a + 4) >> 3)];
  const int a2 = sclip2[112 + ((a + 3) >> 3)];
  const int a3 = (a1 + 1) >> 1;
  p[-2*step] = clip1[255 + p1 + a3];
  p[-  step] = clip1[255 + p0 + a2];
  p[      0] = clip1[255 + q0 - a1];
  p[   step] = clip1[255 + q1 - a3];
}

// 6 pixels in, 6 pixels out
static WEBP_INLINE void do_filter6(uint8_t* p, int step) {
  const int p2 = p[-3*step], p1 = p[-2*step], p0 = p[-step];
  const int q0 = p[0], q1 = p[step], q2 = p[2*step];
  const int a = sclip1[1020 + 3 * (q0 - p0) + sclip1[1020 + p1 - q1]];
  const int a1 = (27 * a + 63) >> 7;  // eq. to ((3 * a + 7) * 9) >> 7
  const int a2 = (18 * a + 63) >> 7;  // eq. to ((2 * a + 7) * 9) >> 7
  const int a3 = (9  * a + 63) >> 7;  // eq. to ((1 * a + 7) * 9) >> 7
  p[-3*step] = clip1[255 + p2 + a3];
  p[-2*step] = clip1[255 + p1 + a2];
  p[-  step] = clip1[255 + p0 + a1];
  p[      0] = clip1[255 + q0 - a1];
  p[   step] = clip1[255 + q1 - a2];
  p[ 2*step] = clip1[255 + q2 - a3];
}

static WEBP_INLINE int hev(const uint8_t* p, int step, int thresh) {
  const int p1 = p[-2*step], p0 = p[-step], q0 = p[0], q1 = p[step];
  return (abs0[255 + p1 - p0] > thresh) || (abs0[255 + q1 - q0] > thresh);
}

static WEBP_INLINE int needs_filter(const uint8_t* p, int step, int thresh) {
  const int p1 = p[-2*step], p0 = p[-step], q0 = p[0], q1 = p[step];
  return (2 * abs0[255 + p0 - q0] + abs1[255 + p1 - q1]) <= thresh;
}

static WEBP_INLINE int needs_filter2(const uint8_t* p,
                                     int step, int t, int it) {
  const int p3 = p[-4*step], p2 = p[-3*step], p1 = p[-2*step], p0 = p[-step];
  const int q0 = p[0], q1 = p[step], q2 = p[2*step], q3 = p[3*step];
  if ((2 * abs0[255 + p0 - q0] + abs1[255 + p1 - q1]) > t)
    return 0;
  return abs0[255 + p3 - p2] <= it && abs0[255 + p2 - p1] <= it &&
         abs0[255 + p1 - p0] <= it && abs0[255 + q3 - q2] <= it &&
         abs0[255 + q2 - q1] <= it && abs0[255 + q1 - q0] <= it;
}

//------------------------------------------------------------------------------
// Simple In-loop filtering (Paragraph 15.2)

static void SimpleVFilter16(uint8_t* p, int stride, int thresh) {
  int i;
  for (i = 0; i < 16; ++i) {
    if (needs_filter(p + i, stride, thresh)) {
      do_filter2(p + i, stride);
    }
  }
}

static void SimpleHFilter16(uint8_t* p, int stride, int thresh) {
  int i;
  for (i = 0; i < 16; ++i) {
    if (needs_filter(p + i * stride, 1, thresh)) {
      do_filter2(p + i * stride, 1);
    }
  }
}

static void SimpleVFilter16i(uint8_t* p, int stride, int thresh) {
  int k;
  for (k = 3; k > 0; --k) {
    p += 4 * stride;
    SimpleVFilter16(p, stride, thresh);
  }
}

static void SimpleHFilter16i(uint8_t* p, int stride, int thresh) {
  int k;
  for (k = 3; k > 0; --k) {
    p += 4;
    SimpleHFilter16(p, stride, thresh);
  }
}

//------------------------------------------------------------------------------
// Complex In-loop filtering (Paragraph 15.3)

static WEBP_INLINE void FilterLoop26(uint8_t* p,
                                     int hstride, int vstride, int size,
                                     int thresh, int ithresh, int hev_thresh) {
  while (size-- > 0) {
    if (needs_filter2(p, hstride, thresh, ithresh)) {
      if (hev(p, hstride, hev_thresh)) {
        do_filter2(p, hstride);
      } else {
        do_filter6(p, hstride);
      }
    }
    p += vstride;
  }
}

static WEBP_INLINE void FilterLoop24(uint8_t* p,
                                     int hstride, int vstride, int size,
                                     int thresh, int ithresh, int hev_thresh) {
  while (size-- > 0) {
    if (needs_filter2(p, hstride, thresh, ithresh)) {
      if (hev(p, hstride, hev_thresh)) {
        do_filter2(p, hstride);
      } else {
        do_filter4(p, hstride);
      }
    }
    p += vstride;
  }
}

// on macroblock edges
static void VFilter16(uint8_t* p, int stride,
                      int thresh, int ithresh, int hev_thresh) {
  FilterLoop26(p, stride, 1, 16, thresh, ithresh, hev_thresh);
}

static void HFilter16(uint8_t* p, int stride,
                      int thresh, int ithresh, int hev_thresh) {
  FilterLoop26(p, 1, stride, 16, thresh, ithresh, hev_thresh);
}

// on three inner edges
static void VFilter16i(uint8_t* p, int stride,
                       int thresh, int ithresh, int hev_thresh) {
  int k;
  for (k = 3; k > 0; --k) {
    p += 4 * stride;
    FilterLoop24(p, stride, 1, 16, thresh, ithresh, hev_thresh);
  }
}

static void HFilter16i(uint8_t* p, int stride,
                       int thresh, int ithresh, int hev_thresh) {
  int k;
  for (k = 3; k > 0; --k) {
    p += 4;
    FilterLoop24(p, 1, stride, 16, thresh, ithresh, hev_thresh);
  }
}

// 8-pixels wide variant, for chroma filtering
static void VFilter8(uint8_t* u, uint8_t* v, int stride,
                     int thresh, int ithresh, int hev_thresh) {
  FilterLoop26(u, stride, 1, 8, thresh, ithresh, hev_thresh);
  FilterLoop26(v, stride, 1, 8, thresh, ithresh, hev_thresh);
}

static void HFilter8(uint8_t* u, uint8_t* v, int stride,
                     int thresh, int ithresh, int hev_thresh) {
  FilterLoop26(u, 1, stride, 8, thresh, ithresh, hev_thresh);
  FilterLoop26(v, 1, stride, 8, thresh, ithresh, hev_thresh);
}

static void VFilter8i(uint8_t* u, uint8_t* v, int stride,
                      int thresh, int ithresh, int hev_thresh) {
  FilterLoop24(u + 4 * stride, stride, 1, 8, thresh, ithresh, hev_thresh);
  FilterLoop24(v + 4 * stride, stride, 1, 8, thresh, ithresh, hev_thresh);
}

static void HFilter8i(uint8_t* u, uint8_t* v, int stride,
                      int thresh, int ithresh, int hev_thresh) {
  FilterLoop24(u + 4, 1, stride, 8, thresh, ithresh, hev_thresh);
  FilterLoop24(v + 4, 1, stride, 8, thresh, ithresh, hev_thresh);
}

//------------------------------------------------------------------------------

VP8DecIdct2 VP8Transform;
VP8DecIdct VP8TransformUV;
VP8DecIdct VP8TransformDC;
VP8DecIdct VP8TransformDCUV;

VP8LumaFilterFunc VP8VFilter16;
VP8LumaFilterFunc VP8HFilter16;
VP8ChromaFilterFunc VP8VFilter8;
VP8ChromaFilterFunc VP8HFilter8;
VP8LumaFilterFunc VP8VFilter16i;
VP8LumaFilterFunc VP8HFilter16i;
VP8ChromaFilterFunc VP8VFilter8i;
VP8ChromaFilterFunc VP8HFilter8i;
VP8SimpleFilterFunc VP8SimpleVFilter16;
VP8SimpleFilterFunc VP8SimpleHFilter16;
VP8SimpleFilterFunc VP8SimpleVFilter16i;
VP8SimpleFilterFunc VP8SimpleHFilter16i;

extern void VP8DspInitSSE2(void);
extern void VP8DspInitNEON(void);

void VP8DspInit(void) {
  DspInitTables();

  VP8Transform = TransformTwo;
  VP8TransformUV = TransformUV;
  VP8TransformDC = TransformDC;
  VP8TransformDCUV = TransformDCUV;

  VP8VFilter16 = VFilter16;
  VP8HFilter16 = HFilter16;
  VP8VFilter8 = VFilter8;
  VP8HFilter8 = HFilter8;
  VP8VFilter16i = VFilter16i;
  VP8HFilter16i = HFilter16i;
  VP8VFilter8i = VFilter8i;
  VP8HFilter8i = HFilter8i;
  VP8SimpleVFilter16 = SimpleVFilter16;
  VP8SimpleHFilter16 = SimpleHFilter16;
  VP8SimpleVFilter16i = SimpleVFilter16i;
  VP8SimpleHFilter16i = SimpleHFilter16i;

  // If defined, use CPUInfo() to overwrite some pointers with faster versions.
  if (VP8GetCPUInfo) {
#if defined(WEBP_USE_SSE2)
    if (VP8GetCPUInfo(kSSE2)) {
      VP8DspInitSSE2();
    }
#elif defined(__GNUC__) && defined(__ARM_NEON__)
    if (VP8GetCPUInfo(kNEON)) {
      VP8DspInitNEON();
    }
#endif
  }
}

#if defined(__cplusplus) || defined(c_plusplus)
}    // extern "C"
#endif
