cmake_minimum_required(VERSION 3.8.0)
project(mjpeg-player)
set(32BLIT_PATH "../" CACHE PATH "Path to 32blit.cmake")

#set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -fsanitize=address")

include(${32BLIT_PATH}/32blit.cmake)

#add_definitions("-DPROFILER")

blit_executable(mjpeg-player
    avi-file.cpp
    control-icons.cpp
    file-browser.cpp
    mjpeg-player.cpp
)

blit_assets_yaml(mjpeg-player assets.yml)
