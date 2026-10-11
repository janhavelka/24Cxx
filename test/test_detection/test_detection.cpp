#include "../support/CliFixture.h"

#define CHECK(x) do { if (!(x)) { std::printf("[FAIL] detection check line %d: %s\n", __LINE__, #x); return 1; } } while (false)

namespace {
struct DetectionFixture : Fixture {
  Status responses[128]{};
  DetectionFixture() {
    for (auto& response : responses) response = Status::Error(Err::I2C_NACK_ADDR, "absent", 2);
  }
  static Status detect(uint8_t bus, void* user) {
    auto& fixture = static_cast<DetectionFixture&>(*static_cast<Fixture*>(user));
    ++fixture.transfers; ++fixture.probes; fixture.addresses.push_back(bus);
    const auto response = fixture.responses[bus];
    fixture.counters.record(response.ok(), BusTransferKind::PROBE);
    return response;
  }
  eeprom24cxx_cli::Platform platform() {
    auto value = Fixture::platform(); value.probeAddress = detect; return value;
  }
};
bool contains(const Fixture& fixture, const char* value) { return fixture.output.find(value) != std::string::npos; }
std::string cachedState(eeprom24cxx_cli::Cli& cli, Fixture& fixture) {
  fixture.output.clear(); cli.processCommand("settings"); cli.processCommand("health");
  // Physical callback counters intentionally include these probes; tracked
  // driver health, geometry and current-address ownership must stay unchanged.
  return fixture.output.substr(0, fixture.output.find("Bus: attempts="));
}
std::string modelLine(const Fixture& fixture, const char* model) {
  const auto start = fixture.output.find(std::string("  ") + model + " ");
  return start == std::string::npos ? "" : fixture.output.substr(start, fixture.output.find('\n', start) - start);
}
}

int main() {
  // Discovery reports every physical failure, distinguishes absent addresses,
  // and counts predicted banks without guessing which model is attached.
  DetectionFixture banked;
  banked.responses[0x50] = banked.responses[0x54] = Status::Ok();
  banked.responses[0x51] = Status::Error(Err::I2C_TIMEOUT, "controller timeout", 17);
  auto config = banked.config(); config.variant = DeviceVariant::MICROCHIP_24LC1025;
  banked.geometry = geometryFor(config.variant);
  eeprom24cxx_cli::Cli bankedCli; bankedCli.setup(banked.platform(), config); bankedCli.processCommand("color off");
  const auto beforeState = cachedState(bankedCli, banked);
  const unsigned before = banked.transfers;
  banked.output.clear(); banked.addresses.clear(); bankedCli.processCommand("discover");
  CHECK(banked.transfers == before);
  CHECK(contains(banked, "Detection: range=0x50..0x57 model=24lc1025 base=0x50 identity=unverified"));
  CHECK(contains(banked, "Configured banks: base-valid=yes bank-mask=0x04 addresses=0x50,0x54"));
  CHECK(banked.run(bankedCli, 8));
  CHECK(banked.transfers == before + 8);
  CHECK(banked.addresses.size() == 8);
  for (unsigned index = 0; index < 8; ++index) CHECK(banked.addresses[index] == 0x50U + index);
  CHECK(contains(banked, "  Found 0x50 (EEPROM/bank candidate; identity unverified)"));
  CHECK(contains(banked, "Scan error: address=0x51 status=I2C_TIMEOUT detail=17"));
  CHECK(contains(banked, "Scan complete: 2 ACK, 1 other transport errors."));
  CHECK(contains(banked, "tested=8 ack=2 absent=5 errors=1 candidate-acks=2 configured-bank-acks=2/2 base-ack=yes"));
  CHECK(contains(banked, "outcome=FAILED status=I2C_TIMEOUT code=14 detail=17"));
  CHECK(cachedState(bankedCli, banked) == beforeState);
  CHECK(banked.writes == 0 && banked.reads == 0);

  // Full scans omit reserved addresses and keep EEPROM candidates distinct
  // from unrelated responding peripherals. One callback per owner tick.
  DetectionFixture wholeBus;
  wholeBus.responses[0x3C] = wholeBus.responses[0x50] = wholeBus.responses[0x57] = Status::Ok();
  eeprom24cxx_cli::Cli wholeCli; wholeCli.setup(wholeBus.platform(), wholeBus.config());
  wholeCli.processCommand("color off"); wholeBus.output.clear(); wholeBus.addresses.clear();
  wholeCli.processCommand("scan"); CHECK(wholeBus.run(wholeCli, 112));
  CHECK(wholeBus.addresses.size() == 112);
  for (unsigned index = 0; index < 112; ++index) CHECK(wholeBus.addresses[index] == 0x08U + index);
  CHECK(contains(wholeBus, "range=0x08..0x77"));
  CHECK(contains(wholeBus, "tested=112 ack=3 absent=109 errors=0 candidate-acks=2 configured-bank-acks=1/1 base-ack=yes"));
  CHECK(contains(wholeBus, "outcome=SUCCESS status=OK"));

  // The sibling alias resolves to a canonical session. An error at the base
  // must never be reported as an observed absence; later ACKs cannot erase it.
  DetectionFixture missing;
  missing.responses[0x50] = Status::Error(Err::I2C_BUS, "arbitration loss", 19);
  missing.responses[0x51] = Status::Error(Err::I2C_TIMEOUT, "later failure", 20);
  eeprom24cxx_cli::Cli missingCli; missingCli.setup(missing.platform(), missing.config());
  missingCli.processCommand("color off"); missingCli.processCommand("verbose on"); missing.output.clear();
  missingCli.processCommand("detect"); CHECK(missing.run(missingCli, 8));
  CHECK(contains(missing, "CLI accepted: command=discover"));
  CHECK(contains(missing, "tested=8 ack=0 absent=6 errors=2 candidate-acks=0 configured-bank-acks=0/1 base-ack=error"));
  CHECK(contains(missing, "no EEPROM address ACKs observed"));
  CHECK(contains(missing, "presence unresolved"));
  CHECK(contains(missing, "Scan absent: address=0x52 status=I2C_NACK_ADDR detail=2"));
  CHECK(contains(missing, "outcome=FAILED status=I2C_BUS code=15 detail=19"));
  missing.responses[0x50] = missing.responses[0x51] = Status::Error(Err::I2C_NACK_ADDR, "absent");
  missing.output.clear(); missingCli.processCommand("discover"); CHECK(missing.run(missingCli, 8));
  CHECK(contains(missing, "tested=8 ack=0 absent=8 errors=0 candidate-acks=0 configured-bank-acks=0/1 base-ack=no"));
  CHECK(contains(missing, "outcome=SUCCESS status=OK"));

  // Targeted address-only probes preserve bound geometry, health and pointer
  // ownership. They are useful before bind and do not depend on ACK polling.
  DetectionFixture targeted; targeted.responses[0x57] = Status::Ok();
  targeted.memory[7] = 0xA5;
  eeprom24cxx_cli::Cli targetedCli; targetedCli.setup(targeted.platform(), targeted.config());
  targetedCli.processCommand("color off"); targetedCli.processCommand("read 6 1"); CHECK(targeted.run(targetedCli));
  CHECK(targeted.pointer == 7);
  const auto seeded = cachedState(targetedCli, targeted);
  const unsigned seededTransfers = targeted.transfers;
  targeted.output.clear(); targetedCli.processCommand("probe 0x57");
  CHECK(targeted.transfers == seededTransfers + 1 && targeted.pointer == 7);
  CHECK(contains(targeted, "Presence: address=0x57 mode=address-only status=OK detail=0 identity=unverified"));
  CHECK(cachedState(targetedCli, targeted) == seeded);
  targeted.output.clear(); targetedCli.processCommand("current 1"); CHECK(targeted.run(targetedCli));
  CHECK(contains(targeted, "A5")); CHECK(targeted.pointer == 8);
  targeted.output.clear(); targetedCli.processCommand("probe");
  CHECK(contains(targeted, "Presence: address=0x50 mode=driver status=OK"));
  targetedCli.processCommand("end"); targetedCli.processCommand("ackpoll 0");
  const auto unboundState = cachedState(targetedCli, targeted);
  targeted.output.clear(); targetedCli.processCommand("probe 0x57");
  CHECK(contains(targeted, "mode=address-only status=OK"));
  CHECK(cachedState(targetedCli, targeted) == unboundState);
  targetedCli.processCommand("bind");
  const unsigned addressedReads = targeted.reads;
  targeted.output.clear(); targetedCli.processCommand("probe");
  CHECK(targeted.reads == addressedReads + 1); // Driver fallback remains supported.
  CHECK(contains(targeted, "mode=driver status=OK"));
  const unsigned invalidBefore = targeted.transfers;
  for (const char* command : {"probe 0x4F", "probe 0x58", "probe -1", "probe 0x50junk", "probe 0x50 extra"}) {
    targeted.output.clear(); targetedCli.processCommand(command);
    CHECK(contains(targeted, "INVALID_PARAM")); CHECK(targeted.transfers == invalidBefore);
  }
  targetedCli.processCommand("read 0 8"); targeted.output.clear(); targetedCli.processCommand("probe 0x57");
  CHECK(contains(targeted, "BUSY")); CHECK(targeted.transfers == invalidBefore);
  targetedCli.processCommand("stop");

  DetectionFixture noAdapter; auto platform = noAdapter.platform(); platform.probeAddress = nullptr;
  eeprom24cxx_cli::Cli noAdapterCli; noAdapterCli.setup(platform, noAdapter.config());
  const unsigned noAdapterBefore = noAdapter.transfers;
  noAdapter.output.clear(); noAdapterCli.processCommand("probe 0x50");
  CHECK(contains(noAdapter, "UNSUPPORTED")); CHECK(noAdapter.transfers == noAdapterBefore);

  // Cancelling a physical write does not let either scan path bypass tWR,
  // even after end. Passive owner time must release the barrier first.
  DetectionFixture settling;
  eeprom24cxx_cli::Cli settlingCli; settlingCli.setup(settling.platform(), settling.config());
  settlingCli.processCommand("fill 0 16 0x11"); settlingCli.tick(); CHECK(settling.writes == 1);
  settlingCli.processCommand("end");
  const unsigned barrierTransfers = settling.transfers;
  for (const char* command : {"probe 0x50", "probe", "discover", "scan"}) {
    settling.output.clear(); settlingCli.processCommand(command);
    CHECK(contains(settling, "BUSY")); CHECK(settling.transfers == barrierTransfers);
  }
  CHECK(settling.run(settlingCli, 6));
  settling.output.clear(); settlingCli.processCommand("probe 0x50");
  CHECK(contains(settling, "BUSY")); CHECK(settling.transfers == barrierTransfers);
  settlingCli.tick(); // Release the cached barrier at tWR plus clock margin.
  settling.output.clear(); settlingCli.processCommand("probe 0x50");
  CHECK(settling.transfers == barrierTransfers + 1);
  CHECK(contains(settling, "mode=address-only status=I2C_NACK_ADDR"));

  // Profiles and memory-to-bus mapping are cached diagnostics. Exercise both
  // pointer widths, non-adjacent bank bits and invalid staged bank addresses.
  DetectionFixture mapping;
  eeprom24cxx_cli::Cli mappingCli; mappingCli.setup(mapping.platform(), mapping.config());
  mapping.output.clear(); const unsigned mappingBefore = mapping.transfers;
  mappingCli.processCommand("variants");
  CHECK(modelLine(mapping, "24c02").find("bases=0x50,0x51,0x52,0x53,0x54,0x55,0x56,0x57") != std::string::npos);
  CHECK(modelLine(mapping, "24c04").find("bases=0x50,0x52,0x54,0x56") != std::string::npos);
  CHECK(modelLine(mapping, "24c08").find("bases=0x50,0x54") != std::string::npos);
  const auto c16 = modelLine(mapping, "24c16");
  CHECK(c16.size() >= 10 && c16.find("bases=0x50") == c16.size() - 10);
  CHECK(modelLine(mapping, "24lc1025").find("bases=0x50,0x51,0x52,0x53") != std::string::npos);
  CHECK(modelLine(mapping, "m24m01").find("bases=0x50,0x52,0x54,0x56") != std::string::npos);
  mapping.output.clear(); mappingCli.processCommand("page 0xFE");
  CHECK(contains(mapping, "remaining=2 bank-remaining=2 sla=0x50 pointer=0xFE pointer-bytes=1"));
  mappingCli.processCommand("end"); mappingCli.processCommand("model 24c08");
  mapping.output.clear(); mappingCli.processCommand("page 0x100");
  CHECK(contains(mapping, "sla=0x51 pointer=0x00 pointer-bytes=1"));
  mappingCli.processCommand("model 24lc1025"); mapping.output.clear(); mappingCli.processCommand("page 0x10003");
  CHECK(contains(mapping, "sla=0x54 pointer=0x0003 pointer-bytes=2"));
  mappingCli.processCommand("model 24c16"); mappingCli.processCommand("addr 0x57");
  mapping.output.clear(); mappingCli.processCommand("page 0");
  CHECK(contains(mapping, "INVALID_CONFIG")); CHECK(!contains(mapping, "sla="));
  CHECK(mapping.transfers == mappingBefore);
  mapping.output.clear(); mappingCli.processCommand("discover"); CHECK(mapping.run(mappingCli, 8));
  CHECK(contains(mapping, "Configured banks: base-valid=no bank-mask=0x07 addresses=none"));
  CHECK(contains(mapping, "configured-bank-acks=0/0 base-ack=no"));

  // A cancelled scan has a terminal cancellation marker, no fabricated full
  // summary, no extra transfers on later ticks and no automatic selection.
  DetectionFixture cancelled;
  eeprom24cxx_cli::Cli cancelledCli; cancelledCli.setup(cancelled.platform(), cancelled.config());
  cancelled.output.clear(); cancelled.addresses.clear(); cancelledCli.processCommand("detect");
  CHECK(cancelled.run(cancelledCli, 3)); cancelledCli.processCommand("stop"); CHECK(cancelled.run(cancelledCli, 120));
  CHECK(cancelled.addresses.size() == 3);
  CHECK(contains(cancelled, "outcome=CANCELLED")); CHECK(!contains(cancelled, "Detection summary:"));
  CHECK(!contains(cancelled, "Scan complete:"));
  std::puts("[PASS] detection diagnostics");
  return 0;
}
