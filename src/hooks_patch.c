#include "hooks.h"
#include "hooks_internal.h"
#include "log.h"
#include <string.h>

/* ---- IAT patching ------------------------------------------------------ */

BOOL patch_iat_entry(HMODULE mod, const char *dll, const char *func, void *hook,
                            void **orig_out, void **slot_out)
{
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos;
    IMAGE_NT_HEADERS *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD imp_rva;

    if (!base || base == (BYTE *)(ULONG_PTR)-1)
        return FALSE;
    dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return FALSE;
    nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return FALSE;
    imp_rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!imp_rva)
        return FALSE;

    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + imp_rva); imp->Name; ++imp) {
        const char *name = (const char *)(base + imp->Name);
        IMAGE_THUNK_DATA *oft;
        IMAGE_THUNK_DATA *ft;
        DWORD oldprot;

        if (lstrcmpiA(name, dll) != 0)
            continue;
        oft = (IMAGE_THUNK_DATA *)(base +
                                  (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        ft = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
        for (; oft->u1.AddressOfData; ++oft, ++ft) {
            const char *fname;
            if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal))
                continue;
            fname = (const char *)(base + oft->u1.AddressOfData + 2);
            if (lstrcmpA(fname, func) != 0)
                continue;
            if (orig_out && !*orig_out)
                *orig_out = (void *)(ULONG_PTR)ft->u1.Function;
            if (!VirtualProtect(&ft->u1.Function, sizeof(void *), PAGE_READWRITE, &oldprot))
                return FALSE;
            ft->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&ft->u1.Function, sizeof(void *), oldprot, &oldprot);
            if (slot_out)
                *slot_out = &ft->u1.Function;
            return TRUE;
        }
    }
    return FALSE;
}

void restore_iat_slot(void *slot, void *orig)
{
    DWORD oldprot;
    if (!slot || !orig)
        return;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &oldprot))
        return;
    *(ULONG_PTR *)slot = (ULONG_PTR)orig;
    VirtualProtect(slot, sizeof(void *), oldprot, &oldprot);
}

/* ---- inline trampoline ------------------------------------------------- */

BOOL install_inline_hook(void *target, void *hook, SIZE_T steal, BYTE **tramp_out,
                                BYTE *saved_out)
{
    BYTE *tramp;
    DWORD oldprot;
    INT32 rel;
    SIZE_T i;

    if (!target || !hook || steal < 5 || steal > 16)
        return FALSE;
    tramp = (BYTE *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp)
        return FALSE;
    memcpy(tramp, target, steal);
    if (saved_out)
        memcpy(saved_out, target, steal);
    /* jmp rel32 back to target+steal */
    tramp[steal] = 0xE9;
    rel = (INT32)((BYTE *)target + steal - (tramp + steal + 5));
    memcpy(tramp + steal + 1, &rel, 4);

    if (!VirtualProtect(target, steal, PAGE_EXECUTE_READWRITE, &oldprot)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return FALSE;
    }
    *(BYTE *)target = 0xE9;
    rel = (INT32)((BYTE *)hook - ((BYTE *)target + 5));
    memcpy((BYTE *)target + 1, &rel, 4);
    for (i = 5; i < steal; ++i)
        ((BYTE *)target)[i] = 0x90;
    VirtualProtect(target, steal, oldprot, &oldprot);
    FlushInstructionCache(GetCurrentProcess(), target, steal);
    *tramp_out = tramp;
    return TRUE;
}

void remove_inline_hook(void *target, SIZE_T steal, const BYTE *saved, BYTE *tramp)
{
    DWORD oldprot;
    if (!target || !saved || steal == 0)
        return;
    if (VirtualProtect(target, steal, PAGE_EXECUTE_READWRITE, &oldprot)) {
        memcpy(target, saved, steal);
        VirtualProtect(target, steal, oldprot, &oldprot);
        FlushInstructionCache(GetCurrentProcess(), target, steal);
    }
    if (tramp)
        VirtualFree(tramp, 0, MEM_RELEASE);
}

