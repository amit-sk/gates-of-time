#include <stdio.h>
#include <stdlib.h>

#include "arch_primitives.h"
#include "got.h"

#define TRIALS 10000

void branch1(int *cond, int *ptr)
{
    clear(ptr);

    memory_fence();
    if (*cond)
        return;
    set(ptr);
}

void branch2(int *cond, int *ptr)
{
    clear(ptr);

    for (int i = 0; i < 256; ++i)
        asm("" ::: "memory");

    memory_fence();
    if (*cond)
        return;
    set(ptr);
}

static void test_branch(const char *name, void (*branch)(int *, int *))
{
    static int zero = 0;
    static int one = 1;
    int *line = calloc(1, sizeof(*line));
    int mispredictions = 0;

    if (line == NULL) {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < TRIALS; ++i) {
        branch(&zero, line);
        branch(&zero, line);
        clear(&one);
        branch(&one, line);
        mispredictions += (test(line) == 1);
    }

    printf("%s: %.2f%% mispredictions detected (%d/%d cached after inverse condition)\n",
           name, 100.0 * mispredictions / TRIALS, mispredictions, TRIALS);
    free(line);
}

void test_branches(void)
{
    init();
    test_branch("branch1", branch1);
    test_branch("branch2", branch2);
}

int main(void)
{
    test_branches();
    return EXIT_SUCCESS;
}
