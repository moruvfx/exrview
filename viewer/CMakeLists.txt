find_package(Qt6 REQUIRED COMPONENTS Widgets OpenGL OpenGLWidgets)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(SOURCES
    main.cpp
    MainWindow.cpp MainWindow.h
    ImageView.cpp ImageView.h
    FrameLoader.cpp FrameLoader.h
    resources/resources.qrc)
if(WIN32)
    list(APPEND SOURCES resources/app.rc)
endif()

add_executable(ExrView WIN32 ${SOURCES})
target_link_libraries(ExrView PRIVATE exrcore Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
target_compile_definitions(ExrView PRIVATE EXRVIEW_VERSION="${PROJECT_VERSION}")
if(WIN32)
    target_link_libraries(ExrView PRIVATE shell32)
endif()

install(TARGETS ExrView RUNTIME DESTINATION .)
