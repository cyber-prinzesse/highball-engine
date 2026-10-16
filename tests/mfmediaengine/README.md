# Media engine tests

`mfdeadlock.c`: does IMFMediaEngine hold a lock of its own while it calls the application's
IMFMediaEngineNotify::EventNotify? The main thread holds a lock R and sets the source; the first event that arrives
on another thread takes R inside the callback; a helper thread then calls GetCurrentTime. On Windows the helper
returns at once (OK, exit 0). On Wine 11.18 as shipped the helper waits behind the callback thread, which holds
engine->cs while it waits for R (DEADLOCK, exit 1): the shape of Microsoft Flight Simulator 2024's freeze at its intro
video, fixed by `patches-arm64/0040`. No cross-thread event within 15 s is INCONCLUSIVE (exit 2). Audio-only engine, a
1 s silent WAV written to the temp folder.

```
x86_64-w64-mingw32-clang -O1 mfdeadlock.c -o mfdeadlock.exe -lole32 -loleaut32 -lmfplat -lmf -lmfuuid -luuid
wine mfdeadlock.exe
```
