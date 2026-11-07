// Helpers for patching the game's code in memory.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace patch {

// True if the bytes at addr match expected.
bool Matches(uintptr_t addr, const void* expected, size_t len);

// Overwrite len bytes at addr (makes the page writable, flushes the I-cache).
void Write(uintptr_t addr, const void* data, size_t len);

// Write "call target" (5 bytes) at addr and pad with NOPs up to total_len.
void Call(uintptr_t addr, const void* target, size_t total_len);

// Write "jmp target" (5 bytes) at addr and pad with NOPs up to total_len.
void Jump(uintptr_t addr, uintptr_t target, size_t total_len);

// Check, then patch. Returns false (and logs) if the original bytes differ.
bool Replace(const char* name, uintptr_t addr, const void* original, size_t len,
             const void* replacement, size_t replacement_len);

void Log(const char* fmt, ...);

}  // namespace patch
