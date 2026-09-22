#include <type_traits>
#define INTERRUPT 226
#include "EEPROM24Cxx/EEPROM24Cxx.h"
#include "EEPROM24Cxx/CommandTable.h"
#include "EEPROM24Cxx/Version.h"
static_assert(std::is_trivially_copyable<EEPROM24Cxx::Status>::value, "POD status");
static_assert(std::is_trivially_copyable<EEPROM24Cxx::TransferResult>::value, "POD result");
int main() { EEPROM24Cxx::EEPROM24Cxx memory; memory.end(); return 0; }
