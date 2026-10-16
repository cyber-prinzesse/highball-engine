/* mfdeadlock.c (2026-10-16): does IMFMediaEngine hold its own lock while it calls the application's
 * IMFMediaEngineNotify::EventNotify?  Wine 11.18's mfmediaengine does (engine->cs around every EventNotify, on the
 * caller's thread in Play/Pause/SetSource/... and on the MF work-queue thread for session events).  An application
 * whose callback takes a lock of its own, while another of its threads holds that lock and calls any engine method,
 * then deadlocks: the shape of the MSFS 2024 freeze at the intro video (boot61 b-190445, three threads in
 * RtlpWaitForCriticalSection, one of the sections unnamed and heap-allocated like the engine's).
 *
 * The test: the main thread holds lock R and sets the source; the first EventNotify that arrives on another thread
 * signals and then takes R (blocking while main holds it); main then has a helper thread call GetCurrentTime.  If the
 * engine holds its lock around the callback, the helper never returns while main holds R: DEADLOCK (exit 1).  If the
 * helper returns within 5 s: OK (exit 0).  No cross-thread event within 15 s: INCONCLUSIVE (exit 2).  Audio-only
 * engine, a 1 s silent WAV written next to the exe.
 *
 *   x86_64-w64-mingw32-clang -O1 mfdeadlock.c -o mfdeadlock.exe -lole32 -loleaut32 -lmfplat -lmf -lmfuuid -luuid
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <initguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfmediaengine.h>
#include <stdio.h>
#include <string.h>

static CRITICAL_SECTION R;
static HANDLE incb, cbdone, getdone;
static LONG armed = 1;
static DWORD main_tid;
static IMFMediaEngine *engine;

struct notify { IMFMediaEngineNotify IMFMediaEngineNotify_iface; LONG ref; };

static HRESULT WINAPI notify_QueryInterface(IMFMediaEngineNotify *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IMFMediaEngineNotify))
    {
        *out = iface;
        IMFMediaEngineNotify_AddRef(iface);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI notify_AddRef(IMFMediaEngineNotify *iface)
{
    struct notify *n = CONTAINING_RECORD(iface, struct notify, IMFMediaEngineNotify_iface);
    return InterlockedIncrement(&n->ref);
}
static ULONG WINAPI notify_Release(IMFMediaEngineNotify *iface)
{
    struct notify *n = CONTAINING_RECORD(iface, struct notify, IMFMediaEngineNotify_iface);
    return InterlockedDecrement(&n->ref);
}
static HRESULT WINAPI notify_EventNotify(IMFMediaEngineNotify *iface, DWORD event, DWORD_PTR p1, DWORD p2)
{
    DWORD tid = GetCurrentThreadId();
    printf("  event %lu (%Iu, %lu) on thread %lu%s\n", (unsigned long)event, (size_t)p1, (unsigned long)p2,
           (unsigned long)tid, tid == main_tid ? " (main)" : "");
    fflush(stdout);
    if (tid != main_tid && InterlockedExchange(&armed, 0))
    {
        printf("  callback on thread %lu takes the application lock R...\n", (unsigned long)tid); fflush(stdout);
        SetEvent(incb);
        EnterCriticalSection(&R);
        LeaveCriticalSection(&R);
        printf("  callback got R and released it\n"); fflush(stdout);
        SetEvent(cbdone);
    }
    return S_OK;
}
static const IMFMediaEngineNotifyVtbl notify_vtbl = { notify_QueryInterface, notify_AddRef, notify_Release, notify_EventNotify };

static DWORD WINAPI getter(void *arg)
{
    double t = IMFMediaEngine_GetCurrentTime(engine);
    printf("  helper: GetCurrentTime returned %f\n", t); fflush(stdout);
    SetEvent(getdone);
    return 0;
}

static int write_wav(const WCHAR *path)
{
    const int rate = 8000, seconds = 1;
    DWORD datalen = rate * seconds * 2, n;
    BYTE hdr[44] = "RIFF....WAVEfmt ";
    HANDLE f;
    DWORD v;
    v = 36 + datalen; memcpy(hdr + 4, &v, 4);
    v = 16; memcpy(hdr + 16, &v, 4);                 /* fmt chunk size */
    hdr[20] = 1; hdr[21] = 0;                        /* PCM */
    hdr[22] = 1; hdr[23] = 0;                        /* mono */
    v = rate; memcpy(hdr + 24, &v, 4);
    v = rate * 2; memcpy(hdr + 28, &v, 4);
    hdr[32] = 2; hdr[33] = 0; hdr[34] = 16; hdr[35] = 0;
    memcpy(hdr + 36, "data", 4); memcpy(hdr + 40, &datalen, 4);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    WriteFile(f, hdr, 44, &n, NULL);
    {
        BYTE *z = calloc(datalen, 1);
        WriteFile(f, z, datalen, &n, NULL);
        free(z);
    }
    CloseHandle(f);
    return 1;
}

int main(void)
{
    IMFMediaEngineClassFactory *factory = NULL;
    IMFAttributes *attr = NULL;
    struct notify n = { { &notify_vtbl }, 1 };
    WCHAR wav[MAX_PATH], full[MAX_PATH];
    BSTR src;
    HRESULT hr;
    HANDLE t;
    int rc;

    main_tid = GetCurrentThreadId();
    InitializeCriticalSection(&R);
    incb = CreateEventW(NULL, TRUE, FALSE, NULL);
    cbdone = CreateEventW(NULL, TRUE, FALSE, NULL);
    getdone = CreateEventW(NULL, TRUE, FALSE, NULL);

    GetTempPathW(MAX_PATH, wav); lstrcatW(wav, L"mfdeadlock.wav");
    if (!write_wav(wav)) { printf("cannot write %ls\n", wav); return 3; }
    GetFullPathNameW(wav, MAX_PATH, full, NULL);

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) { printf("MFStartup %#lx\n", hr); return 3; }
    hr = CoCreateInstance(&CLSID_MFMediaEngineClassFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IMFMediaEngineClassFactory, (void **)&factory);
    if (FAILED(hr)) { printf("MFMediaEngineClassFactory %#lx\n", hr); return 3; }
    MFCreateAttributes(&attr, 2);
    IMFAttributes_SetUnknown(attr, &MF_MEDIA_ENGINE_CALLBACK, (IUnknown *)&n.IMFMediaEngineNotify_iface);
    hr = IMFMediaEngineClassFactory_CreateInstance(factory, MF_MEDIA_ENGINE_AUDIOONLY, attr, &engine);
    if (FAILED(hr)) { printf("CreateInstance %#lx\n", hr); return 3; }
    printf("engine created, main thread %lu, source %ls\n", (unsigned long)main_tid, full); fflush(stdout);

    src = SysAllocString(full);
    EnterCriticalSection(&R);
    printf("main holds R and sets the source\n"); fflush(stdout);
    hr = IMFMediaEngine_SetSource(engine, src);
    printf("SetSource %#lx\n", hr); fflush(stdout);
    if (WaitForSingleObject(incb, 15000) != WAIT_OBJECT_0)
    {
        printf("INCONCLUSIVE: no event on another thread within 15 s\n");
        LeaveCriticalSection(&R);
        rc = 2;
    }
    else
    {
        printf("main: the callback thread is waiting for R; helper calls GetCurrentTime\n"); fflush(stdout);
        t = CreateThread(NULL, 0, getter, NULL, 0, NULL);
        if (WaitForSingleObject(getdone, 5000) == WAIT_OBJECT_0)
        {
            printf("OK: an engine call does not wait for the application's callback\n");
            rc = 0;
        }
        else
        {
            printf("DEADLOCK: GetCurrentTime is blocked while the callback thread waits for R (the engine holds its lock around EventNotify)\n");
            rc = 1;
        }
        fflush(stdout);
        LeaveCriticalSection(&R);
        WaitForSingleObject(cbdone, 5000);
        WaitForSingleObject(t, 5000);
        CloseHandle(t);
    }
    IMFMediaEngine_Shutdown(engine);
    IMFMediaEngine_Release(engine);
    IMFAttributes_Release(attr);
    IMFMediaEngineClassFactory_Release(factory);
    SysFreeString(src);
    MFShutdown();
    CoUninitialize();
    DeleteFileW(wav);
    printf("exit %d\n", rc);
    return rc;
}
