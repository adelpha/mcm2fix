// Crash reporting (on whenever Log=1): if the game crashes or quits on its own,
// the log says where and why, and a small crash dump is written next to it.
//
// - Faults (access violation, illegal instruction, divide by zero, heap
//   corruption) are logged as they happen with the registers, the module and
//   offset of the faulting code, and the return addresses found on the stack.
//   The game may handle some of these itself, so the first few are logged and
//   the rest counted.
// - An unhandled fault also writes logs\mcm2fix-<date>-<time>.dmp (dbghelp's
//   MiniDumpWriteDump, loaded when needed).
// - The game's ExitProcess call is logged with its exit code and caller, so a
//   quit to the desktop that is not a crash is told apart from one that is.
// - Memory use (address space used, largest free block) and a heap check are
//   logged at each display mode change (menus <-> race).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "config.h"
#include "patch.h"
#include "xp_compat.h"
#include "dd/surface.h"
#include <malloc.h>

#if defined(_MSC_VER)
#include <intrin.h>
#define CRASH_RET() _ReturnAddress()
#define CRASH_FRAME() _AddressOfReturnAddress()
#else
#define CRASH_RET() __builtin_return_address(0)
#define CRASH_FRAME() __builtin_frame_address(0)
#endif

void SmoothMemoryLog();
void SmoothPartsMemoryLog();
void TextureNamesMemoryLog();
namespace dd {
void GpuTextMemoryLog();
void HudTextMemoryLog();
void ReplacementMemoryLog();
void GpuMemoryLog();
}  // namespace dd

namespace power {
void Shutdown(const char* why);
}

namespace crash {
namespace {

LONG g_faults = 0;
constexpr LONG kLogFaults = 8;
LONG g_dumped = 0;
using ExitProcessFn = void(WINAPI*)(UINT);
ExitProcessFn g_exit = nullptr;
LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;

// "Mcm2.exe+1a2b3" (or the bare address if no module holds it).
void Where(uintptr_t a, char* out, size_t cap) {
  HMODULE m = nullptr;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(a), &m) && m) {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');
    name = name ? name + 1 : path;
    std::snprintf(out, cap, "%08x (%s+%x)", static_cast<unsigned>(a), name,
                  static_cast<unsigned>(a - reinterpret_cast<uintptr_t>(m)));
  } else {
    std::snprintf(out, cap, "%08x", static_cast<unsigned>(a));
  }
}

bool IsCode(uintptr_t a) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (!VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi))) return false;
  if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return false;
  return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Return addresses (values on the stack that point into code), newest first.
void LogStack(uintptr_t esp, const char* tag) {
  const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
  const uintptr_t top = reinterpret_cast<uintptr_t>(tib->StackBase);
  if (esp < reinterpret_cast<uintptr_t>(tib->StackLimit) || esp >= top) {
    patch::Log("%s: stack pointer %08x outside this thread's stack", tag, static_cast<unsigned>(esp));
    return;
  }
  int shown = 0;
  for (uintptr_t p = esp; p + 4 <= top && p < esp + 4096 && shown < 24; p += 4) {
    const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
    if (v < 0x10000 || !IsCode(v)) continue;
    char w[160];
    Where(v, w, sizeof(w));
    patch::Log("%s:   [esp+%03x] %s", tag, static_cast<unsigned>(p - esp), w);
    ++shown;
  }
}

const char* CodeName(DWORD c) {
  switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "access violation";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
    case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
    case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
    case 0xC0000374: return "heap corruption";
    default: return nullptr;
  }
}

void LogFault(const char* tag, EXCEPTION_POINTERS* ep) {
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  const CONTEXT* c = ep->ContextRecord;
  const char* name = CodeName(er->ExceptionCode);
  char at[160];
  Where(reinterpret_cast<uintptr_t>(er->ExceptionAddress), at, sizeof(at));
  char extra[96] = {};
  if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
    const ULONG_PTR kind = er->ExceptionInformation[0];
    std::snprintf(extra, sizeof(extra), ", %s %08x", kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                  static_cast<unsigned>(er->ExceptionInformation[1]));
  }
  patch::Log("%s: %s (%08lx%s) at %s, thread %lu", tag, name ? name : "exception", er->ExceptionCode, extra, at,
             GetCurrentThreadId());
  if (er->ExceptionCode == EXCEPTION_STACK_OVERFLOW) return;  // little stack left to work with
  patch::Log("%s: eax=%08lx ebx=%08lx ecx=%08lx edx=%08lx esi=%08lx edi=%08lx ebp=%08lx esp=%08lx", tag, c->Eax,
             c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
  LogStack(c->Esp, tag);
}

LONG CALLBACK FaultHandler(EXCEPTION_POINTERS* ep) {
  if (!CodeName(ep->ExceptionRecord->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
  const LONG n = InterlockedIncrement(&g_faults);
  if (n <= kLogFaults) {
    LogFault("fault", ep);
    if (n == 1) patch::Log("fault: (logged as it happens; it may still be handled and harmless, e.g. a pointer check)");
    if (n == kLogFaults) patch::Log("fault: further faults are only counted");
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

std::string LogDir() {
  const std::string local = xp::LocalAppData();
  return local.empty() ? std::string() : local + "\\Motocross Madness 2\\logs";
}

void WriteDump(EXCEPTION_POINTERS* ep) {
  if (InterlockedIncrement(&g_dumped) != 1) return;
  const std::string dir = LogDir();
  if (dir.empty()) return;
  HMODULE dbg = LoadLibraryA("dbghelp.dll");
  using DumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, DWORD, void*, void*, void*);
  auto dump = dbg ? reinterpret_cast<DumpFn>(reinterpret_cast<void*>(GetProcAddress(dbg, "MiniDumpWriteDump"))) : nullptr;
  if (!dump) {
    patch::Log("crash: no dbghelp.dll MiniDumpWriteDump; no crash dump");
    return;
  }
  SYSTEMTIME st;
  GetLocalTime(&st);
  char path[MAX_PATH];
  std::snprintf(path, sizeof(path), "%s\\mcm2fix-%04u%02u%02u-%02u%02u%02u.dmp", dir.c_str(), st.wYear, st.wMonth,
                st.wDay, st.wHour, st.wMinute, st.wSecond);
  HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    patch::Log("crash: could not create %s", path);
    return;
  }
  struct {
    DWORD thread;
    EXCEPTION_POINTERS* ep;
    BOOL client;
  } info = {GetCurrentThreadId(), ep, FALSE};
  // MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs
  const DWORD type = 0x00000000 | 0x00000040 | 0x00000001;
  const BOOL ok = dump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &info, nullptr, nullptr);
  CloseHandle(f);
  patch::Log("crash: dump %s %s", path, ok ? "written" : "FAILED");
}

void LogMemoryNow(const char* why);

LONG WINAPI UnhandledFilter(EXCEPTION_POINTERS* ep) {
  patch::Log("crash: UNHANDLED - the game is about to close");
  LogFault("crash", ep);
  LogMemoryNow("at the crash");
  WriteDump(ep);
  power::Shutdown("crash");
  return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void WINAPI HookedExitProcess(UINT code) {
  const uintptr_t ret = reinterpret_cast<uintptr_t>(CRASH_RET());
  char w[160];
  Where(ret, w, sizeof(w));
  const char* name = CodeName(code);
  patch::Log("exit: the game called ExitProcess(%u = %08x%s%s) from %s; %ld faults seen", code, code,
             name ? ", " : "", name ? name : "", w, g_faults);
  if (name || code >= 0xC0000000u) LogStack(reinterpret_cast<uintptr_t>(CRASH_FRAME()), "exit");
  LogMemoryNow("at exit");
  power::Shutdown("game closing");
  (g_exit ? g_exit : &ExitProcess)(code);
}

// The game's import slot for dll!fn (0 if it imports no such function).
uintptr_t FindIat(const char* dll, const char* fn) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
  auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return 0;
  for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
    if (lstrcmpiA(reinterpret_cast<const char*>(base + d->Name), dll) != 0) continue;
    const DWORD names = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
    auto* n = reinterpret_cast<const IMAGE_THUNK_DATA32*>(base + names);
    auto* slot = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + d->FirstThunk);
    for (; n->u1.AddressOfData; ++n, ++slot) {
      if (n->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
      auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + n->u1.AddressOfData);
      if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), fn) == 0)
        return reinterpret_cast<uintptr_t>(&slot->u1.Function);
    }
  }
  return 0;
}

void LogMemoryNow(const char* why) {
  uintptr_t used = 0, committed = 0, largest = 0, free_total = 0;
  uintptr_t priv = 0, mapped = 0, image = 0, reserved = 0;
  unsigned regions = 0;
  MEMORY_BASIC_INFORMATION mbi{};
  for (uintptr_t a = 0x10000; a < 0x7FFF0000u && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi));) {
    const uintptr_t size = mbi.RegionSize;
    if (mbi.State == MEM_FREE) {
      free_total += size;
      if (size > largest) largest = size;
    } else {
      used += size;
      ++regions;
      if (mbi.State == MEM_COMMIT) {
        committed += size;
        if (mbi.Type == MEM_PRIVATE) priv += size;
        else if (mbi.Type == MEM_MAPPED) mapped += size;
        else image += size;
      } else {
        reserved += size;
      }
    }
    const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + size;
    if (next <= a) break;
    a = next;
  }
  patch::Log("memory %s: address space used %u MB (committed %u MB: private %u, mapped %u, modules %u; reserved %u; "
             "%u regions), free %u MB, largest free block %u MB",
             why, static_cast<unsigned>(used >> 20), static_cast<unsigned>(committed >> 20),
             static_cast<unsigned>(priv >> 20), static_cast<unsigned>(mapped >> 20), static_cast<unsigned>(image >> 20),
             static_cast<unsigned>(reserved >> 20), regions, static_cast<unsigned>(free_total >> 20),
             static_cast<unsigned>(largest >> 20));
}

// Each heap: how much is in use, in how many blocks (whose heap grows).
void LogHeaps() {
  HANDLE heaps[64];
  const DWORD n = GetProcessHeaps(64, heaps);
  const DWORD t0 = GetTickCount();
  DWORD bad = 0;
  const HANDLE crt = reinterpret_cast<HANDLE>(_get_heap_handle());
  for (DWORD i = 0; i < n && i < 64; ++i) {
    if (!HeapValidate(heaps[i], 0, nullptr)) {
      ++bad;
      patch::Log("memory: heap %p FAILED its check (corrupted)", heaps[i]);
      continue;
    }
    uint64_t busy = 0, region = 0;
    unsigned blocks = 0;
    if (HeapLock(heaps[i])) {
      PROCESS_HEAP_ENTRY e{};
      while (HeapWalk(heaps[i], &e)) {
        if (e.wFlags & PROCESS_HEAP_ENTRY_BUSY) {
          busy += e.cbData;
          ++blocks;
        } else if (e.wFlags & PROCESS_HEAP_REGION) {
          region += e.Region.dwCommittedSize;
        }
      }
      HeapUnlock(heaps[i]);
    }
    if (busy >= (1u << 20) || i == 0)
      patch::Log("memory: heap %p%s%s: %u MB in use in %u blocks (%u MB committed in regions)", heaps[i],
                 heaps[i] == GetProcessHeap() ? " (process)" : "", heaps[i] == crt ? " (mcm2fix)" : "",
                 static_cast<unsigned>(busy >> 20), blocks, static_cast<unsigned>(region >> 20));
  }
  patch::Log("memory: %lu heaps checked in %lu ms, %lu bad; %ld faults so far", n, GetTickCount() - t0, bad,
             g_faults);
}

}  // namespace

// Called at each display mode change: memory use, every heap, and what
// mcm2fix itself holds (to find what grows from race to race).
void OnModeChange(DWORD w, DWORD h) {
  if (!GetConfig().log) return;
  static unsigned changes = 0;
  char why[48];
  std::snprintf(why, sizeof(why), "at %lux%lu (#%u)", w, h, ++changes);
  LogMemoryNow(why);
  LogHeaps();
  dd::Surface::MemoryLog();
  dd::GpuTextMemoryLog();
  dd::HudTextMemoryLog();
  dd::ReplacementMemoryLog();
  dd::GpuMemoryLog();
  SmoothMemoryLog();
  SmoothPartsMemoryLog();
  TextureNamesMemoryLog();
}

void Apply() {
  if (!GetConfig().log) return;
  const bool veh = AddVectoredExceptionHandler(0, &FaultHandler) != nullptr;
  g_prev_filter = SetUnhandledExceptionFilter(&UnhandledFilter);
  const uintptr_t slot = FindIat("kernel32.dll", "ExitProcess");
  if (slot) g_exit = reinterpret_cast<ExitProcessFn>(patch::HookIat(slot, reinterpret_cast<void*>(&HookedExitProcess)));
  patch::Log("crash log: fault log %s, crash dump on, ExitProcess %s", veh ? "on" : "FAILED",
             slot ? "hooked" : "not found");
  LogMemoryNow("at start");
}

}  // namespace crash
