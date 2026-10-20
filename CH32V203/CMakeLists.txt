cmake_minimum_required(VERSION 3.20)
project(DAPLink_CH32V203 C ASM)

# Chip: CH32V203C8T6 = 64 KB flash / 20 KB RAM (WCH family "D6" peripheral set).
set(ONBOARDDAP_USB_VID "0x1209" CACHE STRING "USB VID (default: pid.codes shared test ID, private bench use only)")
set(ONBOARDDAP_USB_PID "0x0001" CACHE STRING "USB PID (default: pid.codes shared test ID)")
option(DAP_FW_V1 "Build CMSIS-DAP v1 (HID) instead of v2 (WinUSB bulk)" OFF)
set(LINKER_SCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/link_c8t6.ld CACHE FILEPATH "Linker script")

set(ARCH_FLAGS -march=rv32imac_zicsr -mabi=ilp32 -msmall-data-limit=8 -mno-save-restore)
set(OPT_FLAGS  -std=gnu11 -Os -ffunction-sections -fdata-sections -fsigned-char -fmessage-length=0)

file(GLOB PERIPH ${CMAKE_CURRENT_SOURCE_DIR}/src/Peripheral/*.c)
file(GLOB USBLIB ${CMAKE_CURRENT_SOURCE_DIR}/src/USBLib/*.c)
file(GLOB USBUSR ${CMAKE_CURRENT_SOURCE_DIR}/src/USBUsr/*.c)
set(DAP_SRC
  ${CMAKE_CURRENT_SOURCE_DIR}/../DAP/Source/DAP.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../DAP/Source/DAP_vendor.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../DAP/Source/JTAG_DP.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../DAP/Source/SW_DP.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../DAP/Source/SWO.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../DAP/Source/UART.c)

add_executable(DAPLink
  src/main.c src/vcom_serial.c src/dap_xfer.c src/timestamp.c src/swo_usart.c src/target_power.c
  src/Startup/core_riscv.c src/Startup/system_ch32v20x.c src/Startup/startup_ch32v20x_D6.S
  ${PERIPH} ${USBLIB} ${USBUSR} ${DAP_SRC})

target_include_directories(DAPLink PRIVATE
  src src/Startup src/Peripheral src/USBLib src/USBUsr src/CMSIS_Driver ../DAP/Include)
target_compile_definitions(DAPLink PRIVATE CH32V20x_D6 STD_GCC)
target_compile_definitions(DAPLink PRIVATE ONBOARDDAP_USB_VID=${ONBOARDDAP_USB_VID} ONBOARDDAP_USB_PID=${ONBOARDDAP_USB_PID})
if(DAP_FW_V1)
  target_compile_definitions(DAPLink PRIVATE DAP_FW_V1)
endif()
target_compile_options(DAPLink PRIVATE ${ARCH_FLAGS} ${OPT_FLAGS} -Wall -Wno-unused)
target_link_options(DAPLink PRIVATE ${ARCH_FLAGS} -nostartfiles --specs=nano.specs --specs=nosys.specs
  -Wl,--gc-sections -Wl,-Map=DAPLink.map -T ${LINKER_SCRIPT})
set_target_properties(DAPLink PROPERTIES SUFFIX ".elf" LINK_DEPENDS ${LINKER_SCRIPT})

add_custom_command(TARGET DAPLink POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:DAPLink> DAPLink.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:DAPLink> DAPLink.hex
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:DAPLink>)
