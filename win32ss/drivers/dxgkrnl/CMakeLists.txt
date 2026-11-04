
add_library(dxgkrnl MODULE dxgkrnl.c dxgkrnl.rc)
set_module_type(dxgkrnl kernelmodedriver)
add_importlibs(dxgkrnl ntoskrnl hal)
add_cd_file(TARGET dxgkrnl DESTINATION reactos/system32/drivers FOR all)
add_registry_inf(dxgkrnl_reg.inf)

# Optional test hook: -DDXGK_TEST_MINIPORT=<path to a WDDM display miniport .sys>
# puts it on the CD as viogpudo.sys and binds it to virtio-gpu (PCI\VEN_1AF4&DEV_1050).
if(DXGK_TEST_MINIPORT)
    add_cd_file(FILE "${DXGK_TEST_MINIPORT}" DESTINATION reactos/system32/drivers NAME_ON_CD viogpudo.sys FOR all)
    add_registry_inf(dxgkrnl_test_reg.inf)
endif()
