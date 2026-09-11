cmake_minimum_required(VERSION 3.16)
project(VividMatchGui VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

if(DEFINED ENV{OpenCV_DIR} AND NOT DEFINED OpenCV_DIR)
    set(OpenCV_DIR "$ENV{OpenCV_DIR}")
endif()
find_package(OpenCV REQUIRED)

qt_add_executable(VividMatchGui WIN32
    src/main.cpp
    src/ui/main/mainwin.cpp
    src/ui/main/mainwin.h
    src/ui/main/imagecomparepage.cpp
    src/ui/main/imagecomparepage.h
    src/ui/main/batchcompareworker.cpp
    src/ui/main/batchcompareworker.h
    src/ui/main/batchcomparepage.cpp
    src/ui/main/batchcomparepage.h
    src/ui/main/videocomparepage.cpp
    src/ui/main/videocomparepage.h
)

# The video page fingerprints clips with the C++ video/audio headers, so it
# needs OpenCV's include and library directories.
target_include_directories(VividMatchGui PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../cpp"
    ${OpenCV_INCLUDE_DIRS})
target_link_libraries(VividMatchGui PRIVATE Qt${QT_VERSION_MAJOR}::Widgets ${OpenCV_LIBS})

set_target_properties(VividMatchGui PROPERTIES
    WIN32_EXECUTABLE TRUE
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_SOURCE_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/bin"
)

if(MSVC)
    target_compile_options(VividMatchGui PRIVATE /utf-8)
    target_compile_definitions(VividMatchGui PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
