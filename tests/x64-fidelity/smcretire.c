// smcretire.c (Highball, 2026-10-18): code that is decrypted, run and encrypted again, the pattern of the copy protection
// in Microsoft Flight Simulator 2024 that keeps FEX translating the same functions. One RWX page holds a 300-instruction
// function; each round XORs its bytes back to plain code, calls it, and XORs them to garbage again. Before smc-retire
// FEX translated the function on every round; with it, the translation comes back when the plain bytes do. Every call's
// result is checked, so a translation brought back for bytes that differ shows up as a stale call. Further rounds mix
// several versions, a second thread writing data on the page, and a protection change between rounds.
// Build: tests/x64-fidelity/build.sh
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*fn_t)(void);

enum { BODY = 2 + 300 * 3 + 1 };
static uint8_t* page;
static volatile uint64_t* data;
static volatile LONG stop_writer;

// version k: mov eax, k ; 300 x add eax, 1 ; ret (returns k + 300)
static void write_plain(uint8_t* p, uint32_t k) {
    *p++ = 0xB8;
    memcpy(p, &k, 4);
    p += 4;
    for (int i = 0; i < 300; i++) {
        *p++ = 0x83;
        *p++ = 0xC0;
        *p++ = 0x01;
    }
    *p = 0xC3;
}

static void xor_body(uint8_t* p, uint8_t key) {
    for (int i = 0; i < 5 + 300 * 3 + 1; i++) {
        p[i] ^= key;
    }
}

static DWORD WINAPI writer(LPVOID arg) {
    (void)arg;
    uint64_t i = 0;
    while (!stop_writer) {
        *data = ++i;
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
    const int n = argc > 1 ? atoi(argv[1]) : 5000;
    page = VirtualAlloc(NULL, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!page) {
        printf("VirtualAlloc failed\n");
        return 2;
    }
    fn_t f = (fn_t)page;
    data = (volatile uint64_t*)(page + 2048);
    int failures = 0;
    LARGE_INTEGER t0;

    // 1. Decrypt, run, encrypt: one version. With smc-retire every round after the first brings the translation back.
    write_plain(page, 7);
    xor_body(page, 0x5A);
    int stale = 0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < n; i++) {
        xor_body(page, 0x5A);
        if (f() != 307) {
            stale++;
        }
        xor_body(page, 0x5A);
    }
    double s = seconds_since(t0);
    printf("decrypt, run, encrypt: %d rounds in %.3f s, %.1f us each, %d stale calls (expect 0)\n", n, s, s * 1e6 / n, stale);
    failures += stale != 0;

    // 2. Eight versions in turn (more than a set keeps): every result must be its own version's.
    stale = 0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < n; i++) {
        const uint32_t k = (uint32_t)(i & 7);
        write_plain(page, k);
        if (f() != (int)k + 300) {
            stale++;
        }
    }
    s = seconds_since(t0);
    printf("eight versions in turn: %d rounds in %.3f s, %.1f us each, %d stale calls (expect 0)\n", n, s, s * 1e6 / n, stale);
    failures += stale != 0;

    // 3. Two versions in turn: both stay kept, so after the first two rounds nothing is translated.
    stale = 0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < n; i++) {
        const uint32_t k = 40 + (uint32_t)(i & 1);
        page[1] = (uint8_t)k;
        if (f() != (int)k + 300) {
            stale++;
        }
    }
    s = seconds_since(t0);
    printf("two versions in turn: %d rounds in %.3f s, %.1f us each, %d stale calls (expect 0)\n", n, s, s * 1e6 / n, stale);
    failures += stale != 0;

    // 4. Rounds 1 and 3 again beside a thread writing data on the same page.
    HANDLE th = CreateThread(NULL, 0, writer, NULL, 0, NULL);
    write_plain(page, 7);
    xor_body(page, 0x5A);
    stale = 0;
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < n; i++) {
        xor_body(page, 0x5A);
        if (f() != 307) {
            stale++;
        }
        xor_body(page, 0x5A);
        xor_body(page, 0x5A);
        const uint32_t k = 40 + (uint32_t)(i & 1);
        page[1] = (uint8_t)k;
        if (f() != (int)k + 300) {
            stale++;
        }
        page[1] = 7;
        xor_body(page, 0x5A);
    }
    s = seconds_since(t0);
    stop_writer = 1;
    WaitForSingleObject(th, INFINITE);
    printf("beside a data-writing thread: %d rounds in %.3f s, %d stale calls (expect 0), counter at %llu\n", n, s, stale,
           (unsigned long long)*data);
    failures += stale != 0;

    // 5. A protection change between rounds (read-execute, then read-write-execute again): kept translations on the page
    // are dropped, so each round translates again; results stay right.
    xor_body(page, 0x5A);
    stale = 0;
    QueryPerformanceCounter(&t0);
    const int m = n / 10 ? n / 10 : 1;
    for (int i = 0; i < m; i++) {
        DWORD old;
        xor_body(page, 0x5A);
        VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old);
        VirtualProtect(page, 4096, PAGE_EXECUTE_READWRITE, &old);
        xor_body(page, 0x5A);
        if (f() != 307) {
            stale++;
        }
    }
    s = seconds_since(t0);
    printf("protection change between rounds: %d rounds in %.3f s, %.1f us each, %d stale calls (expect 0)\n", m, s,
           s * 1e6 / m, stale);
    failures += stale != 0;

    printf("%s\n", failures ? "FAILED" : "PASSED");
    fflush(stdout);
    // SMCRETIRE_PAUSE=<ms>: stay alive so FEX's statistics file (FEX_PROFILESTATS=1) can be read before the process exits
    if (getenv("SMCRETIRE_PAUSE")) {
        Sleep((DWORD)atoi(getenv("SMCRETIRE_PAUSE")));
    }
    return failures ? 1 : 0;
}
