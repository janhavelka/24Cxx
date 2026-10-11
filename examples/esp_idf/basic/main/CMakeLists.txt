# ESP-IDF uses the checkout directory as the component name. Do not require
# consumers to name their checkout EEPROM24Cxx (archive and package names vary).
get_filename_component(EEPROM24CXX_COMPONENT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
get_filename_component(EEPROM24CXX_COMPONENT_NAME "${EEPROM24CXX_COMPONENT_DIR}" NAME)
set(EEPROM24CXX_CONSOLE_COMPONENT esp_driver_uart)
if(IDF_TARGET STREQUAL "esp32s3")
  set(EEPROM24CXX_CONSOLE_COMPONENT esp_driver_usb_serial_jtag)
endif()
idf_component_register(
  SRCS "main.cpp" "../../../common/Eeprom24CxxCli.cpp"
  INCLUDE_DIRS "../../../common" "../../../../include"
  REQUIRES ${EEPROM24CXX_COMPONENT_NAME} esp_driver_i2c esp_timer esp_driver_gpio ${EEPROM24CXX_CONSOLE_COMPONENT}
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
