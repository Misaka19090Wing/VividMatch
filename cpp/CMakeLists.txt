cmake_minimum_required(VERSION 3.16)
project(VividMatch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

# The sources carry Chinese comments; without this MSVC warns and can mangle
# bytes depending on the active code page. build_msvc.bat passes /utf-8 too.
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_executable(vividmatch_image image_tool.cpp)
target_link_libraries(vividmatch_image PRIVATE ${OpenCV_LIBS})

add_executable(vividmatch_image_test test_visual_fingerprint.cpp)
target_link_libraries(vividmatch_image_test PRIVATE ${OpenCV_LIBS})

add_executable(vividmatch_video video_tool.cpp)
target_link_libraries(vividmatch_video PRIVATE ${OpenCV_LIBS})

add_executable(vividmatch_video_test test_video_fingerprint.cpp)
target_link_libraries(vividmatch_video_test PRIVATE ${OpenCV_LIBS})
