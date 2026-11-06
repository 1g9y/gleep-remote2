cmake_minimum_required(VERSION 3.10)
project(gleep_remote C)
set(CMAKE_C_STANDARD 99)

# Sanity check: must be cross-compiling with the Vita toolchain, not host gcc.
if(NOT CMAKE_C_COMPILER MATCHES "arm-vita-eabi")
    message(FATAL_ERROR
        "Not using the Vita cross-compiler (found: ${CMAKE_C_COMPILER}).\n"
        "Configure with the VitaSDK toolchain file, e.g.:\n"
        "  cmake -DCMAKE_TOOLCHAIN_FILE=${VITASDK}/share/vita.toolchain.cmake ..")
endif()

add_executable(${PROJECT_NAME} main.c font.c)

target_link_libraries(${PROJECT_NAME}
    vita2d
    SceNet_stub
    SceNetCtl_stub
    SceCtrl_stub
    SceTouch_stub
    SceSysmodule_stub
    SceGxm_stub
    SceDisplay_stub
    SceCommonDialog_stub
    ScePgf_stub
    SceAppMgr_stub
    m
)

set(VITA_APPID    "GLEE00001")
set(VITA_TITLE    "Gleep Remote")
set(VITA_VERSION  "01.00")

# Resolve VITASDK: CMake variable first, then environment, then a sane default.
if(NOT DEFINED VITASDK OR VITASDK STREQUAL "")
    set(VITASDK "$ENV{VITASDK}")
endif()
if(NOT VITASDK OR VITASDK STREQUAL "")
    if(EXISTS "/usr/local/vitasdk")
        set(VITASDK "/usr/local/vitasdk")
    else()
        message(FATAL_ERROR "VITASDK not set and /usr/local/vitasdk not found")
    endif()
endif()

message(STATUS "Using VITASDK: ${VITASDK}")

# Belt and suspenders: SDK headers/libs live in these two spots.
include_directories(${VITASDK}/include ${VITASDK}/arm-vita-eabi/include)
link_directories(${VITASDK}/lib ${VITASDK}/arm-vita-eabi/lib)

include(${VITASDK}/share/vita.cmake)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_APPID} ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME ${VITA_TITLE}
    FILE ${CMAKE_SOURCE_DIR}/sce_sys/icon0.png sce_sys/icon0.png
    FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/bg0.png sce_sys/livearea/contents/bg0.png
    FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
    FILE ${CMAKE_SOURCE_DIR}/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
