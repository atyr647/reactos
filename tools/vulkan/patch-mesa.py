#!/usr/bin/env python3
"""Applies the ReactOS-related Mesa patches (idempotent). usage: patch-mesa.py <mesa-src-dir>"""
import sys, os
root = sys.argv[1]
def patch(rel, old, new):
    p = os.path.join(root, rel)
    s = open(p).read()
    if new in s:
        return
    assert old in s, rel
    open(p, 'w').write(s.replace(old, new + old, 1))

# llvmpipe's MCJIT/ORC code can land code and data sections > 2 GiB apart (VirtualAlloc hints are best effort on
# ReactOS), which overflows 32-bit relocations: always use the Large code model on x86-64.
patch('src/gallium/auxiliary/gallivm/lp_bld_misc.cpp',
      "#if DETECT_ARCH_PPC_64\n   /*\n    * Large programs, e.g. gnome-shell",
      "#if DETECT_ARCH_X86_64\n   /* code and data sections can be more than 2 GiB apart (ReactOS) */\n   builder.setCodeModel(CodeModel::Large);\n#endif\n\n")
patch('src/gallium/auxiliary/gallivm/lp_bld_init_orc.cpp',
      "#if DETECT_ARCH_PPC_64 == 1\n   /*\n    * Large programs, e.g. gnome-shell",
      "#if DETECT_ARCH_X86_64\n   /* code and data sections can be more than 2 GiB apart (ReactOS) */\n   JTMB.setCodeModel(CodeModel::Large);\n#endif\n\n")

# COFF x86-64 JIT code uses 32-bit RIP-relative relocations between its sections, so they must all be within
# 2 GiB. LLVM's SectionMemoryManager allocates code/rodata/data separately and ReactOS can put them far apart
# (observed 2.4 GiB): allocate every section from one reserved arena instead (Windows x86-64 only).
_ARENA = "#if defined(_WIN32) && DETECT_ARCH_X86_64\n#include <windows.h>\n/*\n * COFF x86-64 code refers to constant pools and data sections with 32-bit RIP-relative relocations, so all\n * sections of generated code must live within 2 GiB of each other. LLVM's SectionMemoryManager allocates\n * code/rodata/data from separate VirtualAlloc calls that can end up anywhere (observed on ReactOS), so\n * carve every section out of one reserved arena instead.\n */\nclass ArenaMemoryManager : public BaseMemoryManager {\n   uint8_t *Base;\n   size_t Size, Used, Committed;\n   static const size_t ArenaSize = 1024ull << 20;\n\n   uint8_t *alloc(uintptr_t Sz, unsigned Align) {\n      if (!Base)\n         return NULL;\n      if (Align < 16)\n         Align = 16;\n      size_t Off = (Used + Align - 1) & ~(size_t)(Align - 1);\n      if (Off + Sz > Size)\n         return NULL;\n      size_t End = (Off + Sz + 0xFFFF) & ~(size_t)0xFFFF;\n      if (End > Committed) {\n         if (!VirtualAlloc(Base + Committed, End - Committed, MEM_COMMIT, PAGE_EXECUTE_READWRITE))\n            return NULL;\n         Committed = End;\n      }\n      Used = Off + Sz;\n      return Base + Off;\n   }\npublic:\n   ArenaMemoryManager() : Used(0), Committed(0), Size(ArenaSize) {\n      Base = (uint8_t *)VirtualAlloc(NULL, ArenaSize, MEM_RESERVE, PAGE_EXECUTE_READWRITE);\n   }\n   ~ArenaMemoryManager() {\n      if (Base)\n         VirtualFree(Base, 0, MEM_RELEASE);\n   }\n   uint8_t *allocateCodeSection(uintptr_t Sz, unsigned Align, unsigned, llvm::StringRef) override {\n      return alloc(Sz, Align);\n   }\n   uint8_t *allocateDataSection(uintptr_t Sz, unsigned Align, unsigned, llvm::StringRef, bool) override {\n      return alloc(Sz, Align);\n   }\n   bool finalizeMemory(std::string *ErrMsg = nullptr) override {\n      FlushInstructionCache(GetCurrentProcess(), Base, Committed);\n      return false;\n   }\n   void registerEHFrames(uint8_t *, uint64_t, size_t) override {}\n   void deregisterEHFrames() override {}\n};\n#endif\n\n"
_BODY_OLD = "   BaseMemoryManager *mm;\n   mm = new llvm::SectionMemoryManager();\n"
_BODY_NEW = '   BaseMemoryManager *mm;\n#if defined(_WIN32) && DETECT_ARCH_X86_64\n   mm = new ArenaMemoryManager();\n#else\n   mm = new llvm::SectionMemoryManager();\n#endif\n'
_f = os.path.join(root, 'src/gallium/auxiliary/gallivm/lp_bld_misc.cpp')
_s = open(_f).read()
if 'ArenaMemoryManager' not in _s:
    _hook = 'extern "C"\nLLVMMCJITMemoryManagerRef\nlp_get_default_memory_manager()'
    assert _hook in _s and _BODY_OLD in _s
    _s = _s.replace(_hook, _ARENA + _hook, 1).replace(_BODY_OLD, _BODY_NEW, 1)
    open(_f, 'w').write(_s)
