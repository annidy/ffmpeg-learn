cmake_minimum_required(VERSION 3.12)
project(ffmpeg-learn)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

find_package(FFmpeg REQUIRED AVCODEC AVFORMAT AVUTIL AVDEVICE POSTPROC SWSCALE)
find_package(SDL2 REQUIRED)

if(FFMPEG_FOUND)
#  FFMPEG_INCLUDE_DIRS  - Include directory necessary for using the required components headers.
#  FFMPEG_LIBRARIES     - Link these to use the required ffmpeg components.
#  FFMPEG_DEFINITIONS   - Compiler switches required for using the required ffmpeg components.
    message("FFMPEG_INCLUDE_DIRS = ${FFMPEG_INCLUDE_DIRS} ")
    message("FFMPEG_LIBRARIES = ${FFMPEG_LIBRARIES} ")
    message("FFMPEG_DEFINITIONS = ${FFMPEG_DEFINITIONS} ")

    include_directories(${FFMPEG_INCLUDE_DIRS})

endif()
add_executable(tutorial01 tutorial01.c)
target_link_libraries(tutorial01 ${FFMPEG_LIBRARIES} lzma z)

add_executable(tutorial02 tutorial02.c)
# 包含SDL2头文件目录
target_include_directories(tutorial02 PRIVATE ${SDL2_INCLUDE_DIRS} ${FFMPEG_LIBRARIES} lzma z)
# 链接SDL2库
target_link_libraries(tutorial02 PRIVATE ${SDL2_LIBRARIES} ${FFMPEG_LIBRARIES} lzma z)