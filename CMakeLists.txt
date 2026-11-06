cmake_minimum_required(VERSION 3.10)
project(gleep_remote C)
set(CMAKE_C_STANDARD 99)

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

set(VITA_APPID  "GLEE00001")
set(VITA_TITLE  "Gleep Remote")
set(VITA_VERSION "01.00")

include(${VITASDK}/share/vita.cmake)
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME})
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_APPID} ${PROJECT_NAME} sce_sys)
add_vpk_property(${PROJECT_NAME}.vpk "TITLE" "${VITA_TITLE}")
add_vpk_property(${PROJECT_NAME}.vpk "APP_VER" "${VITA_VERSION}")
add_vpk_property(${PROJECT_NAME}.vpk "PARENTAL_LEVEL" "1")