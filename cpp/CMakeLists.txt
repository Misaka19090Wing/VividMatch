cmake_minimum_required(VERSION 3.16)
project(VividMatch LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenCV REQUIRED)

add_executable(vividmatch_image image_tool.cpp)
target_link_libraries(vividmatch_image PRIVATE ${OpenCV_LIBS})

add_executable(vividmatch_image_test test_visual_fingerprint.cpp)
target_link_libraries(vividmatch_image_test PRIVATE ${OpenCV_LIBS})
