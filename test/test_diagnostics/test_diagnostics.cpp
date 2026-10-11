#include "../support/CliFixture.h"

#define CHECK(x) do { if (!(x)) { std::printf("[FAIL] diagnostic check line %d: %s\n", __LINE__, #x); return 1; } } while (false)

namespace {
void seed(Fixture& fixture) {
  for (size_t index = 0; index < sizeof(fixture.memory); ++index)
    fixture.memory[index] = static_cast<uint8_t>((index * 37U) ^ 0x91U);
}
bool contains(const Fixture& fixture, const char* text) { return fixture.output.find(text) != std::string::npos; }
}

int main() {
  // Exercise every primary operation and restoration while enforcing the
  // shared owner's one-physical-transaction-per-tick budget.
  for (const auto* command : {"rw_suite 6 40 confirm", "xfer_demo 6 40 confirm", "stress_mix 6 40 2 confirm",
                               "randbench 6 40 12 confirm", "typed_demo 6 confirm"}) {
    Fixture fixture; seed(fixture);
    const std::vector<uint8_t> original(fixture.memory, fixture.memory + sizeof(fixture.memory));
    eeprom24cxx_cli::Cli cli;
    cli.setup(fixture.platform(), fixture.config()); cli.processCommand("color off"); fixture.output.clear();
    const auto before = fixture.transfers;
    cli.processCommand(command);
    CHECK(fixture.transfers == before);
    CHECK(fixture.run(cli, 3000));
    CHECK(contains(fixture, "primary=OK detail=0 restore=OK detail=0"));
    CHECK(contains(fixture, "Original scratch bytes observed by readback after restoration."));
    CHECK(!contains(fixture, "result=FAIL"));
    CHECK(std::memcmp(fixture.memory, original.data(), original.size()) == 0);
    CHECK(contains(fixture, "name=out-of-range-no-transfer result=PASS"));
    CHECK(contains(fixture, "name=final-window result=PASS"));
    if (std::strstr(command, "rw_suite")) {
      CHECK(contains(fixture, "name=changed-update result=PASS"));
      CHECK(contains(fixture, "name=unchanged-update result=PASS"));
      CHECK(contains(fixture, "compared=40 skipped=40"));
      CHECK(contains(fixture, "reads=4 writes=1 fills=2 updates=2 verifies=1 probes=0 current=0"));
    } else if (std::strstr(command, "xfer_demo")) {
      CHECK(contains(fixture, "name=zero-budget result=PASS"));
      CHECK(contains(fixture, "name=busy-admission result=PASS"));
      CHECK(contains(fixture, "name=stale-owner result=PASS"));
      CHECK(contains(fixture, "name=retained-owner result=PASS"));
      CHECK(contains(fixture, "name=exactly-once-result result=PASS"));
    } else if (std::strstr(command, "stress_mix")) {
      CHECK(contains(fixture, "reads=6 writes=2 fills=2 updates=4 verifies=2 probes=2 current=2"));
    } else if (std::strstr(command, "randbench")) {
      CHECK(contains(fixture, "name=random-read result=PASS"));
      CHECK(contains(fixture, "reads=14 writes=12 fills=0 updates=0 verifies=0 probes=0 current=0"));
    } else {
      CHECK(contains(fixture, "name=typed-le-values result=PASS"));
      CHECK(contains(fixture, "name=typed-be-signed-values result=PASS"));
      CHECK(contains(fixture, "int32=-1234567"));
      CHECK(contains(fixture, "float32=1.25 float64=-42.50 bool=true"));
      CHECK(contains(fixture, "uint32le=0xCAFEBABE bool=false uint8=0x81"));
    }
  }

  // A verified byte write alone misses a damaged neighbor. The independent
  // shadow check must fail before restoration can overwrite that evidence.
  Fixture collateral; seed(collateral);
  const std::vector<uint8_t> original(collateral.memory, collateral.memory + sizeof(collateral.memory));
  eeprom24cxx_cli::Cli collateralCli;
  collateralCli.setup(collateral.platform(), collateral.config()); collateralCli.processCommand("color off");
  uint32_t random = 0x24C02B01U;
  random ^= random << 13U; random ^= random >> 17U; random ^= random << 5U;
  collateral.corruptWriteAttempt = 1;
  collateral.corruptWriteAddress = 32 + ((random % 16 + 1) % 16);
  collateral.output.clear(); collateralCli.processCommand("randbench 32 16 1 confirm");
  CHECK(collateral.run(collateralCli, 300));
  CHECK(contains(collateral, "name=random-write result=PASS"));
  CHECK(contains(collateral, "result=FAIL"));
  CHECK(contains(collateral, "backup=retained restore-required=yes primary=VERIFY_MISMATCH"));
  CHECK(collateral.writes == 1);
  CHECK(std::memcmp(collateral.memory, original.data(), original.size()) != 0);
  collateralCli.processCommand("restore confirm"); CHECK(collateral.run(collateralCli, 300));
  CHECK(std::memcmp(collateral.memory, original.data(), original.size()) == 0);
  CHECK(contains(collateral, "primary=VERIFY_MISMATCH detail=0 restore=OK"));

  // Capability-limited adapters and a one-byte scratch range keep a real
  // addressed-read fallback; they must not read outside the retained window.
  Fixture unsupported; seed(unsupported); auto config = unsupported.config();
  config.supportsCurrentAddressRead = false;
  eeprom24cxx_cli::Cli unsupportedCli; unsupportedCli.setup(unsupported.platform(), config);
  unsupportedCli.processCommand("color off"); unsupported.output.clear();
  unsupportedCli.processCommand("stress_mix 255 1 1 confirm");
  CHECK(unsupported.run(unsupportedCli, 300));
  CHECK(contains(unsupported, "primary=OK detail=0 restore=OK"));
  CHECK(contains(unsupported, "current=0 current-skipped=1"));
  CHECK(contains(unsupported, "name=current-fallback-read result=PASS"));

  // Optional telemetry cannot be replaced with invented zero-transfer evidence.
  Fixture unmeasured; seed(unmeasured); auto platform = unmeasured.platform();
  platform.transferStats = nullptr;
  eeprom24cxx_cli::Cli unmeasuredCli; unmeasuredCli.setup(platform, unmeasured.config());
  unmeasuredCli.processCommand("color off"); unmeasured.output.clear();
  const auto beforeUnmeasured = unmeasured.transfers;
  unmeasuredCli.processCommand("xfer_demo 32 16 confirm");
  CHECK(contains(unmeasured, "UNSUPPORTED") && unmeasured.transfers == beforeUnmeasured);
  unmeasured.output.clear(); unmeasuredCli.processCommand("rw_suite 32 16 confirm");
  CHECK(unmeasured.run(unmeasuredCli, 1000));
  CHECK(contains(unmeasured, "name=out-of-range-rejected result=PASS"));
  CHECK(!contains(unmeasured, "no-transfer"));
  CHECK(contains(unmeasured, "primary=OK detail=0 restore=OK"));

  // A cancelled ordinary page write retains tWR. Scratch guard tests must
  // reject BUSY without reporting false boundary failures or touching the bus.
  Fixture settling; eeprom24cxx_cli::Cli settlingCli;
  settlingCli.setup(settling.platform(), settling.config()); settlingCli.processCommand("color off");
  settlingCli.processCommand("fill 0 0xA5 16"); CHECK(settling.run(settlingCli, 1));
  settlingCli.processCommand("stop"); settling.output.clear();
  const unsigned transfers = settling.transfers;
  settlingCli.processCommand("rw_suite 32 16 confirm");
  CHECK(settling.transfers == transfers && contains(settling, "BUSY"));
  CHECK(!contains(settling, "VERIFY_MISMATCH"));
  CHECK(!contains(settling, "Scratch check:"));

  std::puts("[PASS] scratch operation coverage, transfer contracts, typed codecs, collateral corruption and restoration");
  return 0;
}
