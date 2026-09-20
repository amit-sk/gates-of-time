#ifndef GOT_H
#define GOT_H

#include <stdbool.h>
#include <stdint.h>

void clear(int *ptr);
void set(volatile int *ptr);
int test(int *ptr);

/*
 * assumes that the data in the memory locations pointed by in and out is always zero.
 * several attempts at implementation:
 * not - simplest implementation
 * imul_not - uses imul instruction to introduce a delay, similar to the GoT paper
 * imul_not2 - similar to GoT github - a more complex implementation to avoid compiler optimizations
 */
void not(int *in, int *out);
void imul_not(int *in, int *out);
uintptr_t imul_not2(uintptr_t in, uintptr_t out, uintptr_t trash);

void nand(int *in1, int *in2, int *out);
void nand2(int *in1, int *in2, int *out1, int *out2);

uintptr_t fan2(uintptr_t in, uintptr_t out1, uintptr_t out2,
               uintptr_t trash);
uintptr_t and(uintptr_t input1, uintptr_t input2, uintptr_t output,
                   uintptr_t trash);
uintptr_t or(uintptr_t input1, uintptr_t input2, uintptr_t output,
                  uintptr_t trash);
uintptr_t half_adder_impl(volatile uintptr_t a, volatile uintptr_t b,
                          volatile uintptr_t sum, volatile uintptr_t carry,
                          volatile uintptr_t trash);
uintptr_t full_adder_impl(volatile uintptr_t a, volatile uintptr_t b,
                          volatile uintptr_t carry_in,
                          volatile uintptr_t sum,
                          volatile uintptr_t carry_out,
                          volatile uintptr_t trash);
uintptr_t adder3_impl(const uintptr_t a[static 3],
                      const uintptr_t b[static 3],
                      const uintptr_t sum[static 3],
                      volatile uintptr_t trash);

void init(void);

#endif
