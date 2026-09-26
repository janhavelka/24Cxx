// Arduino Print.h defines HEX before the shared CLI header in the real example.
#define HEX 16
#include "Eeprom24CxxCli.h"
#undef HEX
#include "WireTransportHelpers.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace EEPROM24Cxx;
struct Fixture {
  std::string output;
  std::vector<uint8_t> addresses;
  uint8_t memory[2048]{};
  unsigned transfers = 0;
  unsigned writes = 0;
  unsigned reads = 0;
  unsigned probes = 0;
  uint32_t ms = 0;
  uint32_t busyUntil = 0;
  bool writeProtected = false;
  bool failWrite = false;
  bool nackWrite = false;
  bool failRead = false;
  Geometry geometry = geometryFor(DeviceVariant::ZETTA_ZD24C02B);
  static uint32_t clock(void* user) { return static_cast<Fixture*>(user)->ms; }
  static void print(void* user, const char* format, va_list args) {
    char text[2048]; std::vsnprintf(text, sizeof(text), format, args);
    static_cast<Fixture*>(user)->output += text;
  }
  uint32_t address(uint8_t bus, const uint8_t* prefix) const {
    uint32_t value = prefix[0];
    if (geometry.wordAddressBytes == 2) value = (value << 8U) | prefix[1];
    const uint32_t bank = (bus >> geometry.bankAddressShift) & ((1U << geometry.bankAddressBits) - 1U);
    return value | (bank << (8U * geometry.wordAddressBytes));
  }
  static TransportResult write(uint8_t bus, const uint8_t* tx, size_t n, uint32_t, void* user) {
    auto& f = *static_cast<Fixture*>(user);
    ++f.transfers; ++f.writes; f.addresses.push_back(bus);
    if (f.ms < f.busyUntil) return TransportResult::Error(TransportCode::NACK_ADDRESS, 0, WriteCommit::NOT_COMMITTED);
    if (f.failWrite) { f.failWrite = false; f.busyUntil = f.ms + 5; return TransportResult::Error(TransportCode::TIMEOUT); }
    if (f.nackWrite) { f.nackWrite = false; return TransportResult::Error(TransportCode::NACK_ADDRESS, 0, WriteCommit::NOT_COMMITTED); }
    const uint32_t start = f.address(bus, tx);
    if (n <= f.geometry.wordAddressBytes) return TransportResult::Error(TransportCode::IO_ERROR);
    if (!f.writeProtected) for (size_t i = f.geometry.wordAddressBytes; i < n; ++i) {
      const uint32_t target = (start / f.geometry.pageSizeBytes) * f.geometry.pageSizeBytes +
          (start + static_cast<uint32_t>(i) - f.geometry.wordAddressBytes) % f.geometry.pageSizeBytes;
      if (target >= sizeof(f.memory)) return TransportResult::Error(TransportCode::IO_ERROR);
      f.memory[target] = tx[i];
    }
    f.busyUntil = f.ms + 5;
    return TransportResult::Ok(n, 0);
  }
  static TransportResult read(uint8_t bus, const uint8_t* tx, size_t n,
                               uint8_t* out, size_t count, uint32_t, void* user) {
    auto& f = *static_cast<Fixture*>(user);
    ++f.transfers; ++f.reads; f.addresses.push_back(bus);
    if (f.ms < f.busyUntil) return TransportResult::Error(TransportCode::NACK_ADDRESS);
    if (f.failRead) { f.failRead = false; return TransportResult::Error(TransportCode::TIMEOUT); }
    const uint32_t start = f.address(bus, tx);
    if (n != f.geometry.wordAddressBytes || start + count > sizeof(f.memory))
      return TransportResult::Error(TransportCode::IO_ERROR);
    std::memcpy(out, f.memory + start, count);
    return TransportResult::Ok(n, count);
  }
  static TransportResult probe(uint8_t bus, uint32_t, void* user) {
    auto& f = *static_cast<Fixture*>(user);
    ++f.transfers; ++f.probes; f.addresses.push_back(bus);
    if (bus < 0x50 || bus > 0x57 || f.ms < f.busyUntil)
      return TransportResult::Error(TransportCode::NACK_ADDRESS);
    return TransportResult::Ok(0, 0);
  }
  static Status scan(uint8_t bus, void* user) {
    const auto result = probe(bus, 50, user);
    return result.ok() ? Status::Ok() : Status::Error(Err::I2C_NACK_ADDR, "probe NACK");
  }
  Config config() {
    Config c;
    c.i2cWrite = write; c.i2cWriteRead = read; c.i2cProbe = probe; c.i2cUser = this;
    c.nowMs = clock; c.timeUser = this; c.maxTxBytes = 17; c.maxRxBytes = 16;
    c.offlineThreshold = 2;
    return c;
  }
  eeprom24cxx_cli::Platform platform() {
    eeprom24cxx_cli::Platform p;
    p.vprintf = print; p.nowMs = clock; p.probeAddress = scan; p.user = this;
    return p;
  }
  bool run(eeprom24cxx_cli::Cli& cli, unsigned ticks = 80) {
    for (unsigned i = 0; i < ticks; ++i) {
      const unsigned before = transfers;
      cli.tick();
      if (transfers > before + 1) return false;
      ++ms;
    }
    return true;
  }
};
#define CHECK(x) do { if (!(x)) { std::printf("[FAIL] CLI check line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main() {
  // ESP32 Wire code 2 is ambiguous for payload transactions. An address-only
  // probe is the only context in which it proves an address NACK.
  const auto ambiguous = eeprom24cxx_cli::wireResult(2, 9);
  CHECK(ambiguous.code == TransportCode::NACK_UNSPECIFIED);
  CHECK(ambiguous.writeCommit == WriteCommit::INDETERMINATE);
  CHECK(ambiguous.completedTxBytes == 0);
  CHECK(eeprom24cxx_cli::wireResult(2, 0, 0, true).code == TransportCode::NACK_ADDRESS);
  CHECK(eeprom24cxx_cli::wireResult(0, 9).completedTxBytes == 9);
  // Model precisely the relevant Wire mutex/buffer cleanup behavior, not an
  // entire framework. Flushing must precede STOP; null storage uses no I2C.
  struct BufferedWire {
    size_t length = 3;
    bool locked = true;
    bool nullBuffer = false;
    unsigned physical = 0;
    unsigned payloadSent = 0;
    void flush() { length = 0; }
    uint8_t endTransmission(bool) {
      if (nullBuffer) return 4; // Pinned Wire does not unlock this path.
      ++physical; payloadSent += static_cast<unsigned>(length); locked = false; return 0;
    }
    size_t requestFrom(uint8_t, size_t, bool) {
      if (!nullBuffer) ++physical;
      locked = false; return 0;
    }
  } wire;
  CHECK(eeprom24cxx_cli::discardWireTx(wire, 0x50, 3).physicalAttempt);
  CHECK(!wire.locked && wire.physical == 1 && wire.payloadSent == 0);
  wire = {}; wire.nullBuffer = true;
  CHECK(!eeprom24cxx_cli::discardWireTx(wire, 0x50, 0).physicalAttempt);
  CHECK(!wire.locked && wire.physical == 0 && wire.payloadSent == 0);
  Fixture f; eeprom24cxx_cli::Cli cli;
  cli.setup(f.platform(), f.config());
  CHECK(f.writes == 0 && f.probes == 1);
  f.output.clear(); cli.processCommand("help");
  CHECK(f.output.find("\033[36m=== EEPROM24Cxx CLI Help ===\033[0m") != std::string::npos);
  CHECK(f.output.find("\033[32m[Configuration]\033[0m") != std::string::npos);
  CHECK(f.output.find("\033[36mhelp / ?                        \033[0m -") != std::string::npos);
  CHECK(f.output.find("fill <addr> <byte> <N>") != std::string::npos);
  CHECK(f.output.find("wverify") != std::string::npos);
  unsigned before = f.transfers;
  for (const char* command : {"health", "drv", "state", "online", "settings", "cfg", "snapshot", "version", "ver", "diag", "progress", "status", "?", "variants", "size", "heap", "verbose", "addr", "timeout", "model"})
    cli.processCommand(command);
  CHECK(f.transfers == before && f.writes == 0);
  for (const char* command : {"write 0 256", "write 0 -1", "write 0 1 invalid", "fill 0 1 0", "read -1", "read 0 257", "readbyte 0 2", "verify 0", "addr 0x40", "stress -1", "writebyte 0 1 2", "help extra", "write 4294967296 1"}) {
    cli.processCommand(command); CHECK(f.transfers == before);
  }
  cli.processCommand("color off"); f.output.clear(); cli.processCommand("help");
  CHECK(f.output.find('\033') == std::string::npos);
  for (char ch : std::string("writebyte 0 99 ") + std::string(280, ' ')) cli.feed(ch);
  cli.feed('\n'); CHECK(f.transfers == before); CHECK(f.writes == 0);
  // Invalid control characters must not silently combine into a valid mutation.
  for (char ch : std::string("writebyte 0 9") + char(1) + "9\n") cli.feed(ch);
  CHECK(f.transfers == before);
  // Direct calls have the same strict character contract as serial input.
  // strtoul accepts leading C whitespace/signs, so reject control characters
  // before tokenization rather than allowing them to conceal a write operand.
  for (const char* command : {"writebyte 0 \v+9", "writebyte 0 \f9", "writebyte 0 \r9", "writebyte 0 \n9", "writebyte 0 \x80" "9",
                              "crc 0", "crc 0 1 extra", "strings 0", "strings 0 1 65", "strings 0 1 0", "text 0 0", "verbose 2", "size extra", "selftest extra"}) {
    f.output.clear(); cli.processCommand(command);
    CHECK(f.output.find("INVALID_PARAM") != std::string::npos);
    CHECK(f.run(cli, 1)); CHECK(f.transfers == before);
  }
  f.output.clear(); cli.processCommand("wverify 6 0xAA 0xBB 0xCC 0xDD");
  CHECK(f.transfers == before); // Admission only, no synchronous write.
  cli.tick(); CHECK(f.writes == 1);
  // Reject commands before parsing their data into the active borrowed buffer.
  cli.processCommand("write 6 1 2 3 4");
  CHECK(f.output.find("BUSY") != std::string::npos);
  CHECK(f.run(cli));
  CHECK(f.writes == 2 && f.memory[6] == 0xAA && f.memory[7] == 0xBB && f.memory[8] == 0xCC && f.memory[9] == 0xDD);
  CHECK(f.output.find("verified=4 commit=VERIFIED") != std::string::npos);
  f.output.clear(); cli.processCommand("progress");
  CHECK(f.output.find("verified=4 commit=VERIFIED") != std::string::npos);
  before = f.transfers;
  cli.processCommand("health"); CHECK(f.transfers == before);
  CHECK(f.output.find("write-busy-polls=") != std::string::npos);
  f.output.clear(); cli.processCommand("read 6 4"); CHECK(f.run(cli));
  CHECK(f.output.find("00006: AA BB CC DD") != std::string::npos);
  const unsigned previousWrites = f.writes;
  cli.processCommand("verify 6 0xAA 0xBB 0xCC 0xDD"); CHECK(f.run(cli));
  CHECK(f.writes == previousWrites);
  f.output.clear(); cli.processCommand("fillverify 20 0xA5 9"); CHECK(f.run(cli));
  for (size_t i = 20; i < 29; ++i) CHECK(f.memory[i] == 0xA5);
  CHECK(f.memory[29] == 0);
  f.writeProtected = true;
  f.output.clear(); cli.processCommand("wverify 20 0"); CHECK(f.run(cli));
  CHECK(f.output.find("VERIFY_MISMATCH") != std::string::npos);
  CHECK(f.output.find("Write: status=OK detail=0") != std::string::npos);
  CHECK(f.output.find("Verification: status=VERIFY_MISMATCH") != std::string::npos);
  CHECK(f.memory[20] == 0xA5);
  f.writeProtected = false;
  f.failWrite = true;
  before = f.writes;
  f.output.clear(); cli.processCommand("writebyte 30 99"); CHECK(f.run(cli));
  CHECK(f.writes == before + 1); CHECK(f.output.find("commit=INDETERMINATE") != std::string::npos);
  // Cancel after the first page: preserve accepted prefix and the tWR barrier.
  f.output.clear(); cli.processCommand("fill 40 0x55 24"); cli.tick();
  before = f.writes;
  cli.processCommand("cancel");
  CHECK(f.output.find("CANCELLED") != std::string::npos);
  CHECK(f.output.find("accepted=8 completed=0") != std::string::npos);
  CHECK(f.run(cli)); CHECK(f.writes == before);
  cli.processCommand("readbyte 40"); CHECK(f.run(cli));
  CHECK(f.memory[40] == 0x55 && f.memory[48] == 0);
  // Finite stress consumes failures and remains read-only.
  before = f.writes; f.output.clear(); f.failRead = true;
  cli.processCommand("stress 3"); CHECK(f.run(cli));
  CHECK(f.output.find("Stress stopped: ok=2 fail=1") != std::string::npos);
  CHECK(f.writes == before);
  before = f.transfers; CHECK(f.run(cli)); CHECK(f.transfers == before);
  cli.processCommand("discover"); CHECK(f.transfers == before);
  CHECK(f.run(cli, 8)); CHECK(f.transfers == before + 8);
  CHECK(f.output.find("bank candidate; identity unverified") != std::string::npos);
  before = f.transfers; cli.processCommand("scan"); cli.tick();
  cli.processCommand("stop"); CHECK(f.run(cli)); CHECK(f.transfers == before + 1);
  // Staged address/model survives end/bind and bank address selection is visible.
  before = f.transfers; cli.processCommand("end"); cli.processCommand("model 24c04");
  cli.processCommand("addr 0x52"); CHECK(f.transfers == before);
  f.geometry = geometryFor(DeviceVariant::C04);
  cli.processCommand("bind"); CHECK(f.transfers == before);
  cli.processCommand("read 0xFF 2"); CHECK(f.run(cli));
  CHECK(f.addresses[f.addresses.size() - 2] == 0x52 && f.addresses.back() == 0x53);
  before = f.transfers; cli.processCommand("unbind"); cli.processCommand("writebyte 0 12");
  CHECK(f.transfers == before);
  cli.processCommand("begin"); CHECK(f.transfers == before + 1);
  // Numbers without an explicit hex prefix are decimal, including leading
  // zeroes. Interpreting a write address as octal could modify the wrong byte.
  cli.processCommand("writebyte 010 010"); CHECK(f.run(cli));
  CHECK(f.memory[10] == 10);
  cli.processCommand("writebyte 08 08"); CHECK(f.run(cli));
  CHECK(f.memory[8] == 8);
  cli.processCommand("writebyte 0x0B 0X0C"); CHECK(f.run(cli));
  CHECK(f.memory[11] == 12);
  // A definite first page remains accepted when the following page address
  // NACK proves that newest chunk accepted nothing. Display both facts.
  Fixture partial;
  eeprom24cxx_cli::Cli partialCli;
  partialCli.setup(partial.platform(), partial.config());
  partialCli.processCommand("color off");
  partial.output.clear(); partialCli.processCommand("fill 0 0x66 16");
  partialCli.tick(); CHECK(partial.writes == 1);
  partial.ms += 6; partial.nackWrite = true;
  CHECK(partial.run(partialCli));
  CHECK(partial.writes == 2 && partial.memory[0] == 0x66 && partial.memory[8] == 0);
  CHECK(partial.output.find("accepted=8") != std::string::npos);
  CHECK(partial.output.find("commit=ACCEPTED last-chunk-commit=NOT_COMMITTED") != std::string::npos);
  // Config clock is a valid fallback. Without either clock, writes/stress must
  // fail before admission rather than remain pending forever at timestamp 0.
  Fixture fallback;
  eeprom24cxx_cli::Cli fallbackCli;
  auto fallbackPlatform = fallback.platform(); fallbackPlatform.nowMs = nullptr;
  fallbackCli.setup(fallbackPlatform, fallback.config());
  fallbackCli.processCommand("writebyte 1 42"); CHECK(fallback.run(fallbackCli));
  CHECK(fallback.memory[1] == 42);
  Fixture clockless;
  eeprom24cxx_cli::Cli clocklessCli;
  auto clocklessPlatform = clockless.platform(); clocklessPlatform.nowMs = nullptr;
  auto clocklessConfig = clockless.config(); clocklessConfig.nowMs = nullptr;
  clocklessCli.setup(clocklessPlatform, clocklessConfig);
  before = clockless.transfers;
  clocklessCli.processCommand("writebyte 1 42"); clocklessCli.processCommand("stress 2");
  CHECK(clockless.run(clocklessCli)); CHECK(clockless.transfers == before);
  CHECK(clockless.output.find("requires a clock") != std::string::npos);
  // MB85RC-compatible read-only views operate on the full configured range,
  // including address banks, with bounded fixed buffers and one callback/tick.
  Fixture views;
  views.geometry = geometryFor(DeviceVariant::C16);
  auto viewConfig = views.config(); viewConfig.variant = DeviceVariant::C16;
  auto viewPlatform = views.platform();
  viewPlatform.heapStats = [](void*) { return eeprom24cxx_cli::HeapStats{1234, 900, 700}; };
  eeprom24cxx_cli::Cli viewCli;
  viewCli.setup(viewPlatform, viewConfig); viewCli.processCommand("color off");
  for (unsigned index = 0; index < 512; ++index) views.memory[index] = static_cast<uint8_t>(index);
  views.output.clear(); before = views.transfers;
  viewCli.processCommand("crc 0 512"); CHECK(views.transfers == before);
  CHECK(views.run(viewCli)); CHECK(views.transfers == before + 32);
  CHECK(views.output.find("length=512 crc=0x1C613576") != std::string::npos);
  CHECK(views.output.find("kind=READ state=SUCCEEDED") != std::string::npos);
  CHECK(views.output.find("requested=512 accepted=0 completed=512") != std::string::npos);
  views.output.clear(); viewCli.processCommand("status");
  CHECK(views.output.find("completed=512") != std::string::npos);
  std::memcpy(views.memory, "123456789", 9);
  views.output.clear(); viewCli.processCommand("crc 0 9"); CHECK(views.run(viewCli));
  CHECK(views.output.find("crc=0xCBF43926") != std::string::npos);
  views.output.clear(); viewCli.processCommand("hexdump 0 9"); CHECK(views.run(viewCli));
  CHECK(views.output.find("|123456789|") != std::string::npos);
  views.memory[0] = 'A'; views.memory[1] = '\n'; views.memory[2] = '\033';
  views.memory[3] = '\\'; views.memory[4] = '"'; views.memory[5] = 0xFF;
  views.output.clear(); viewCli.processCommand("text 0 6"); CHECK(views.run(viewCli));
  CHECK(views.output.find("00000: \"A\\x0A\\x1B\\\\\\\"\\xFF\"") != std::string::npos);
  CHECK(views.output.find('\033') == std::string::npos);
  std::memset(views.memory, 0, sizeof(views.memory));
  std::memcpy(views.memory + 253, "cross-bank string", 17);
  std::memcpy(views.memory + 300, "end", 3);
  views.output.clear(); viewCli.processCommand("strings 240 80"); CHECK(views.run(viewCli));
  CHECK(views.output.find("000FD: cross-bank string") != std::string::npos);
  CHECK(views.output.find(": end") == std::string::npos);
  CHECK(views.output.find("Strings: 1 found") != std::string::npos);
  // Retain a possible string prefix across a CLI buffer boundary as well as
  // a physical read boundary, including the maximum supported minimum.
  std::memset(views.memory, 0, sizeof(views.memory));
  std::memset(views.memory + 250, 'Q', 70);
  views.output.clear(); viewCli.processCommand("strings 0 320 64"); CHECK(views.run(viewCli));
  CHECK(views.output.find(std::string("000FA: ") + std::string(70, 'Q')) != std::string::npos);
  CHECK(views.output.find("Strings: 1 found in 320 completed bytes") != std::string::npos);
  views.output.clear(); viewCli.processCommand("heap");
  CHECK(views.output.find("Heap: free=1234 minimum-free=900 largest-free-block=700 bytes") != std::string::npos);
  // Full-chip defaults are finite and read-only.
  views.output.clear(); before = views.transfers;
  viewCli.processCommand("strings"); CHECK(views.run(viewCli, 128));
  CHECK(views.transfers == before + 128);
  CHECK(views.output.find("2048 completed bytes") != std::string::npos);
  CHECK(views.writes == 0);
  // Stop between chunks as well as in a physical chunk; cancelled or failed
  // reads must never print a checksum as though it covered the entire range.
  views.output.clear(); viewCli.processCommand("crc 0 512");
  viewCli.processCommand("progress");
  const auto idStart = views.output.find("request-id=");
  CHECK(idStart != std::string::npos);
  const auto rangeId = views.output.substr(idStart, views.output.find('\n', idStart) - idStart);
  CHECK(rangeId != "request-id=0");
  CHECK(views.run(viewCli, 16));
  views.output.clear();
  viewCli.processCommand("progress");
  CHECK(views.output.find("requested=512 accepted=0 completed=256") != std::string::npos);
  CHECK(views.output.find(rangeId) != std::string::npos);
  views.output.clear();
  before = views.transfers; viewCli.processCommand("cancel"); CHECK(views.run(viewCli));
  CHECK(views.transfers == before);
  CHECK(views.output.find("state=CANCELLED") != std::string::npos);
  CHECK(views.output.find(rangeId) != std::string::npos);
  CHECK(views.output.find("crc=0x") == std::string::npos);
  views.output.clear(); viewCli.processCommand("crc 0 512"); viewCli.processCommand("progress");
  const auto nextIdStart = views.output.find("request-id=");
  CHECK(nextIdStart != std::string::npos);
  const auto nextRangeId = views.output.substr(nextIdStart, views.output.find('\n', nextIdStart) - nextIdStart);
  CHECK(views.run(viewCli, 17));
  views.output.clear(); viewCli.processCommand("progress");
  CHECK(views.output.find(nextRangeId) != std::string::npos);
  views.output.clear();
  before = views.transfers; viewCli.processCommand("cancel"); CHECK(views.run(viewCli));
  CHECK(views.transfers == before);
  CHECK(views.output.find(nextRangeId) != std::string::npos);
  CHECK(views.output.find("completed=272") != std::string::npos);
  CHECK(views.output.find("crc=0x") == std::string::npos);
  views.output.clear(); viewCli.processCommand("crc 0 512"); CHECK(views.run(viewCli, 16));
  views.failRead = true; CHECK(views.run(viewCli));
  CHECK(views.output.find("status=I2C_TIMEOUT") != std::string::npos);
  CHECK(views.output.find("completed=256") != std::string::npos);
  CHECK(views.output.find("chunk-offset=256") != std::string::npos);
  CHECK(views.output.find("crc=0x") == std::string::npos);
  views.output.clear(); before = views.transfers;
  for (const char* command : {"crc 2040 9", "read 0 2049", "text 4294967295 2", "strings 2048 1"}) {
    viewCli.processCommand(command); CHECK(views.run(viewCli, 1));
    CHECK(views.transfers == before);
  }
  CHECK(views.output.find("ADDRESS_OUT_OF_RANGE") != std::string::npos);
  // Cached queries remain available while a long read owns its borrowed buffer.
  views.output.clear(); viewCli.processCommand("read 0 512"); CHECK(views.run(viewCli, 1));
  before = views.transfers;
  for (const char* command : {"addr", "model", "size", "timeout", "heap", "variants", "settings", "health", "progress"}) {
    viewCli.processCommand(command); CHECK(views.transfers == before);
  }
  CHECK(views.output.find("BUSY") == std::string::npos);
  viewCli.processCommand("writebyte 0 1"); CHECK(views.output.find("BUSY") != std::string::npos);
  viewCli.processCommand("stop");
  views.output.clear(); viewCli.processCommand("selftest"); CHECK(views.run(viewCli));
  CHECK(views.output.find("Read-only selftest: PASS") != std::string::npos);
  views.output.clear(); views.failRead = true;
  viewCli.processCommand("selfcheck"); CHECK(views.run(viewCli));
  CHECK(views.output.find("Read-only selftest: FAIL") != std::string::npos);
  views.output.clear(); viewCli.processCommand("stress 2"); CHECK(views.run(viewCli));
  CHECK(views.output.find("kind=READ") == std::string::npos);
  CHECK(views.output.find("Stress stopped: ok=2 fail=0") != std::string::npos);
  views.output.clear(); viewCli.processCommand("verbose on"); viewCli.processCommand("stress 1");
  CHECK(views.run(viewCli)); CHECK(views.output.find("kind=READ") != std::string::npos);
  CHECK(views.writes == 0);
  std::puts("[PASS] CLI startup, parsing, memory views, CRC, page writes, verification, cancellation, health and read-only diagnostics");
  return 0;
}
