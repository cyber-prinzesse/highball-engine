// smcdata.c (Highball, 2026-10-18): data written beside translated code, the pattern behind Microsoft Flight Simulator
// 2024's stutter under FEX. One RWX page holds a 300-instruction function (as costly to recompile as a game's), a small
// function the rewrite checks patch, and 2 KB in, a counter. Under FEX's SMC tracking every write to the page faults;
// before smc-precise each fault threw away the page's translations, so the next call recompiled the big function. The
// test times that loop, then checks that real code rewrites are still seen, alone and while a second thread keeps
// writing data on the same page (the window smc-precise opens for each replayed store).
// Build: tests/x64-fidelity/build.sh
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*fn_t)(void);

static uint8_t* page;
static volatile uint64_t* data;
static volatile LONG stop_writer;

static DWORD WINAPI writer(LPVOID arg) {
    (void)arg;
    uint64_t i = 0;
    while (!stop_writer) {
        *data = ++i; // data writes on the code page, from another thread
    }
    return 0;
}

static double seconds_since(LARGE_INTEGER t0) {
    LARGE_INTEGER t1, freq;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    return (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
}

int main(int argc, char** argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 20000;
    page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!page) {
        printf("VirtualAlloc failed\n");
        return 2;
    }
    // big, at 0: xor eax, eax ; 300 x add eax, 1 ; ret (returns 300)
    uint8_t* b = page;
    *b++ = 0x31;
    *b++ = 0xC0;
    for (int i = 0; i < 300; i++) {
        *b++ = 0x83;
        *b++ = 0xC0;
        *b++ = 0x01;
    }
    *b++ = 0xC3;
    fn_t big = (fn_t)page;
    // small, at 1024: mov eax, imm32 ; ret (the rewrite checks patch its immediate at 1025)
    static const uint8_t mov_eax_ret[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
    memcpy(page + 1024, mov_eax_ret, sizeof(mov_eax_ret));
    fn_t f = (fn_t)(page + 1024);
    data = (volatile uint64_t*)(page + 2048);
    int failures = 0;

    // 0. Data only: the big function runs once (so the page holds translated code), then n writes with no call between.
    // Without replay only the first write faults; with replay every write faults: the time per write is a fault's cost.
    LARGE_INTEGER t0;
    big();
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < n; i++) {
        *data = (uint64_t)i;
    }
    double s0 = seconds_since(t0);
    printf("data only: %d writes in %.3f s, %.2f us each\n", n, s0, s0 * 1e6 / n);

    // 1. Data beside code: call the big function, then write the counter, n times.
    QueryPerformanceCounter(&t0);
    long long sum = 0;
    for (int i = 0; i < n; i++) {
        sum += big();
        *data = (uint64_t)i;
    }
    double s = seconds_since(t0);
    printf("data beside code: %d calls of a 300-instruction function and %d writes in %.3f s, %.1f us each; sum %lld (expect %lld), "
           "counter %llu (expect %d)\n",
           n, n, s, s * 1e6 / n, sum, 300LL * n, (unsigned long long)*data, n - 1);
    failures += sum != 300LL * n || *data != (uint64_t)(n - 1);

    // 2. Code rewritten alone: the new immediate must be returned.
    int stale = 0;
    for (int k = 2; k < 200; k++) {
        page[1025] = (uint8_t)k;
        if (f() != k) {
            stale++;
        }
    }
    printf("code rewritten 198 times: %d stale calls (expect 0)\n", stale);
    failures += stale != 0;

    // 3. Code rewritten while another thread writes data on the same page.
    HANDLE th = CreateThread(NULL, 0, writer, NULL, 0, NULL);
    stale = 0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < n; i++) {
        const int k = i & 0x7F;
        page[1025] = (uint8_t)k;
        if (f() != k) {
            stale++;
        }
    }
    s = seconds_since(t0);
    stop_writer = 1;
    WaitForSingleObject(th, INFINITE);
    printf("code rewritten %d times beside a data-writing thread in %.3f s: %d stale calls (expect 0), counter at %llu\n", n, s, stale,
           (unsigned long long)*data);
    failures += stale != 0;

    printf("%s\n", failures ? "FAILED" : "PASSED");
    fflush(stdout);
    // SMCDATA_PAUSE=<ms>: stay alive so FEX's statistics file (FEX_PROFILESTATS=1) can be read before the process exits
    if (getenv("SMCDATA_PAUSE")) {
        Sleep((DWORD)atoi(getenv("SMCDATA_PAUSE")));
    }
    return failures ? 1 : 0;
}
