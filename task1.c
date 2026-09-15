#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "got.h"

#define TRIALS 10000

static void report(const char *state, int correct, int total)
{
    if (total == 0) {
        printf("%s: no trials\n", state);
        return;
    }

    printf("%s: %.2f%% success (%d/%d correct)\n",
           state, 100.0 * correct / total, correct, total);
}

int main(void)
{
    _Alignas(64) int line[16] = {0};
    int total[2] = {0};
    int correct[2] = {0};

    srand((unsigned int)time(NULL));
    init();

    for (int i = 0; i < TRIALS; ++i) {
        int expected = rand() % 2;
        ++total[expected];

        if (expected)
            set(line);
        else
            clear(line);

        int observed = test(line);
        correct[expected] += observed == expected;
    }

    report("Cached", correct[1], total[1]);
    report("Uncached", correct[0], total[0]);
    return EXIT_SUCCESS;
}
