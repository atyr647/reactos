/*
 * PROJECT:     ReactOS Linux kernel module host (LinuxKPI)
 * PURPOSE:     Minimal ELF64 x86-64 relocatable object (.ko / .o) loader
 */

#include "linuxkpi.h"

#define NDEBUG
#include <debug.h>

#pragma pack(push, 1)
typedef struct { UCHAR e_ident[16]; USHORT e_type, e_machine; ULONG e_version; ULONG64 e_entry, e_phoff, e_shoff; ULONG e_flags; USHORT e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } ELF64_EHDR;
typedef struct { ULONG sh_name, sh_type; ULONG64 sh_flags, sh_addr, sh_offset, sh_size; ULONG sh_link, sh_info; ULONG64 sh_addralign, sh_entsize; } ELF64_SHDR;
typedef struct { ULONG st_name; UCHAR st_info, st_other; USHORT st_shndx; ULONG64 st_value, st_size; } ELF64_SYM;
typedef struct { ULONG64 r_offset, r_info; LONG64 r_addend; } ELF64_RELA;
#pragma pack(pop)

#define SHT_SYMTAB 2
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHF_ALLOC 2
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2
#define STB_WEAK 2

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_GOT32 3
#define R_X86_64_PLT32 4
#define R_X86_64_GOTPCREL 9
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_PC64 24
#define R_X86_64_GOTPCRELX 41
#define R_X86_64_REX_GOTPCRELX 42

/* One 16-byte entry per symbol: [0..7] = GOT slot (symbol address), [8..13] = jmp *slot(%rip) (a PLT stub) */
#define STUB_SIZE 16

struct _LKPI_MODULE
{
    PUCHAR Image;       /* the file */
    ULONG ImageSize;
    PUCHAR Region;      /* loaded sections + stubs + common symbols */
    SIZE_T RegionSize;
    PUCHAR *SecBase;    /* per-section load address (NULL if not loaded) */
    ULONG64 *SymAddr;   /* per-symbol resolved address */
    PUCHAR Stubs;
    ULONG NumSyms;
    PVOID InitFn, ExitFn;
    BOOLEAN Initialized;
};

static PVOID
LkpiFindExport(const char *Name)
{
    ULONG i;
    for (i = 0; i < LkpiExportCount; i++)
        if (!strcmp(LkpiExports[i].Name, Name))
            return LkpiExports[i].Address;
    return NULL;
}

static NTSTATUS
LkpiReadFile(PCUNICODE_STRING Path, PUCHAR *Data, PULONG Size)
{
    OBJECT_ATTRIBUTES Oa;
    IO_STATUS_BLOCK Iosb;
    HANDLE File;
    FILE_STANDARD_INFORMATION Info;
    NTSTATUS Status;
    PUCHAR Buf;

    InitializeObjectAttributes(&Oa, (PUNICODE_STRING)Path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    Status = ZwCreateFile(&File, GENERIC_READ | SYNCHRONIZE, &Oa, &Iosb, NULL, FILE_ATTRIBUTE_NORMAL,
                          FILE_SHARE_READ, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, NULL, 0);
    if (!NT_SUCCESS(Status)) return Status;
    Status = ZwQueryInformationFile(File, &Iosb, &Info, sizeof(Info), FileStandardInformation);
    if (!NT_SUCCESS(Status) || Info.EndOfFile.QuadPart > 0x10000000) { ZwClose(File); return NT_SUCCESS(Status) ? STATUS_FILE_TOO_LARGE : Status; }
    Buf = ExAllocatePoolWithTag(PagedPool, (SIZE_T)Info.EndOfFile.QuadPart, LKPI_TAG);
    if (!Buf) { ZwClose(File); return STATUS_NO_MEMORY; }
    Status = ZwReadFile(File, NULL, NULL, NULL, &Iosb, Buf, (ULONG)Info.EndOfFile.QuadPart, NULL, NULL);
    ZwClose(File);
    if (!NT_SUCCESS(Status)) { ExFreePoolWithTag(Buf, LKPI_TAG); return Status; }
    *Data = Buf;
    *Size = (ULONG)Info.EndOfFile.QuadPart;
    return STATUS_SUCCESS;
}

VOID
LkpiFreeModule(PLKPI_MODULE M)
{
    if (!M) return;
    if (M->Image) ExFreePoolWithTag(M->Image, LKPI_TAG);
    if (M->Region) ExFreePoolWithTag(M->Region, LKPI_TAG);
    if (M->SecBase) ExFreePoolWithTag(M->SecBase, LKPI_TAG);
    if (M->SymAddr) ExFreePoolWithTag(M->SymAddr, LKPI_TAG);
    ExFreePoolWithTag(M, LKPI_TAG);
}

#define FAIL(msg, ...) do { DbgPrint("LKPI: " msg "\n", ##__VA_ARGS__); Status = STATUS_INVALID_IMAGE_FORMAT; goto fail; } while (0)

NTSTATUS
LkpiLoadModule(PCUNICODE_STRING Path, PLKPI_MODULE *Out)
{
    PLKPI_MODULE M;
    ELF64_EHDR *Eh;
    ELF64_SHDR *Sh, *SymSh = NULL;
    ELF64_SYM *Syms;
    const char *StrTab;
    NTSTATUS Status;
    ULONG i, j;
    SIZE_T Total = 0, Off;
    ULONG MissingSyms = 0;

    M = ExAllocatePoolWithTag(NonPagedPool, sizeof(*M), LKPI_TAG);
    if (!M) return STATUS_NO_MEMORY;
    RtlZeroMemory(M, sizeof(*M));

    Status = LkpiReadFile(Path, &M->Image, &M->ImageSize);
    if (!NT_SUCCESS(Status)) { DbgPrint("LKPI: cannot read module (%08lx)\n", Status); goto fail; }

    Eh = (ELF64_EHDR *)M->Image;
    if (M->ImageSize < sizeof(*Eh) || memcmp(Eh->e_ident, "\177ELF", 4) || Eh->e_ident[4] != 2 || Eh->e_machine != 62 || Eh->e_type != 1)
        FAIL("not an ELF64 x86-64 relocatable object");
    if (Eh->e_shoff + (ULONG64)Eh->e_shnum * sizeof(ELF64_SHDR) > M->ImageSize) FAIL("bad section table");
    Sh = (ELF64_SHDR *)(M->Image + Eh->e_shoff);

    M->SecBase = ExAllocatePoolWithTag(NonPagedPool, Eh->e_shnum * sizeof(PUCHAR), LKPI_TAG);
    if (!M->SecBase) { Status = STATUS_NO_MEMORY; goto fail; }
    RtlZeroMemory(M->SecBase, Eh->e_shnum * sizeof(PUCHAR));

    for (i = 0; i < Eh->e_shnum; i++)
        if (Sh[i].sh_type == SHT_SYMTAB) SymSh = &Sh[i];
    if (!SymSh) FAIL("no symbol table");
    Syms = (ELF64_SYM *)(M->Image + SymSh->sh_offset);
    M->NumSyms = (ULONG)(SymSh->sh_size / sizeof(ELF64_SYM));
    StrTab = (const char *)(M->Image + Sh[SymSh->sh_link].sh_offset);

    /* Layout: [alloc sections][common symbols][stubs] */
    for (i = 0; i < Eh->e_shnum; i++)
    {
        if (!(Sh[i].sh_flags & SHF_ALLOC)) continue;
        Total = ALIGN_UP_BY(Total, max(Sh[i].sh_addralign, 1));
        Total += (SIZE_T)Sh[i].sh_size;
    }
    for (i = 1; i < M->NumSyms; i++)
        if (Syms[i].st_shndx == SHN_COMMON)
        {
            Total = ALIGN_UP_BY(Total, max(Syms[i].st_value, 1));
            Total += (SIZE_T)Syms[i].st_size;
        }
    Total = ALIGN_UP_BY(Total, 64);
    M->RegionSize = Total + (SIZE_T)M->NumSyms * STUB_SIZE;
    M->Region = ExAllocatePoolWithTag(NonPagedPool, M->RegionSize, LKPI_TAG);
    if (!M->Region) { Status = STATUS_NO_MEMORY; goto fail; }
    RtlZeroMemory(M->Region, M->RegionSize);
    M->Stubs = M->Region + Total;

    Off = 0;
    for (i = 0; i < Eh->e_shnum; i++)
    {
        if (!(Sh[i].sh_flags & SHF_ALLOC)) continue;
        Off = ALIGN_UP_BY(Off, max(Sh[i].sh_addralign, 1));
        M->SecBase[i] = M->Region + Off;
        if (Sh[i].sh_type != SHT_NOBITS) RtlCopyMemory(M->SecBase[i], M->Image + Sh[i].sh_offset, (SIZE_T)Sh[i].sh_size);
        Off += (SIZE_T)Sh[i].sh_size;
    }

    M->SymAddr = ExAllocatePoolWithTag(NonPagedPool, M->NumSyms * sizeof(ULONG64), LKPI_TAG);
    if (!M->SymAddr) { Status = STATUS_NO_MEMORY; goto fail; }
    RtlZeroMemory(M->SymAddr, M->NumSyms * sizeof(ULONG64));

    for (i = 1; i < M->NumSyms; i++)
    {
        ELF64_SYM *S = &Syms[i];
        const char *Name = StrTab + S->st_name;

        if (S->st_shndx == SHN_UNDEF)
        {
            PVOID A = !strcmp(Name, "_GLOBAL_OFFSET_TABLE_") ? (PVOID)M->Stubs : LkpiFindExport(Name);
            if (!A && (S->st_info >> 4) != STB_WEAK && Name[0]) { DbgPrint("LKPI: unresolved symbol '%s'\n", Name); MissingSyms++; }
            M->SymAddr[i] = (ULONG64)(ULONG_PTR)A;
        }
        else if (S->st_shndx == SHN_ABS)
            M->SymAddr[i] = S->st_value;
        else if (S->st_shndx == SHN_COMMON)
        {
            Off = ALIGN_UP_BY(Off, max(S->st_value, 1));
            M->SymAddr[i] = (ULONG64)(ULONG_PTR)(M->Region + Off);
            Off += (SIZE_T)S->st_size;
        }
        else if (S->st_shndx < Eh->e_shnum && M->SecBase[S->st_shndx])
            M->SymAddr[i] = (ULONG64)(ULONG_PTR)M->SecBase[S->st_shndx] + S->st_value;

        if (!strcmp(Name, "init_module") && (S->st_info & 0xf) == 2) M->InitFn = (PVOID)(ULONG_PTR)M->SymAddr[i];
        if (!strcmp(Name, "cleanup_module") && (S->st_info & 0xf) == 2) M->ExitFn = (PVOID)(ULONG_PTR)M->SymAddr[i];

        /* GOT slot + PLT stub for every symbol */
        {
            PUCHAR E = M->Stubs + (SIZE_T)i * STUB_SIZE;
            *(ULONG64 *)E = M->SymAddr[i];
            E[8] = 0xff; E[9] = 0x25;
            *(LONG *)(E + 10) = (LONG)(-14);   /* jmp *slot(%rip): slot = E, next insn at E+14 */
        }
    }
    if (MissingSyms) { DbgPrint("LKPI: %lu unresolved symbols, refusing to load\n", MissingSyms); Status = STATUS_DRIVER_ENTRYPOINT_NOT_FOUND; goto fail; }

    for (i = 0; i < Eh->e_shnum; i++)
    {
        ELF64_RELA *R;
        PUCHAR Target;
        ULONG n;

        if (Sh[i].sh_type != SHT_RELA) continue;
        if (Sh[i].sh_info >= Eh->e_shnum || !M->SecBase[Sh[i].sh_info]) continue; /* relocations for non-loaded sections */
        Target = M->SecBase[Sh[i].sh_info];
        R = (ELF64_RELA *)(M->Image + Sh[i].sh_offset);
        n = (ULONG)(Sh[i].sh_size / sizeof(ELF64_RELA));
        for (j = 0; j < n; j++)
        {
            ULONG Type = (ULONG)R[j].r_info, SymIdx = (ULONG)(R[j].r_info >> 32);
            PUCHAR P = Target + R[j].r_offset;
            ULONG64 S = SymIdx < M->NumSyms ? M->SymAddr[SymIdx] : 0;
            PUCHAR Stub = M->Stubs + (SIZE_T)SymIdx * STUB_SIZE;
            LONG64 V;

            switch (Type)
            {
                case R_X86_64_NONE: break;
                case R_X86_64_64: *(ULONG64 *)P = S + R[j].r_addend; break;
                case R_X86_64_PC64: *(LONG64 *)P = (LONG64)(S + R[j].r_addend - (ULONG64)(ULONG_PTR)P); break;
                case R_X86_64_PC32:
                case R_X86_64_PLT32:
                    V = (LONG64)(S + R[j].r_addend - (ULONG64)(ULONG_PTR)P);
                    if (V != (LONG)V)
                    {
                        /* out of range (symbol lives in the host driver): go through the stub */
                        if (Type == R_X86_64_PC32 && SymIdx >= M->NumSyms) FAIL("PC32 out of range");
                        V = (LONG64)((ULONG64)(ULONG_PTR)(Stub + 8) + R[j].r_addend - (ULONG64)(ULONG_PTR)P);
                        if (Type == R_X86_64_PC32 || V != (LONG)V) FAIL("PC32/PLT32 to %p out of range (type %lu)", (PVOID)S, Type);
                    }
                    *(LONG *)P = (LONG)V;
                    break;
                case R_X86_64_GOTPCREL:
                case R_X86_64_GOTPCRELX:
                case R_X86_64_REX_GOTPCRELX:
                    V = (LONG64)((ULONG64)(ULONG_PTR)Stub + R[j].r_addend - (ULONG64)(ULONG_PTR)P);
                    *(LONG *)P = (LONG)V;
                    break;
                case R_X86_64_32:
                    V = (LONG64)(S + R[j].r_addend);
                    if ((ULONG64)V != (ULONG)V) FAIL("R_X86_64_32 out of range");
                    *(ULONG *)P = (ULONG)V;
                    break;
                case R_X86_64_32S:
                    V = (LONG64)(S + R[j].r_addend);
                    if (V != (LONG)V) FAIL("R_X86_64_32S out of range (module not built position independent?)");
                    *(LONG *)P = (LONG)V;
                    break;
                default:
                    FAIL("unsupported relocation type %lu", Type);
            }
        }
    }

    DbgPrint("LKPI: loaded module: %lu bytes in memory at %p, init=%p exit=%p\n", (ULONG)M->RegionSize, M->Region, M->InitFn, M->ExitFn);
    *Out = M;
    return STATUS_SUCCESS;

fail:
    LkpiFreeModule(M);
    return Status;
}

NTSTATUS
LkpiRunInit(PLKPI_MODULE M, int *Result)
{
    if (!M->InitFn) return STATUS_ENTRYPOINT_NOT_FOUND;
    *Result = ((int LX_ABI (*)(void))M->InitFn)();
    M->Initialized = (*Result == 0);
    return STATUS_SUCCESS;
}

VOID
LkpiRunExit(PLKPI_MODULE M)
{
    if (M->Initialized && M->ExitFn) ((void LX_ABI (*)(void))M->ExitFn)();
    M->Initialized = FALSE;
}
