#ifndef GOT_H
#define GOT_H

void clear(int *ptr);
void set(volatile int *ptr);
int test(int *ptr);

// assumes that the data in the memory locations pointed by in and out is always zero.
void not(int *in, int *out);

void init(void);

#endif
