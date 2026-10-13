/* ordertest: x86 memory ordering (TSO) as every thread of an x86-64 program sees it.
 * Message passing: a writer stores data and then a flag, a reader loads the flag and then the data. x86 never lets
 * the reader see the new flag with older data. Arm's weak ordering does, unless the emulator orders that thread's
 * memory accesses: FEX turns on Apple's hardware TSO mode, which on macOS is per thread (fex/patches/0018). Each
 * pair runs on two threads created for it, so a thread the emulator left weakly ordered shows as a failing pair.
 * Usage: ordertest.exe [pairs] [rounds], defaults 4 and 2,000,000 (the M4 measurement of 2026-10-12 used 6 pairs
 * of 20,000,000 rounds: up to 2,841,851 reorderings per pair before 0018, none after).
 * 32-bit counters, so the 32-bit build (ordertest32.exe, through WoW64) reads and writes them in one access. */
#include "fid.h"
#include <stdlib.h>

#define MAX_PAIRS 16

static volatile LONG data_[MAX_PAIRS * 16];   /* one cache line apart: index * 16 */
static volatile LONG flag_[MAX_PAIRS * 16];
static volatile LONG stop_;
static LONG bad_[MAX_PAIRS];
static LONG rounds_;

static DWORD WINAPI writer(void *arg)
{
    int p = (int)(INT_PTR)arg;
    for (LONG i = 1; i <= rounds_; i++)
    {
        data_[p * 16] = i;
        flag_[p * 16] = i;
    }
    return 0;
}

static DWORD WINAPI reader(void *arg)
{
    int p = (int)(INT_PTR)arg;
    LONG n = 0;
    while (!stop_)
    {
        LONG f = flag_[p * 16];
        LONG d = data_[p * 16];
        if (d < f) n++;
    }
    bad_[p] = n;
    return 0;
}

int main(int argc, char **argv)
{
    int pairs = argc > 1 ? atoi(argv[1]) : 4;
    HANDLE w[MAX_PAIRS], r[MAX_PAIRS];
    DWORD t0;
    char id[64];

    fid_name = "ordertest";
    rounds_ = argc > 2 ? atol(argv[2]) : 2000000;
    if (pairs < 1) pairs = 1;
    if (pairs > MAX_PAIRS) pairs = MAX_PAIRS;
    fid_info("ordertest.config", "%d pairs, %ld rounds, %u-bit", pairs, (long)rounds_, (unsigned)(sizeof(void *) * 8));
    t0 = GetTickCount();
    for (int p = 0; p < pairs; p++)
    {
        r[p] = CreateThread(NULL, 0, reader, (void *)(INT_PTR)p, 0, NULL);
        w[p] = CreateThread(NULL, 0, writer, (void *)(INT_PTR)p, 0, NULL);
    }
    WaitForMultipleObjects(pairs, w, TRUE, INFINITE);
    stop_ = 1;
    WaitForMultipleObjects(pairs, r, TRUE, INFINITE);
    for (int p = 0; p < pairs; p++)
    {
        snprintf(id, sizeof(id), "ordertest.mp.pair%d", p);
        fid_check(bad_[p] == 0, id, "%ld reads saw the new flag with older data", (long)bad_[p]);
    }
    fid_info("ordertest.ms", "%lu", (unsigned long)(GetTickCount() - t0));
    return fid_summary();
}
