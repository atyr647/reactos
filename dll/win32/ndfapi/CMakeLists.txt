
remove_definitions(-D_WIN32_WINNT=0x502)
add_definitions(-D_WIN32_WINNT=0x600)

spec2def(ndfapi.dll ndfapi.spec ADD_IMPORTLIB)

list(APPEND SOURCE
    main.c
    ${CMAKE_CURRENT_BINARY_DIR}/ndfapi_stubs.c
    ${CMAKE_CURRENT_BINARY_DIR}/ndfapi.def)

add_library(ndfapi MODULE ${SOURCE})
set_module_type(ndfapi win32dll ENTRYPOINT 0)
add_importlibs(ndfapi kernel32 ntdll)
add_dependencies(ndfapi psdk)
add_cd_file(TARGET ndfapi DESTINATION reactos/system32 FOR all)
