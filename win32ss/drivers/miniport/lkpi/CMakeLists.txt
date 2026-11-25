
add_library(lkpivmp MODULE lkpivmp.c lkpivmp_io.c lkpivmp.rc)
set_module_type(lkpivmp kernelmodedriver)
add_importlibs(lkpivmp ntoskrnl videoprt)
add_cd_file(TARGET lkpivmp DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(lkpivmp_reg.inf)
