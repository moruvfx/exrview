# DLL cho Windows Explorer: thumbnail + preview pane cho .exr
add_library(ExrViewShell SHARED
    Common.h Common.cpp
    ThumbnailProvider.cpp
    PreviewHandler.cpp
    Dll.cpp
    ExrViewShell.def)
target_link_libraries(ExrViewShell PRIVATE exrcore shlwapi ole32 advapi32 gdi32 user32 shell32)
target_compile_definitions(ExrViewShell PRIVATE UNICODE _UNICODE)
install(TARGETS ExrViewShell RUNTIME DESTINATION . LIBRARY DESTINATION .)
