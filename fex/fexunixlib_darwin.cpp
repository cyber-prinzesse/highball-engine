// Highball spike 2026-10-04: Darwin build of FEX Windows unix helper (Source/Windows/UnixLib/FEXUnixLib.cpp, MIT).
// Same function table and argument structs; prctl-based pieces replaced by their macOS equivalents.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_vm.h>
#include <cstring>

enum class FEXUnixLibFunctions : uint32_t { SetHardwareTSOControl, SetKernelUnalignedAtomicControl, Madvise, SetVMAName, GetSHMStatsVMA, DeleteSHMStatsFile, MapFile, GetPID, AliasWrite, MAX };
struct FEXUnixLib_SetHardwareTSOControlArgs { bool Enable; };
struct FEXUnixLib_SetKernelUnalignedAtomicControl { uint64_t Flags; };
struct FEXUnixLib_Madvise { const void* Addr; size_t Size; int32_t Advise; uint32_t pad; };
struct FEXUnixLib_SetVMAName { const void* Addr; size_t Size; const char* Name; };
struct FEXUnixLib_GetSHMStatsVMA { void* SHMBase; uint32_t MapSize; uint32_t MaxSize; };
struct FEXUnixLib_MapFile { int32_t FD; uint64_t MapSize; void* Result; };
struct FEXUnixLib_GetPID { uint32_t Result; };
struct FEXUnixLib_AliasWrite { void* Addr; const void* Data; uint32_t Size; uint32_t Pad; };

using NTSTATUS = int32_t;
using unixlib_entry_t = NTSTATUS (*)(void*);
constexpr NTSTATUS STATUS_SUCCESS = 0;
constexpr NTSTATUS STATUS_NOT_SUPPORTED = 0xC00000BB;
constexpr NTSTATUS STATUS_INTERNAL_ERROR = 0xC00000E5;
constexpr NTSTATUS STATUS_ACCESS_DENIED = 0xC0000022;

static void trace(const char* what, long a, long b) { if (getenv("FEXUNIX_TRACE")) fprintf(stderr, "fexunixlib: %s %lx %lx\n", what, a, b); }
static NTSTATUS TSOControl(void* _Args) {
  trace("TSOControl", ((const FEXUnixLib_SetHardwareTSOControlArgs*)_Args)->Enable, 0);
  auto Args = reinterpret_cast<const FEXUnixLib_SetHardwareTSOControlArgs*>(_Args);
  kern_return_t kr = thread_set_x86_64_compat(Args->Enable ? 1 : 0);   /* per thread, needs the cross-architecture entitlement */
  return kr == KERN_SUCCESS ? STATUS_SUCCESS : STATUS_NOT_SUPPORTED;
}
static NTSTATUS UnalignedAtomicControl(void* a) { trace("UnalignedAtomic", ((const FEXUnixLib_SetKernelUnalignedAtomicControl*)a)->Flags, 0); return STATUS_NOT_SUPPORTED; }
static NTSTATUS Madvise(void* _Args) {
  auto Args = reinterpret_cast<const FEXUnixLib_Madvise*>(_Args);
  int advise;
  trace("Madvise", (long)Args->Addr, Args->Advise);
  switch (Args->Advise) {
    case 4: advise = MADV_DONTNEED; break;
    case 8: advise = MADV_FREE; break;
    case 14: case 15: return STATUS_SUCCESS;
    default: if (Args->Advise >= 0 && Args->Advise <= 3) advise = Args->Advise; else return STATUS_SUCCESS;
  }
  return madvise(const_cast<void*>(Args->Addr), Args->Size, advise) == 0 ? STATUS_SUCCESS : STATUS_INTERNAL_ERROR;
}
static NTSTATUS SetVMAName(void* a) { trace("SetVMAName", (long)((const FEXUnixLib_SetVMAName*)a)->Addr, ((const FEXUnixLib_SetVMAName*)a)->Size); return STATUS_SUCCESS; }
static std::string StatsPath() { const char* t = getenv("TMPDIR"); return std::string(t ? t : "/tmp/") + "fex-" + std::to_string(getpid()) + "-stats"; }
static NTSTATUS GetSHMStatsVMA(void* _Args) {
  auto Args = reinterpret_cast<FEXUnixLib_GetSHMStatsVMA*>(_Args);
  trace("GetSHMStatsVMA", (long)Args->SHMBase, Args->MapSize);
  int fd = open(StatsPath().c_str(), O_CREAT | O_RDWR, 0600);
  if (fd == -1) return STATUS_INTERNAL_ERROR;
  if (ftruncate(fd, Args->MapSize) == -1) { close(fd); return STATUS_INTERNAL_ERROR; }
  if (!Args->SHMBase) {
    void* base = mmap(nullptr, Args->MaxSize, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) { close(fd); return STATUS_INTERNAL_ERROR; }
    Args->SHMBase = base;
  }
  void* shared = mmap(Args->SHMBase, Args->MapSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
  close(fd);
  return shared == MAP_FAILED ? STATUS_INTERNAL_ERROR : STATUS_SUCCESS;
}
static NTSTATUS DeleteSHMStatsFile(void*) { unlink(StatsPath().c_str()); return STATUS_SUCCESS; }
static NTSTATUS MapFile(void* _Args) {
  auto Args = reinterpret_cast<FEXUnixLib_MapFile*>(_Args);
  trace("MapFile", Args->FD, Args->MapSize);
  Args->Result = mmap(nullptr, Args->MapSize, PROT_READ, MAP_SHARED | MAP_NORESERVE, Args->FD, 0);
  close(Args->FD);
  if (Args->Result == MAP_FAILED) { Args->Result = nullptr; return STATUS_INTERNAL_ERROR; }
  return STATUS_SUCCESS;
}
// Highball smc-precise (2026-10-18): writes Size bytes at Addr through a second, writable mapping of the same memory, so
// the page keeps its read-execute protection for every thread. FEX replays a guest store into the data of a page that
// holds translated code this way: opening the page instead let another thread's code write land unseen and run stale
// (smcdata.exe, 1,960 stale calls of 2,000). Aligned 1, 2, 4 and 8 byte writes are single stores, as on x86.
static NTSTATUS AliasWrite(void* _Args) {
  auto Args = reinterpret_cast<const FEXUnixLib_AliasWrite*>(_Args);
  const mach_vm_address_t Address = reinterpret_cast<mach_vm_address_t>(Args->Addr);
  const mach_vm_address_t Page = Address & ~static_cast<mach_vm_address_t>(vm_page_size - 1);
  if (Address - Page + Args->Size > vm_page_size) return STATUS_NOT_SUPPORTED;
  mach_vm_address_t Alias = 0;
  vm_prot_t Cur, Max;
  kern_return_t kr = mach_vm_remap(mach_task_self(), &Alias, vm_page_size, 0, VM_FLAGS_ANYWHERE, mach_task_self(), Page, FALSE, &Cur, &Max,
                                   VM_INHERIT_NONE);
  if (kr != KERN_SUCCESS) { trace("AliasWrite remap", (long)Address, kr); return STATUS_INTERNAL_ERROR; }
  kr = mach_vm_protect(mach_task_self(), Alias, vm_page_size, FALSE, VM_PROT_READ | VM_PROT_WRITE);
  if (kr == KERN_SUCCESS) {
    void* Dst = reinterpret_cast<void*>(Alias + (Address - Page));
    const uintptr_t D = reinterpret_cast<uintptr_t>(Dst);
    switch (Args->Size) {
      case 1: __atomic_store_n(static_cast<uint8_t*>(Dst), *static_cast<const uint8_t*>(Args->Data), __ATOMIC_SEQ_CST); break;
      case 2: if (D % 2 == 0) { uint16_t v; memcpy(&v, Args->Data, 2); __atomic_store_n(static_cast<uint16_t*>(Dst), v, __ATOMIC_SEQ_CST); } else { memcpy(Dst, Args->Data, 2); } break;
      case 4: if (D % 4 == 0) { uint32_t v; memcpy(&v, Args->Data, 4); __atomic_store_n(static_cast<uint32_t*>(Dst), v, __ATOMIC_SEQ_CST); } else { memcpy(Dst, Args->Data, 4); } break;
      case 8: if (D % 8 == 0) { uint64_t v; memcpy(&v, Args->Data, 8); __atomic_store_n(static_cast<uint64_t*>(Dst), v, __ATOMIC_SEQ_CST); } else { memcpy(Dst, Args->Data, 8); } break;
      default: memcpy(Dst, Args->Data, Args->Size); __atomic_thread_fence(__ATOMIC_SEQ_CST); break;
    }
  } else {
    trace("AliasWrite protect", (long)Address, kr);
  }
  mach_vm_deallocate(mach_task_self(), Alias, vm_page_size);
  return kr == KERN_SUCCESS ? STATUS_SUCCESS : STATUS_ACCESS_DENIED;
}
static NTSTATUS GetPID(void* _Args) { trace("GetPID", 0, 0); reinterpret_cast<FEXUnixLib_GetPID*>(_Args)->Result = getpid(); return STATUS_SUCCESS; }

extern "C" __attribute__((visibility("default"))) const unixlib_entry_t __wine_unix_call_funcs[] = {
  TSOControl, UnalignedAtomicControl, Madvise, SetVMAName, GetSHMStatsVMA, DeleteSHMStatsFile, MapFile, GetPID, AliasWrite,
};
static_assert(sizeof(__wine_unix_call_funcs) / sizeof(__wine_unix_call_funcs[0]) == static_cast<uint32_t>(FEXUnixLibFunctions::MAX));
