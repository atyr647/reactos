
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

spec2def(wevtapi.dll wevtapi.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/wevtapi_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/wevtapi.def)

add_library(wevtapi MODULE ${SOURCE})
set_module_type(wevtapi win32dll ENTRYPOINT 0)
add_importlibs(wevtapi kernel32 ntdll)
add_dependencies(wevtapi psdk)
add_cd_file(TARGET wevtapi DESTINATION reactos/system32 FOR all)
