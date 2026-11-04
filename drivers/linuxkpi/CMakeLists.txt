
add_library(linuxkpi MODULE linuxkpi.c elfldr.c shim.c linuxkpi.rc)
set_module_type(linuxkpi kernelmodedriver)
add_importlibs(linuxkpi ntoskrnl hal)
add_cd_file(TARGET linuxkpi DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(linuxkpi_reg.inf)

# Development aid: put a Linux module on the CD as \SystemRoot\lkpi\test.ko
set(LKPI_TEST_MODULE "" CACHE FILEPATH "Linux .ko to load at start of the LinuxKPI host")
if(LKPI_TEST_MODULE AND EXISTS "${LKPI_TEST_MODULE}")
    add_cd_file(FILE "${LKPI_TEST_MODULE}" DESTINATION reactos/system32/drivers NAME_ON_CD lkpitest.ko FOR all)
endif()
