
add_library(linuxkpi MODULE linuxkpi.c elfldr.c shim.c drmdev.c input.c lkpidisk.c linuxkpi.rc)
set_module_type(linuxkpi kernelmodedriver)
add_importlibs(linuxkpi ntoskrnl hal)
add_cd_file(TARGET linuxkpi DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(linuxkpi_reg.inf)

# Development aid: Linux modules put on the CD as \SystemRoot\system32\drivers\lkpimodN.ko and loaded in order
# by the host driver at start (cmake -DLKPI_MODULES="/path/kpi.ko;/path/drm.ko;...").
set(LKPI_MODULES "" CACHE STRING "Linux .ko files to load at start of the LinuxKPI host, in order")
set(_lkpi_count 0)
foreach(_m ${LKPI_MODULES})
    if(EXISTS "${_m}")
        add_cd_file(FILE "${_m}" DESTINATION reactos/system32/drivers NAME_ON_CD lkpimod${_lkpi_count}.ko FOR all)
        math(EXPR _lkpi_count "${_lkpi_count} + 1")
    endif()
endforeach()
target_compile_definitions(linuxkpi PRIVATE LKPI_MODULE_COUNT=${_lkpi_count})
