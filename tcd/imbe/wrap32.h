#ifndef _WRAP32_H_
#define _WRAP32_H_

// 32-bit arithmetic that wraps around, as the fixed point code means it (the
// basic operators' overflow check, the phase accumulators modulo 2^32), done
// in unsigned arithmetic: a signed overflow or the left shift of a negative
// value is undefined in C++ (before C++20), the result here is the same bits.
// Added to the vocoder in tcd 0.23.3 (UBSan reported them).

#include <cstdint>

static inline int32_t wadd32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
static inline int32_t wsub32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
static inline int32_t wmul32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
static inline int32_t wshl32(int32_t a, int n)     { return (int32_t)((uint32_t)a << n); }

#endif
