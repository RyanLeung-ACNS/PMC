# Cloud cross-build of InitialTesting for size checks (not the NECTO build).
# It uses the prebuilt mikroSDK libraries committed in .meproject and builds
# LVGL from lvgl_940 with the heavy lv_conf, as NECTO does for a 1 MB part.
cmake_minimum_required(VERSION 3.19)
project(pmc_xbuild C ASM)

set(PROJ ${CMAKE_CURRENT_SOURCE_DIR}/../../InitialTesting)
# As InitialTesting/CMakeLists.txt: Release is -Os. NECTO links the Release
# mikroSDK libraries for a Release build and the Debug ones otherwise.
set(CMAKE_C_FLAGS_RELEASE "-Os -DNDEBUG")
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(SDK_CFG Release)
else()
    set(SDK_CFG Debug)
endif()
set(SDK ${PROJ}/.meproject/setup/${SDK_CFG})
set(CMAKE_PREFIX_PATH ${SDK}/lib/cmake)
file(GLOB _pkgs RELATIVE ${SDK}/lib/cmake ${SDK}/lib/cmake/*)
foreach(p ${_pkgs})
    set(${p}_DIR ${SDK}/lib/cmake/${p})
endforeach()

foreach(p MikroC.Core MikroSDK.LVGL.Common MikroSDK.GraphicLibrary MikroSDK.USB.Tiny MikroSDK.Ethernet.CycloneTCP)
    find_package(${p} REQUIRED CONFIG)
endforeach()

file(GLOB_RECURSE LVGL_SRC ${PROJ}/lvgl_940/src/*.c)
add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC ${PROJ}/lvgl_940 ${PROJ}/lvgl_940/lv_conf/heavy)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE LV_LVGL_H_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -w)

file(GLOB APP_SRC ${PROJ}/*.c ${PROJ}/driver/*.c ${PROJ}/build-MM4/generated/*.c)
add_executable(InitialTesting.elf ${APP_SRC} ${PROJ}/.meproject/setup/FileStartup/stm32f407zg.s)
target_include_directories(InitialTesting.elf PRIVATE ${PROJ} ${PROJ}/driver ${PROJ}/1ms_timer ${PROJ}/1ms_timer/arm ${PROJ}/build-MM4/generated)
target_compile_definitions(InitialTesting.elf PRIVATE
    ARM_TIMER=1 CLEAR_FLAG=0
    "INTERRUPT_ROUTINE=__attribute__ ((interrupt(\"IRQ\"))) void SysTick_Handler(void)")
target_link_libraries(InitialTesting.elf PRIVATE MikroC.Core MikroSDK.LVGL.Common MikroSDK.GraphicLibrary
    MikroSDK.USB.Tiny MikroSDK.Ethernet.CycloneTCP lvgl m)
target_link_options(InitialTesting.elf PRIVATE -T${PROJ}/.meproject/setup/FileLinker/stm32f407zg.ld
    -Wl,-Map=${CMAKE_BINARY_DIR}/InitialTesting.map)
