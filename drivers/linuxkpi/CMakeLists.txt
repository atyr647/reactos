
add_library(linuxkpi MODULE linuxkpi.c elfldr.c shim.c drmdev.c input.c lkpidisk.c linuxkpi.rc)
set_module_type(linuxkpi kernelmodedriver)
add_importlibs(linuxkpi ntoskrnl hal)
add_cd_file(TARGET linuxkpi DESTINATION reactos/system32/drivers NO_CAB FOR all)

# Linux modules (cmake -DLKPI_MODULES="/path/kpi.ko;/path/kpi2.ko;...", in load order) are linked into
# linuxkpi.sys itself (lkpimods.S, .incbin), so the driver is self-contained and can start before any file system exists.
set(LKPI_MODULES "" CACHE STRING "Linux .ko files to embed in the LinuxKPI host, in load order")
set(_lkpi_count 0)
set(_lkpi_asm "    .section .rdata,\"dr\"\n    .balign 16\n")
set(_lkpi_tbl "")
foreach(_m ${LKPI_MODULES})
    if(EXISTS "${_m}")
        set(_lkpi_asm "${_lkpi_asm}lkpimod${_lkpi_count}_start:\n    .incbin \"${_m}\"\nlkpimod${_lkpi_count}_end:\n    .balign 16\n")
        set(_lkpi_tbl "${_lkpi_tbl}    .quad lkpimod${_lkpi_count}_start, lkpimod${_lkpi_count}_end\n")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_m}")
        math(EXPR _lkpi_count "${_lkpi_count} + 1")
    endif()
endforeach()
target_compile_definitions(linuxkpi PRIVATE LKPI_MODULE_COUNT=${_lkpi_count})
if(_lkpi_count GREATER 0)
    set(_lkpi_s "${CMAKE_CURRENT_BINARY_DIR}/lkpimods.S")
    file(WRITE ${_lkpi_s}.tmp "${_lkpi_asm}    .globl LkpiEmbeddedTable\nLkpiEmbeddedTable:\n${_lkpi_tbl}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_lkpi_s}.tmp ${_lkpi_s})
    target_sources(linuxkpi PRIVATE ${_lkpi_s})
    set_source_files_properties(${_lkpi_s} PROPERTIES OBJECT_DEPENDS "${LKPI_MODULES}")
    target_compile_definitions(linuxkpi PRIVATE LKPI_EMBEDDED)
endif()

# With modules on the CD the host starts at system start (the boot option /NOLINUXKPI skips it); without any it
# stays a demand-start driver.
if(_lkpi_count GREATER 0)
    add_registry_inf(linuxkpi_reg_boot.inf)
else()
    add_registry_inf(linuxkpi_reg.inf)
endif()
