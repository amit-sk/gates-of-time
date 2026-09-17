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

void init(void);

#endif
