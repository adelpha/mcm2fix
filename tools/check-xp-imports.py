#!/usr/bin/env python3
"""Fails if mcm2fix.dll imports anything Windows XP (SP3) does not have.

    tools/check-xp-imports.py build/mcm2fix.dll [objdump]

Uses MinGW objdump to list the import table. The check is a deny list: DLLs
that do not exist on XP, Vista+ functions of the system DLLs mcm2fix uses, and
the msvcrt "secure" (_s) functions that only Vista's msvcrt.dll exports. Load
anything newer with LoadLibrary/GetProcAddress instead (see src/xp_compat.cpp).
"""
import re
import subprocess
import sys

NO_XP_DLLS = {
    "d3d11.dll", "d3d12.dll", "dxgi.dll", "d3dcompiler_47.dll", "bcrypt.dll", "ncrypt.dll",
    "dwmapi.dll", "ucrtbase.dll", "vcruntime140.dll", "msvcp140.dll", "propsys.dll",
    "shcore.dll", "dcomp.dll", "mfplat.dll", "avrt.dll", "synchronization.dll",
}

NO_XP_FUNCS = {
    # kernel32 (Vista+)
    "GetTickCount64", "InitializeConditionVariable", "SleepConditionVariableCS",
    "SleepConditionVariableSRW", "WakeConditionVariable", "WakeAllConditionVariable",
    "InitializeSRWLock", "AcquireSRWLockExclusive", "AcquireSRWLockShared",
    "ReleaseSRWLockExclusive", "ReleaseSRWLockShared", "TryAcquireSRWLockExclusive",
    "InitOnceBeginInitialize", "InitOnceComplete", "InitOnceExecuteOnce",
    "CreateWaitableTimerExW", "CreateEventExW", "CreateEventExA", "CreateMutexExW",
    "CreateSemaphoreExW", "GetThreadId", "GetProcessId", "QueryFullProcessImageNameW",
    "GetFinalPathNameByHandleW", "SetFileInformationByHandle", "GetFileInformationByHandleEx",
    "CancelIoEx", "GetLocaleInfoEx", "LCIDToLocaleName", "LocaleNameToLCID", "CompareStringEx",
    "GetUserDefaultLocaleName", "FlsAlloc", "FlsFree", "FlsGetValue", "FlsSetValue",
    "InitializeCriticalSectionEx", "SetThreadErrorMode", "GetErrorMode", "Wow64DisableWow64FsRedirection",
    "GetSystemTimePreciseAsFileTime", "QueryUnbiasedInterruptTime", "SetDefaultDllDirectories",
    "AddDllDirectory", "CreateSymbolicLinkW", "GetCurrentProcessorNumber",
    # advapi32 (Vista+)
    "RegCopyTreeA", "RegCopyTreeW", "RegDeleteTreeA", "RegDeleteTreeW", "RegGetValueA",
    "RegGetValueW", "RegDeleteKeyExA", "RegDeleteKeyExW", "RegSetKeyValueA", "RegSetKeyValueW",
    # shell32 / user32 (Vista+)
    "SHGetKnownFolderPath", "SetProcessDPIAware", "SetProcessDpiAwarenessContext",
    "GetDpiForWindow", "GetDpiForSystem", "RegisterTouchWindow",
}


def imports(dll, objdump):
    out = subprocess.run([objdump, "-p", dll], check=True, capture_output=True, text=True).stdout
    current = None
    for line in out.splitlines():
        m = re.match(r"\s*DLL Name:\s*(\S+)", line)
        if m:
            current = m.group(1).lower()
            yield current, None
            continue
        m = re.match(r"\s+[0-9a-f]+\s+\d+\s+(\S+)\s*$", line)
        if current and m:
            yield current, m.group(1)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    objdump = sys.argv[2] if len(sys.argv) > 2 else "i686-w64-mingw32-objdump"
    bad = []
    count = 0
    for dll, func in imports(sys.argv[1], objdump):
        if func is None:
            if dll in NO_XP_DLLS or dll.startswith("api-ms-win-"):
                bad.append(f"{dll} (DLL not on XP)")
            continue
        count += 1
        if func in NO_XP_FUNCS:
            bad.append(f"{dll}!{func} (Vista+)")
        elif dll == "msvcrt.dll" and func.endswith("_s"):
            bad.append(f"msvcrt.dll!{func} (not in XP's msvcrt)")
    if bad:
        print("Windows XP import check FAILED:")
        for b in bad:
            print("  " + b)
        return 1
    print(f"Windows XP import check passed ({count} imports)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
