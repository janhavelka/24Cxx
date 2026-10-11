// Arduino Print.h defines HEX before the shared CLI header in the real example.
#define HEX 16
#include "Eeprom24CxxCli.h"
#undef HEX
#include "../support/CliFixture.h"

#define CHECK(x) do { if (!(x)) { std::printf("[FAIL] CLI check line %d: %s\n", __LINE__, #x); return 1; } } while (false)
int main() {
  Fixture f; eeprom24cxx_cli::Cli cli;
  cli.setup(f.platform(), f.config());
  CHECK(f.writes == 0 && f.probes == 1);
  CHECK(f.output.find("bound=yes consec=0 ok=1 fail=0") != std::string::npos);
  CHECK(f.output.find("=== EEPROM24Cxx CLI Help ===") != std::string::npos);
  Fixture failedStartup; failedStartup.failProbe = true;
  eeprom24cxx_cli::Cli failedStartupCli; failedStartupCli.setup(failedStartup.platform(), failedStartup.config());
  CHECK(failedStartup.writes == 0 && failedStartup.probes == 1);
  CHECK(failedStartup.output.find("DEGRADED") != std::string::npos);
  CHECK(failedStartup.output.find("bound=yes consec=1 ok=0 fail=1") != std::string::npos);
  // The scan adapter must preserve a timeout instead of counting it as an
  // ordinary absent address. Scans bypass the driver's passive health.
  failedStartup.output.clear(); failedStartup.failProbe = true;
  failedStartupCli.processCommand("discover"); CHECK(failedStartup.run(failedStartupCli, 8));
  CHECK(failedStartup.output.find("Scan complete: 7 ACK, 1 other transport errors.") != std::string::npos);
  failedStartupCli.processCommand("health");
  CHECK(failedStartup.output.find("bound=yes consec=1 ok=0 fail=1") != std::string::npos);
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
  views.output.clear(); before = views.transfers; viewCli.processCommand("selftest"); CHECK(views.run(viewCli, 160));
  CHECK(views.output.find("Read-only selftest: PASS") != std::string::npos);
  CHECK(views.output.find("bytes=2048 crc=0x") != std::string::npos);
  CHECK(views.transfers == before + 128);
  CHECK(views.output.find("00000:") == std::string::npos); // Compact full-array report.
  views.output.clear(); views.failRead = true;
  viewCli.processCommand("selfcheck"); CHECK(views.run(viewCli));
  CHECK(views.output.find("Read-only selftest: FAIL") != std::string::npos);
  views.output.clear(); viewCli.processCommand("stress 2"); CHECK(views.run(viewCli));
  CHECK(views.output.find("kind=READ") == std::string::npos);
  CHECK(views.output.find("Stress stopped: ok=2 fail=0") != std::string::npos);
  views.output.clear(); viewCli.processCommand("verbose on"); viewCli.processCommand("stress 1");
  CHECK(views.run(viewCli)); CHECK(views.output.find("kind=READ") != std::string::npos);
  CHECK(views.writes == 0);
  // Cooperative field diagnostics, counters and current-address reads.
  Fixture field; eeprom24cxx_cli::Cli fieldCli;
  fieldCli.setup(field.platform(), field.config()); fieldCli.processCommand("color off");
  for (unsigned index = 0; index < sizeof(field.memory); ++index)
    field.memory[index] = static_cast<uint8_t>(index ^ 0x63U);
  before = field.transfers;
  for (const char* command : {"geometry", "timing", "page 7", "offline", "stats", "xfer_stats", "wp", "job", "result", "scratch"})
    fieldCli.processCommand(command);
  CHECK(field.transfers == before);
  CHECK(field.output.find("number=0 offset=7 remaining=1") != std::string::npos);
  fieldCli.processCommand("xfer_reset"); field.output.clear(); fieldCli.processCommand("xfer_assert 0 0 0 0");
  CHECK(field.output.find("[I] OK") != std::string::npos);
  field.output.clear(); fieldCli.processCommand("xfer_assert 1");
  CHECK(field.output.find("VERIFY_MISMATCH") != std::string::npos);
  fieldCli.processCommand("readbyte 10"); CHECK(field.run(fieldCli));
  field.output.clear(); fieldCli.processCommand("current 2"); CHECK(field.run(fieldCli));
  CHECK(field.output.find("kind=CURRENT_READ") != std::string::npos);
  CHECK(field.output.find("0000B: 68 6F") != std::string::npos);
  CHECK(field.writes == 0);
  field.output.clear(); fieldCli.processCommand("watch 10 4 3 2"); CHECK(field.run(fieldCli));
  CHECK(field.output.find("Watch stopped: ok=3 fail=0") != std::string::npos);
  before = field.transfers; CHECK(field.run(fieldCli)); CHECK(field.transfers == before);
  fieldCli.processCommand("watch 10 4 10 5"); fieldCli.tick();
  before = field.transfers; fieldCli.processCommand("stop"); CHECK(field.run(fieldCli)); CHECK(field.transfers == before);
  fieldCli.processCommand("wp 1"); CHECK(field.writeProtected);
  fieldCli.processCommand("wp 0"); CHECK(!field.writeProtected);
  fieldCli.processCommand("end"); fieldCli.processCommand("offline 3"); fieldCli.processCommand("init 0x52");
  CHECK(field.addresses.back() == 0x52);
  field.output.clear(); fieldCli.processCommand("settings"); CHECK(field.output.find("offline-threshold=3") != std::string::npos);
  fieldCli.processCommand("iface_reset"); CHECK(field.resets == 1);
  before = field.transfers; fieldCli.processCommand("readbyte 0"); CHECK(field.transfers == before);
  CHECK(field.run(fieldCli, 7)); CHECK(field.transfers == before);
  fieldCli.processCommand("readbyte 0"); CHECK(field.run(fieldCli));
  CHECK(field.transfers == before + 1);
  // Even a failed electrical recovery may emit STOP. Stop/end and probes/scans
  // cannot bypass its post-callback settling barrier.
  field.failReset = true; fieldCli.processCommand("iface_reset");
  CHECK(field.resets == 2); before = field.transfers;
  fieldCli.processCommand("stop"); fieldCli.processCommand("end");
  for (const char* command : {"probe", "discover", "current", "begin"}) fieldCli.processCommand(command);
  CHECK(field.run(fieldCli, 5)); CHECK(field.transfers == before);
  CHECK(field.run(fieldCli, 2)); fieldCli.processCommand("bind"); field.failReset = false;
  field.output.clear(); fieldCli.processCommand("stats reset");
  CHECK(field.output.find("total=0 ok=0 fail=0") != std::string::npos);
  CHECK(field.output.find("passive driver health preserved") != std::string::npos);
  before = field.transfers;
  for (const char* command : {"rw_suite 0 8", "rw_suite 0 257 confirm", "rw_suite 0 8 yes", "stress_mix 0 8 0 confirm",
                              "randbench 0 8 1001 confirm", "typed_demo 0 extra confirm", "restore yes", "current 0",
                              "watch 0 257", "watch 0 4 1 0", "xfer_assert 0 0", "wp 2", "init 0x58", "page -1"}) {
    field.output.clear(); fieldCli.processCommand(command); CHECK(field.run(fieldCli, 1));
    CHECK(field.output.find("INVALID_PARAM") != std::string::npos); CHECK(field.transfers == before);
  }
  fieldCli.processCommand("end"); fieldCli.processCommand("model 24c01");
  field.output.clear(); fieldCli.processCommand("page 127");
  CHECK(field.output.find("remaining=1 bank-remaining=1") != std::string::npos);
  CHECK(field.transfers == before);
  // All scratch commands restore nontrivial original data, including an
  // unaligned region spanning physical pages. Each tick remains bounded.
  Fixture scratch; eeprom24cxx_cli::Cli scratchCli;
  scratchCli.setup(scratch.platform(), scratch.config()); scratchCli.processCommand("color off");
  for (unsigned index = 0; index < sizeof(scratch.memory); ++index)
    scratch.memory[index] = static_cast<uint8_t>((index * 29U) ^ 0xD3U);
  const std::vector<uint8_t> original(scratch.memory, scratch.memory + sizeof(scratch.memory));
  before = scratch.transfers; scratchCli.processCommand("rw_suite 6 19 confirm"); CHECK(scratch.transfers == before);
  CHECK(scratch.run(scratchCli, 500));
  CHECK(std::memcmp(scratch.memory, original.data(), original.size()) == 0);
  CHECK(scratch.output.find("primary=OK detail=0 restore=OK") != std::string::npos);
  CHECK(scratch.output.find("compared=19 skipped=19") != std::string::npos);
  CHECK(scratch.output.find("Original scratch bytes observed") != std::string::npos);
  scratch.output.clear(); before = scratch.writes;
  scratchCli.processCommand("stress_mix 32 8 4 confirm"); CHECK(scratch.run(scratchCli, 200));
  CHECK(scratch.writes == before + 13);
  CHECK(std::memcmp(scratch.memory, original.data(), original.size()) == 0);
  scratch.output.clear(); before = scratch.writes;
  scratchCli.processCommand("randbench 32 16 5 confirm"); CHECK(scratch.run(scratchCli, 200));
  CHECK(scratch.writes == before + 7);
  CHECK(std::memcmp(scratch.memory, original.data(), original.size()) == 0);
  scratch.output.clear(); scratchCli.processCommand("xfer_demo 32 8 confirm"); CHECK(scratch.run(scratchCli, 100));
  CHECK(scratch.output.find("rounds=4/4") != std::string::npos);
  CHECK(std::memcmp(scratch.memory, original.data(), original.size()) == 0);
  scratchCli.processCommand("typed_demo 32 confirm");
  CHECK(scratch.run(scratchCli, 200));
  CHECK(scratch.output.find("name=typed-le-values result=PASS") != std::string::npos);
  CHECK(scratch.output.find("name=typed-be-signed-values result=PASS") != std::string::npos);
  CHECK(std::memcmp(scratch.memory, original.data(), original.size()) == 0);
  // A backup read failure cannot schedule any programming or claim restore.
  scratch.output.clear(); before = scratch.writes; scratch.failRead = true;
  scratchCli.processCommand("rw_suite 32 8 confirm"); CHECK(scratch.run(scratchCli));
  CHECK(scratch.writes == before && scratch.output.find("backup=none") != std::string::npos);
  CHECK(scratch.output.find("primary=I2C_TIMEOUT") != std::string::npos);
  // Stop after the first page is issued: no restoration or subsequent page is
  // silently sent. Backup survives end/rebind and rejects target changes.
  scratch.output.clear(); scratchCli.processCommand("rw_suite 6 16 confirm");
  CHECK(scratch.run(scratchCli, 2)); // Backup, then first physical page write.
  before = scratch.transfers; const unsigned stoppedWrites = scratch.writes;
  scratchCli.processCommand("stop"); CHECK(scratch.transfers == before);
  scratch.output.clear(); scratchCli.processCommand("discover"); scratchCli.processCommand("wp 1");
  CHECK(scratch.transfers == before && !scratch.writeProtected);
  CHECK(scratch.output.find("BUSY") != std::string::npos);
  scratchCli.processCommand("scratch");
  CHECK(scratch.run(scratchCli)); CHECK(scratch.writes == stoppedWrites);
  CHECK(scratch.output.find("backup=retained restore-required=yes primary=CANCELLED") != std::string::npos);
  CHECK(std::memcmp(scratch.memory, original.data(), original.size()) != 0);
  scratchCli.processCommand("end");
  scratch.output.clear(); scratchCli.processCommand("model 24c04"); scratchCli.processCommand("begin 0x51");
  scratchCli.processCommand("writebyte 0 1");
  CHECK(scratch.output.find("BUSY") != std::string::npos); CHECK(scratch.transfers == before);
  scratchCli.processCommand("bind");
  scratch.output.clear(); scratchCli.processCommand("restore confirm"); CHECK(scratch.transfers == before);
  CHECK(scratch.run(scratchCli, 200));
  CHECK(std::memcmp(scratch.memory, original.data(), original.size()) == 0);
  CHECK(scratch.output.find("primary=CANCELLED detail=0 restore=OK") != std::string::npos);
  // An ambiguous pattern failure retains backup, requires explicit restore,
  // and is never silently replayed even after its physical barrier settles.
  scratch.output.clear(); before = scratch.writes; scratch.failWrite = true;
  scratchCli.processCommand("xfer_demo 32 8 confirm"); CHECK(scratch.run(scratchCli));
  CHECK(scratch.writes == before + 1);
  CHECK(scratch.output.find("commit=INDETERMINATE") != std::string::npos);
  CHECK(scratch.output.find("primary=I2C_TIMEOUT detail=0 restore=NO_RESULT") != std::string::npos);
  scratchCli.processCommand("restore confirm"); CHECK(scratch.run(scratchCli));
  CHECK(std::memcmp(scratch.memory, original.data(), original.size()) == 0);
  // Restore failure preserves a successful primary result and the original
  // backup. The second explicit restore is a new owner-authorized attempt.
  scratch.output.clear(); scratch.failWriteAttempt = scratch.writes + 3;
  scratchCli.processCommand("xfer_demo 32 8 confirm"); CHECK(scratch.run(scratchCli));
  CHECK(scratch.output.find("primary=OK detail=0 restore=I2C_TIMEOUT") != std::string::npos);
  CHECK(scratch.output.find("backup=retained restore-required=yes") != std::string::npos);
  CHECK(std::memcmp(scratch.memory, original.data(), original.size()) != 0);
  before = scratch.writes; CHECK(scratch.run(scratchCli)); CHECK(scratch.writes == before);
  scratchCli.processCommand("restore confirm"); CHECK(scratch.run(scratchCli));
  CHECK(std::memcmp(scratch.memory, original.data(), original.size()) == 0);
  // WP suppression fails verification and never becomes a claimed test pass.
  scratch.output.clear(); scratch.writeProtected = true;
  scratchCli.processCommand("xfer_demo 32 8 confirm"); CHECK(scratch.run(scratchCli));
  CHECK(scratch.output.find("primary=VERIFY_MISMATCH") != std::string::npos);
  scratch.writeProtected = false; scratchCli.processCommand("restore confirm"); CHECK(scratch.run(scratchCli));
  CHECK(std::memcmp(scratch.memory, original.data(), original.size()) == 0);
  // Direct update command distinguishes skips from physical programming.
  scratch.output.clear(); before = scratch.writes;
  scratchCli.processCommand("uverify 0 211"); CHECK(scratch.run(scratchCli));
  CHECK(scratch.writes == before); CHECK(scratch.output.find("skipped=1") != std::string::npos);
  // Stop before any backup/pattern traffic and while the automatic restore is
  // programming. Neither cancellation can initiate a compensating write.
  Fixture restoreCancel; eeprom24cxx_cli::Cli restoreCancelCli;
  restoreCancelCli.setup(restoreCancel.platform(), restoreCancel.config()); restoreCancelCli.processCommand("color off");
  std::memset(restoreCancel.memory, 0x37, sizeof(restoreCancel.memory));
  before = restoreCancel.transfers;
  restoreCancelCli.processCommand("xfer_demo 32 16 confirm"); restoreCancelCli.processCommand("cancel");
  CHECK(restoreCancel.run(restoreCancelCli)); CHECK(restoreCancel.transfers == before);
  restoreCancel.output.clear(); restoreCancelCli.processCommand("restore confirm");
  CHECK(restoreCancel.output.find("NO_RESULT") != std::string::npos);
  restoreCancel.output.clear(); restoreCancelCli.processCommand("xfer_demo 32 16 confirm");
  for (unsigned i = 0; i < 100 && restoreCancel.output.find("name=final-window result=PASS") == std::string::npos; ++i)
    CHECK(restoreCancel.run(restoreCancelCli, 1));
  CHECK(restoreCancel.output.find("name=final-window result=PASS") != std::string::npos);
  CHECK(restoreCancel.run(restoreCancelCli, 1)); // First restore page issued.
  before = restoreCancel.transfers; restoreCancel.output.clear(); restoreCancelCli.processCommand("cancel");
  CHECK(restoreCancel.transfers == before);
  CHECK(restoreCancel.output.find("primary=OK detail=0 restore=CANCELLED") != std::string::npos);
  CHECK(restoreCancel.output.find("backup=retained") != std::string::npos);
  restoreCancelCli.processCommand("restore confirm"); CHECK(restoreCancel.transfers == before);
  CHECK(restoreCancel.run(restoreCancelCli, 1)); CHECK(restoreCancel.transfers == before);
  CHECK(restoreCancel.run(restoreCancelCli, 100));
  for (const auto value : restoreCancel.memory) CHECK(value == 0x37);
  // Terminal stage evidence survives unrelated diagnostics and a later restore.
  // Keeping only the primary Status would lose ambiguous commit/count/mismatch
  // information as soon as _lastResult became a read or restoration result.
  Fixture evidence, replacement; eeprom24cxx_cli::Cli evidenceCli;
  evidenceCli.setup(evidence.platform(), evidence.config()); evidenceCli.processCommand("color off");
  evidence.failWrite = true;
  evidenceCli.processCommand("xfer_demo 32 8 confirm"); CHECK(evidence.run(evidenceCli));
  evidenceCli.processCommand("readbyte 32"); CHECK(evidence.run(evidenceCli));
  evidence.output.clear(); evidenceCli.processCommand("scratch");
  CHECK(evidence.output.find("Last primary stage (retained independently of restoration)") != std::string::npos);
  CHECK(evidence.output.find("kind=VERIFIED_WRITE state=FAILED") != std::string::npos);
  CHECK(evidence.output.find("commit=INDETERMINATE") != std::string::npos);
  // setup cannot retarget a retained backup, replace its time source, or issue
  // startup traffic to a new context. Restoration still belongs to the original.
  auto alternate = replacement.config(); alternate.i2cAddress = 0x51;
  before = evidence.transfers; evidence.output.clear();
  evidenceCli.setup(replacement.platform(), alternate);
  CHECK(evidence.output.find("BUSY") != std::string::npos && replacement.output.empty());
  CHECK(evidence.transfers == before && replacement.transfers == 0);
  evidenceCli.processCommand("restore confirm"); CHECK(evidence.run(evidenceCli));
  CHECK(replacement.transfers == 0 && evidence.addresses.back() == 0x50);
  evidence.output.clear(); evidenceCli.processCommand("scratch");
  CHECK(evidence.output.find("primary=I2C_TIMEOUT detail=0 restore=OK") != std::string::npos);
  CHECK(evidence.output.find("Last restoration stage") != std::string::npos);
  CHECK(evidence.output.find("commit=INDETERMINATE") != std::string::npos);
  CHECK(evidence.output.find("commit=VERIFIED") != std::string::npos);
  // Failure of restoration retains its own detailed effect independently of a
  // successful primary stage and of subsequent ordinary reads.
  evidence.failWriteAttempt = evidence.writes + 3;
  evidenceCli.processCommand("xfer_demo 32 8 confirm"); CHECK(evidence.run(evidenceCli));
  evidenceCli.processCommand("readbyte 32"); CHECK(evidence.run(evidenceCli));
  evidence.output.clear(); evidenceCli.processCommand("scratch");
  CHECK(evidence.output.find("primary=OK detail=0 restore=I2C_TIMEOUT") != std::string::npos);
  CHECK(evidence.output.find("kind=READ state=SUCCEEDED") != std::string::npos);
  CHECK(evidence.output.find("kind=VERIFIED_WRITE state=FAILED") != std::string::npos);
  CHECK(evidence.output.find("commit=INDETERMINATE") != std::string::npos);
  evidenceCli.processCommand("restore confirm"); CHECK(evidence.run(evidenceCli));
  // An in-flight ordinary write also owns the existing callbacks, staging
  // buffer and clock. A rejected setup must leave that transfer usable.
  evidenceCli.processCommand("writebyte 0 42"); before = evidence.transfers;
  evidence.output.clear(); evidenceCli.setup(replacement.platform(), alternate);
  CHECK(evidence.output.find("BUSY") != std::string::npos);
  CHECK(evidence.transfers == before && replacement.transfers == 0);
  CHECK(evidence.run(evidenceCli)); CHECK(evidence.memory[0] == 42 && replacement.memory[0] == 0);
  // A valid idle replacement is allowed; an invalid replacement does not alter
  // staged settings, output context, or the existing binding.
  auto invalidReplacement = alternate; invalidReplacement.maxTxBytes = 0;
  before = evidence.transfers; evidence.output.clear();
  evidenceCli.setup(replacement.platform(), invalidReplacement);
  CHECK(evidence.output.find("INVALID_CONFIG") != std::string::npos && replacement.output.empty());
  evidenceCli.processCommand("settings");
  CHECK(evidence.output.find("address=0x50") != std::string::npos && evidence.transfers == before);
  for (const char value : std::string("writebyte 0 99")) evidenceCli.feed(value);
  evidenceCli.setup(replacement.platform(), alternate);
  CHECK(replacement.probes == 1 && replacement.addresses.back() == 0x51);
  evidenceCli.feed('\n'); CHECK(replacement.run(evidenceCli));
  CHECK(replacement.writes == 0 && replacement.memory[0] == 0); // No old partial command on new target.
  replacement.output.clear(); evidenceCli.processCommand("result");
  CHECK(replacement.output.find("kind=NONE state=IDLE") != std::string::npos);
  // Cancelling or ending scheduling does not make the old context replaceable
  // until its physical write/reset settling barrier expires.
  evidenceCli.processCommand("writebyte 0 55"); evidenceCli.tick(); evidenceCli.processCommand("end");
  before = evidence.transfers; replacement.output.clear();
  evidenceCli.setup(evidence.platform(), evidence.config());
  CHECK(replacement.output.find("BUSY") != std::string::npos && evidence.transfers == before);
  CHECK(replacement.run(evidenceCli, 7));
  evidenceCli.setup(evidence.platform(), evidence.config()); CHECK(evidence.transfers == before + 1);
  evidenceCli.processCommand("iface_reset"); evidenceCli.processCommand("end");
  before = replacement.transfers; evidence.output.clear();
  evidenceCli.setup(replacement.platform(), alternate);
  CHECK(evidence.output.find("BUSY") != std::string::npos && replacement.transfers == before);
  CHECK(evidence.run(evidenceCli, 7));
  evidenceCli.setup(replacement.platform(), alternate); CHECK(replacement.transfers == before + 1);
  // A CLI session spans the whole command, survives cached diagnostics, and
  // emits one correlated terminal record only after all its physical work.
  Fixture jobs; eeprom24cxx_cli::Cli jobCli;
  auto jobConfig = jobs.config(); jobConfig.maxRxBytes = 4;
  jobCli.setup(jobs.platform(), jobConfig); jobs.output.clear();
  jobCli.processCommand("job auto 0");
  before = jobs.transfers;
  for (char value : std::string("read 0 8\n")) jobCli.feed(value);
  CHECK(jobs.output.find("CLI accepted: command=read session=1") != std::string::npos);
  CHECK(jobs.output.find("CLI result:") == std::string::npos && jobs.transfers == before);
  CHECK(jobs.output.find("> ") != std::string::npos); // Admission prompt is not completion.
  CHECK(jobs.run(jobCli, 5) && jobs.transfers == before);
  jobCli.processCommand("job step 0"); CHECK(jobs.transfers == before);
  jobCli.processCommand("settings"); jobCli.processCommand("health");
  jobCli.processCommand("hilmark sync_1");
  CHECK(jobs.output.find("CLI marker: token=sync_1") != std::string::npos);
  jobCli.processCommand("job step 1"); CHECK(jobs.transfers == before + 1);
  CHECK(jobs.output.find("CLI result:") == std::string::npos);
  jobCli.processCommand("job step"); CHECK(jobs.transfers == before + 2);
  const std::string readDone = "CLI result: command=read session=1 outcome=SUCCESS status=OK code=0 detail=0";
  CHECK(jobs.output.find(readDone) != std::string::npos);
  CHECK(jobs.output.find(readDone, jobs.output.find(readDone) + 1) == std::string::npos);
  jobs.output.clear(); jobCli.processCommand("job result");
  CHECK(jobs.output.find(readDone) != std::string::npos && jobs.transfers == before + 2);
  jobCli.processCommand("read 8 8"); jobCli.processCommand("job result");
  CHECK(jobs.output.find("CLI accepted: command=read session=2") != std::string::npos);
  jobCli.processCommand("job cancel");
  CHECK(jobs.output.find("CLI result: command=read session=2 outcome=CANCELLED status=CANCELLED code=19") != std::string::npos);
  CHECK(jobs.transfers == before + 2);
  jobs.output.clear(); jobCli.processCommand("read 8 8"); jobCli.processCommand("job timeout");
  CHECK(jobs.output.find("CLI result: command=read session=3 outcome=TIMED_OUT status=TIMEOUT code=4") != std::string::npos);
  CHECK(jobs.transfers == before + 2);
  jobs.output.clear();
  for (const char* command : {"job auto 2", "job step 2", "job step -1", "job cancel extra", "job result extra",
                              "hilmark", "hilmark a.b", "hilmark token extra", "hilmark 123456789012345678901234567890123"}) {
    jobCli.processCommand(command);
  }
  CHECK(jobs.output.find("CLI marker:") == std::string::npos && jobs.transfers == before + 2);
  jobCli.processCommand("job auto 1"); jobs.output.clear();
  jobs.failRead = true; jobCli.processCommand("stress 3"); CHECK(jobs.run(jobCli));
  CHECK(jobs.output.find("Stress stopped: ok=2 fail=1") != std::string::npos);
  CHECK(jobs.output.find("CLI result: command=stress session=4 outcome=FAILED") != std::string::npos);
  jobs.output.clear(); jobCli.processCommand("discover"); CHECK(jobs.run(jobCli, 8));
  CHECK(jobs.output.find("Scan complete:") != std::string::npos);
  CHECK(jobs.output.find("CLI result: command=discover session=5 outcome=SUCCESS") != std::string::npos);
  jobs.output.clear(); jobCli.processCommand("discover"); jobCli.processCommand("stop");
  CHECK(jobs.output.find("CLI result: command=discover session=6 outcome=CANCELLED") != std::string::npos);
  CHECK(jobs.output.find("Scan complete:") == std::string::npos);
  jobCli.processCommand("job auto 0"); jobs.output.clear();
  jobCli.processCommand("writebyte 0 77"); jobCli.processCommand("job step");
  const unsigned timedWrites = jobs.writes; before = jobs.transfers;
  CHECK(jobs.memory[0] == 77);
  jobCli.processCommand("job timeout"); CHECK(jobs.transfers == before);
  CHECK(jobs.output.find("CLI result: command=writebyte session=7 outcome=TIMED_OUT") != std::string::npos);
  jobCli.processCommand("readbyte 0"); CHECK(jobs.transfers == before);
  CHECK(jobs.output.find("BUSY") != std::string::npos);
  CHECK(jobs.run(jobCli, 8)); CHECK(jobs.writes == timedWrites);
  jobs.output.clear(); jobCli.processCommand("readbyte 0"); jobCli.processCommand("job step");
  CHECK(jobs.output.find("CLI result: command=readbyte session=8 outcome=SUCCESS") != std::string::npos);
  // One logical range may use several independent 256-byte core jobs.
  Fixture streamedJobs; streamedJobs.geometry = geometryFor(DeviceVariant::C16);
  eeprom24cxx_cli::Cli streamedJobCli; auto streamedConfig = streamedJobs.config();
  streamedConfig.variant = DeviceVariant::C16;
  streamedJobCli.setup(streamedJobs.platform(), streamedConfig); streamedJobs.output.clear();
  streamedJobCli.processCommand("crc 0 512"); CHECK(streamedJobs.run(streamedJobCli));
  const std::string crcDone = "CLI result: command=crc session=1 outcome=SUCCESS";
  CHECK(streamedJobs.output.find(crcDone) != std::string::npos);
  CHECK(streamedJobs.output.find(crcDone, streamedJobs.output.find(crcDone) + 1) == std::string::npos);
  // Runtime protocol controls stay staged until bind and cannot grow beyond
  // the injected adapter capacity. Timed waiting performs no ACK polls.
  Fixture protocol; eeprom24cxx_cli::Cli protocolCli;
  auto protocolPlatform = protocol.platform();
  protocolPlatform.sdaPin = 8; protocolPlatform.sclPin = 9;
  protocolPlatform.frequencyHz = 100000; protocolPlatform.writeProtectPin = 4;
  protocolCli.setup(protocolPlatform, protocol.config()); protocol.output.clear();
  before = protocol.transfers;
  for (const char* command : {"bus", "limits", "writecycle", "ackpoll", "deadline"}) protocolCli.processCommand(command);
  CHECK(protocol.output.find("Bus config: sda=8 scl=9 frequency-hz=100000 wp-pin=4") != std::string::npos);
  CHECK(protocol.transfers == before);
  for (const char* command : {"limits 5 3", "writecycle 10", "ackpoll 0"}) protocolCli.processCommand(command);
  CHECK(protocol.output.find("BUSY") != std::string::npos);
  CHECK(protocol.transfers == before);
  protocolCli.processCommand("end"); protocol.output.clear();
  for (const char* command : {"limits 18 16", "limits 17 17", "limits 1 1", "writecycle 4", "writecycle 1001", "ackpoll 2"})
    protocolCli.processCommand(command);
  protocolCli.processCommand("settings");
  CHECK(protocol.output.find("timeout=50 ms tx=17 rx=16") != std::string::npos);
  CHECK(protocol.output.find("ack-polling=yes write-cycle-override=0") != std::string::npos);
  protocolCli.processCommand("limits 5 3"); protocolCli.processCommand("writecycle 10");
  protocolCli.processCommand("ackpoll 0"); protocolCli.processCommand("bind");
  CHECK(protocol.transfers == before);
  const unsigned probesBeforeTimed = protocol.probes;
  protocolCli.processCommand("fillverify 0 170 9"); CHECK(protocol.run(protocolCli, 100));
  CHECK(protocol.writes == 3 && protocol.probes == probesBeforeTimed);
  for (unsigned index = 0; index < 9; ++index) CHECK(protocol.memory[index] == 170);
  CHECK(protocol.output.find("CLI result: command=fillverify session=1 outcome=SUCCESS") != std::string::npos);
  protocolCli.processCommand("end"); protocolCli.processCommand("ackpoll 1");
  protocolCli.processCommand("limits 17 16"); protocolCli.processCommand("writecycle 0");
  protocolCli.processCommand("begin"); CHECK(protocol.probes == probesBeforeTimed + 1);
  Fixture noProbe; eeprom24cxx_cli::Cli noProbeCli;
  auto noProbeConfig = noProbe.config(); noProbeConfig.i2cProbe = nullptr;
  noProbeCli.setup(noProbe.platform(), noProbeConfig); noProbeCli.processCommand("end"); noProbe.output.clear();
  before = noProbe.transfers; noProbeCli.processCommand("ackpoll 1");
  CHECK(noProbe.output.find("UNSUPPORTED") != std::string::npos && noProbe.transfers == before);
  // A whole-job deadline expires even while owner polling is paused, across
  // uint32 clock wrap, and after a successful callback that returned too late.
  Fixture deadlines; eeprom24cxx_cli::Cli deadlineCli;
  deadlineCli.setup(deadlines.platform(), deadlines.config());
  deadlineCli.processCommand("job auto 0"); deadlineCli.processCommand("deadline 5");
  before = deadlines.transfers; deadlines.output.clear();
  deadlineCli.processCommand("readbyte 0"); deadlineCli.processCommand("deadline 9");
  CHECK(deadlines.output.find("BUSY") != std::string::npos);
  deadlines.ms += 5; deadlineCli.tick();
  CHECK(deadlines.transfers == before);
  CHECK(deadlines.output.find("CLI result: command=readbyte session=1 outcome=TIMED_OUT") != std::string::npos);
  deadlines.output.clear(); deadlines.ms = UINT32_MAX - 2U;
  deadlineCli.processCommand("readbyte 0"); deadlines.ms += 3U; deadlineCli.tick();
  CHECK(deadlines.output.find("CLI result:") == std::string::npos);
  deadlines.ms += 2U; deadlineCli.tick();
  CHECK(deadlines.transfers == before);
  CHECK(deadlines.output.find("CLI result: command=readbyte session=2 outcome=TIMED_OUT") != std::string::npos);
  deadlines.output.clear(); deadlineCli.processCommand("deadline 2147483648");
  CHECK(deadlines.output.find("INVALID_PARAM") != std::string::npos);
  Fixture late; eeprom24cxx_cli::Cli lateCli; auto lateConfig = late.config();
  lateConfig.i2cWriteRead = [](uint8_t address, const uint8_t* tx, size_t txLength, uint8_t* rx,
                             size_t rxLength, uint32_t timeout, void* user) {
    const auto result = Fixture::read(address, tx, txLength, rx, rxLength, timeout, user);
    static_cast<Fixture*>(user)->ms += 5U;
    return result;
  };
  lateCli.setup(late.platform(), lateConfig); lateCli.processCommand("deadline 5"); late.output.clear();
  before = late.transfers; lateCli.processCommand("readbyte 0"); lateCli.tick();
  CHECK(late.transfers == before + 1);
  CHECK(late.output.find("kind=READ state=SUCCEEDED") != std::string::npos); // Preserve physical evidence.
  CHECK(late.output.find("CLI result: command=readbyte session=1 outcome=TIMED_OUT") != std::string::npos);
  CHECK(late.output.find("CLI result: command=readbyte session=1 outcome=SUCCESS") == std::string::npos);
  late.output.clear(); lateCli.processCommand("rw_suite 0 8 confirm"); lateCli.tick();
  CHECK(late.writes == 0); // Deadline after backup cannot start a pattern or restoration.
  CHECK(late.output.find("CLI result: command=rw_suite session=2 outcome=TIMED_OUT") != std::string::npos);
  std::puts("[PASS] CLI parsing, memory views, owner polling, correlated sessions, scratch programming, fault/restore and cancellation");
  return 0;
}
