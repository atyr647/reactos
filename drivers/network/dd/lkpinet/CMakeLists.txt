
add_definitions(
    -DNDIS50_MINIPORT
    -DNDIS_MINIPORT_DRIVER
    -DNDIS_LEGACY_MINIPORT=1)

include_directories(${REACTOS_SOURCE_DIR}/drivers/linuxkpi)

add_library(lkpinet MODULE lkpinet.c lkpinet.rc)
set_module_type(lkpinet kernelmodedriver)
add_importlibs(lkpinet ndis ntoskrnl hal)
add_cd_file(TARGET lkpinet DESTINATION reactos/system32/drivers FOR all)
add_driver_inf(lkpinet netlkpi.inf)
