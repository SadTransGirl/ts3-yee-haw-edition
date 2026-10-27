// patch_util.cpp -- the generic code-patch helpers every harness module uses: the exe's base,
// checking bytes and RIP-relative operands before patching, writing code or data, redirecting
// a call through a stub allocated near it, and hooking an import in an import address table.
//
// How the modules use them. A patch target is given as an RVA: an offset from the base the
// game's exe is loaded at. The decomp session (the reverse-engineering notes in ..\re) reads
// the exe at its preferred base 0x140000000, so an address written 0x14027ac30 in a comment
// is RVA 0x27ac30 here, and ExeBase() + rva is where it is in this process. Every patch first
// checks that the bytes it expects are really there (BytesAt, RipRefersTo, or RedirectCall's
// own check): that is the build check, so a different build of the game, or code another mod
// already changed, is left alone and the patch reports "doesn't match this build" instead.
// Most callers run at start-up (DllMain in xinput_proxy.cpp and the Install* functions it
// calls), before the game's own code has run; a few patches are switched on and off later.
// No other thread is paused while a patch is written.

#include "proxy_shared.h"

// The game exe's base address in this process (the module the process was started from).
BYTE* ExeBase() { return (BYTE*)GetModuleHandleW(NULL); }

// True if the exe holds exactly these n bytes at rva: the build check made before a patch
// writes. An unreadable address counts as a mismatch rather than a crash.
bool BytesAt(DWORD rva, const void* expect, size_t n) {
    __try {
        return memcmp(ExeBase() + rva, expect, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// True if the instruction at insnRva (insnLen bytes, disp32 at dispOff)
// addresses targetRva.
// x64 code reaches globals and calls functions through a 32-bit displacement relative to
// the next instruction, so the bytes of such an instruction depend on where it sits; this
// checks where it points instead. Also works for "call rel32" (dispOff 1, insnLen 5).
bool RipRefersTo(DWORD insnRva, size_t dispOff, size_t insnLen, DWORD targetRva) {
    __try {
        BYTE* p = ExeBase() + insnRva;
        return p + insnLen + *(int32_t*)(p + dispOff) == ExeBase() + targetRva;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Copies n bytes over the exe at rva, unprotecting the pages for the write and putting the
// old protection back after. code = true for instructions (the pages are made executable
// and the CPU's instruction cache is flushed); false for data such as a constant or a
// vtable slot. False only if the pages couldn't be unprotected (nothing written).
bool WriteMem(DWORD rva, const void* src, size_t n, bool code) {
    BYTE* dst = ExeBase() + rva;
    DWORD old;
    if (!VirtualProtect(dst, n, code ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE, &old)) return false;
    memcpy(dst, src, n);
    VirtualProtect(dst, n, old, &old);
    if (code) FlushInstructionCache(GetCurrentProcess(), dst, n);
    return true;
}

// Executable memory within +-2 GB of 'site', so a 5-byte rel32 call can reach it.
// Tries addresses one allocation-granularity step (usually 64 KB) at a time outward from the
// site, above then below, until VirtualAlloc accepts one. NULL if nothing in range is free.
// The memory is never freed: hooks stay for the life of the process.
BYTE* AllocNear(BYTE* site, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    uintptr_t base = (uintptr_t)site & ~(gran - 1);
    for (uintptr_t d = gran; d < 0x7FF00000; d += gran) {
        uintptr_t cand[2] = { base + d, base > d ? base - d : 0 };
        for (uintptr_t a : cand) {
            if (!a) continue;
            void* p = VirtualAlloc((void*)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return (BYTE*)p;
        }
    }
    return NULL;
}

// Point the 5-byte "call rel32" at callRva (which must currently call
// expectTargetRva) at hook, through a 14-byte absolute-jump stub placed near.
// Only this one call site changes: the game's function and its other callers are untouched.
// The hook receives the call's arguments and must have the callee's signature; to still run
// the game's function it calls it itself (ExeBase() + expectTargetRva). Returns false, having
// written nothing, if the call isn't there or doesn't go where expected (the build check).
bool RedirectCall(DWORD callRva, DWORD expectTargetRva, void* hook) {
    BYTE* call = ExeBase() + callRva;
    // E8 = call rel32; the target is the next instruction's address plus the rel32.
    __try {
        if (call[0] != 0xE8 || call + 5 + *(int32_t*)(call + 1) != ExeBase() + expectTargetRva) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    BYTE* stub = AllocNear(call, 16);
    if (!stub) return false;
    stub[0] = 0xFF; stub[1] = 0x25;                // jmp qword ptr [rip+0]
    *(int32_t*)(stub + 2) = 0;
    memcpy(stub + 6, &hook, 8);                    // the 8-byte address that jmp reads
    FlushInstructionCache(GetCurrentProcess(), stub, 14);
    // Re-aim the call at the stub; AllocNear keeps the stub within rel32 reach.
    int64_t rel = stub - (call + 5);
    if (rel != (int32_t)rel) return false;
    BYTE patch[5] = { 0xE8 };
    int32_t rel32 = (int32_t)rel;
    memcpy(patch + 1, &rel32, 4);
    return WriteMem(callRva, patch, sizeof patch, true);
}

// Overwrites n bytes of the exe's code at rva (the same as WriteMem with code = true):
// used for in-place instruction patches and for the 14-byte "jmp [rip+0]; address"
// detours other modules write over a function's first instructions.
bool WriteCode(DWORD rva, const void* bytes, size_t n) {
    BYTE* p = ExeBase() + rva;
    DWORD old;
    if (!VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(p, bytes, n);
    VirtualProtect(p, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}

// Patch one named import in a module's import address table (usually the game exe's;
// harness_core.cpp's file I/O hooks and net_session.cpp's ExitProcess hook also patch
// ucrtbase.dll and other DLLs the game loaded).
// The import address table (IAT) is the array of function pointers the Windows loader
// fills in for every DLL function a module imports; the module calls through it. Swapping
// one entry makes every call the module makes to that function go to hook instead. The
// caller keeps the real function (from GetProcAddress) to call on. Returns how many entries
// were patched: 0 if the module doesn't import it by name (by-ordinal imports, and DLLs
// with no name table, are skipped), more than 1 if several DLLs supply it.
int PatchIAT(HMODULE mod, const char* name, void* hook) {
    int patched = 0;
    BYTE* base = (BYTE*)mod;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    IMAGE_IMPORT_DESCRIPTOR* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress);
    // One descriptor per imported DLL; in each, the name table (OriginalFirstThunk) and the
    // pointer table (FirstThunk) run in parallel, entry for entry.
    for (; imp->Name; imp++) {
        if (!imp->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA* names = (IMAGE_THUNK_DATA*)(base + imp->OriginalFirstThunk);
        IMAGE_THUNK_DATA* funcs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, funcs++) {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp(ibn->Name, name) != 0) continue;
            DWORD old;
            if (VirtualProtect(&funcs->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) {
                funcs->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&funcs->u1.Function, sizeof(void*), old, &old);
                Log("hooked %s (imported from %s)", name, (const char*)(base + imp->Name));
                g_hooksInstalled++;
                patched++;
            }
        }
    }
    return patched;
}
