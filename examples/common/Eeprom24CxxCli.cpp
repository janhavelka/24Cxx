#include "Eeprom24CxxCli.h"
#include "EEPROM24Cxx/MemoryHelpers.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace eeprom24cxx_cli {
namespace {
using namespace EEPROM24Cxx;
bool equals(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
bool integer(const char* text, uint32_t low, uint32_t high, uint32_t& out) {
  if (!text || !*text || *text == '-' || *text == '+') return false;
  errno = 0;
  char* end = nullptr;
  const bool hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
  const unsigned long value = std::strtoul(text, &end, hex ? 16 : 10);
  if (errno || end == text || *end || value < low || value > high) return false;
  out = static_cast<uint32_t>(value);
  return true;
}
struct Model { const char* name; DeviceVariant variant; };
constexpr Model models[] = {
  {"zetta", DeviceVariant::ZETTA_ZD24C02B}, {"24c01", DeviceVariant::C01},
  {"24c02", DeviceVariant::C02}, {"24c04", DeviceVariant::C04},
  {"24c08", DeviceVariant::C08}, {"24c16", DeviceVariant::C16},
  {"24c32", DeviceVariant::C32}, {"24c64", DeviceVariant::C64},
  {"24c128", DeviceVariant::C128}, {"24c256", DeviceVariant::C256},
  {"24c512", DeviceVariant::C512}, {"24lc1025", DeviceVariant::MICROCHIP_24LC1025},
  {"m24m01", DeviceVariant::ST_M24M01}
};
const char* modelName(DeviceVariant variant) {
  for (const auto& model : models) if (model.variant == variant) return model.name;
  return "custom";
}
}  // namespace

const char* Cli::color(unsigned code) const {
  if (!_color) return "";
  switch (code) {
    case 31: return "\033[31m";
    case 32: return "\033[32m";
    case 33: return "\033[33m";
    case 36: return "\033[36m";
    case 90: return "\033[90m";
    default: return "\033[0m";
  }
}
void Cli::print(const char* format, ...) {
  if (!_platform.vprintf) return;
  va_list args;
  va_start(args, format);
  _platform.vprintf(_platform.user, format, args);
  va_end(args);
}
uint32_t Cli::now() const {
  return _platform.nowMs ? _platform.nowMs(_platform.user) :
      _config.nowMs ? _config.nowMs(_config.timeUser) : 0;
}
void Cli::status(EEPROM24Cxx::Status value) {
  const unsigned shade = value.ok() ? 32U : value.inProgress() ? 36U : 31U;
  print("%s[%s]%s %s detail=%ld%s%s\n", color(shade), value.ok() || value.inProgress() ? "I" : "E",
        color(0), EEPROM24Cxx::errorName(value.code), static_cast<long>(value.detail),
        value.msg && *value.msg ? ": " : "", value.msg ? value.msg : "");
}
void Cli::setup(const Platform& platform, const EEPROM24Cxx::Config& config) {
  _platform = platform;
  _config = config;
  printVersion();
  print("Diagnostic CLI; application owns the bus. Type 'help' for commands.\n");
  print("EEPROM has no standard identity register: select the exact geometry before writing.\n");
  status(_device.begin(_config));
  print("Startup is read-only. ACK and write completion do not prove stored contents.\n");
  printHealth();
  printHelp();
  printPrompt();
}
void Cli::printPrompt() { print("> "); }
void Cli::printVersion() {
  print("%sEEPROM24Cxx diagnostic CLI%s | %s %s | %s | built %s %s\n", color(36), color(0),
        _platform.framework, _platform.frameworkVersion, _platform.target, __DATE__, __TIME__);
#ifdef EEPROM24CXX_VERSION_STRING
  print("Library: %s\n", EEPROM24CXX_VERSION_FULL);
#endif
}
void Cli::printHelp() {
  print("%s=== EEPROM24Cxx CLI Help ===%s\n", color(36), color(0));
  const auto section = [this](const char* title) { print("\n%s[%s]%s\n", color(32), title, color(0)); };
  const auto item = [this](const char* command, const char* description) {
    print("  %s%-32s%s - %s\n", color(36), command, color(0), description);
  };
  section("Common");
  item("help / ?", "Show this help");
  item("version / ver", "Firmware, library and framework version");
  item("scan / discover", "Probe 0x08..0x77 / 0x50..0x57, one address per tick");
  item("init / begin [addr]", "Bind optional address and check presence; no EEPROM write");
  item("bind / unbind / end", "Bus-silent binding or release; end cancels scheduling");
  item("addr [0x50..0x57]", "Set strap base while ended; model restricts bank bits");
  item("model [name]", "List/select geometry while ended; never auto-detected");
  item("color [0|1|off|on]", "Show or toggle ANSI colors");
  item("verbose [0|1|off|on]", "Show or toggle per-round stress output");
  section("Memory");
  item("read / dump / hexdump <addr> [N]", "Bounded hex+ASCII view; default 16, up to capacity");
  item("readbyte <addr>", "Read one byte");
  item("current / cur [N]", "Current-pointer read, default 1; requires known pointer");
  item("text <addr> [N]", "Escaped ASCII view; default 64, up to capacity");
  item("strings [addr N [minLen]]", "Printable strings; whole chip by default, minLen=4 (1..64)");
  item("crc <addr> <N>", "Compute CRC32/ISO-HDLC over a read-only range");
  item("write <addr> <byte...>", "Explicitly overwrite 1..32 bytes; no readback");
  item("writebyte <addr> <byte>", "Explicitly overwrite one byte; no readback");
  item("wverify <addr> <byte...>", "Explicit write with readback verification");
  item("update / uverify <addr> <bytes>", "Read first; skip unchanged pages, optional verification");
  item("fill <addr> <byte> <N>", "Explicitly overwrite a range; no readback");
  item("fillverify <addr> <byte> <N>", "Explicit fill with readback verification");
  item("verify <addr> <byte...>", "Read-only comparison against 1..32 expected bytes");
  item("progress / status / job / result", "Cached operation progress and commit evidence");
  item("stop / cancel", "Cancel future work; an issued EEPROM write continues");
  section("Configuration");
  item("cfg / settings / snapshot", "Cached staged settings and geometry; no I2C");
  item("timeout [1..1000]", "Set transaction timeout while ended");
  item("offline [0..255]", "Set passive offline threshold while ended; 0 disables");
  item("variants / size", "List profile geometry / show configured capacity");
  item("geometry / timing / page [addr]", "Cached geometry/timing or page and bank boundaries");
  section("Diagnostics");
  item("drv / health / state / online", "Cached health and independent physical bus counters");
  item("diag", "Version, staged settings and cached health");
  item("probe", "Presence check; bypasses tracked driver health");
  item("recover", "One explicit presence check; no writes or bus reset");
  item("stress [N]", "Finite read-only test, default 100; 1..10000 rounds");
  item("watch <addr> <len> [N interval]", "Read-only repeated view; defaults 20 rounds, 1000 ms");
  item("selfcheck / selftest", "Read whole configured array; geometry, CRC and health report");
  item("heap", "Application heap telemetry, when adapter supports it");
  item("stats [reset]", "CLI job counts and health; reset preserves driver health");
  item("xfer_stats [reset] / xfer_reset", "Physical bus counters, including scans and ACK polls");
  item("xfer_assert <N> [read write probe]", "Assert exact physical transfer counts");
  item("wp [0|1]", "Read/set application WP pin; 1=protected, 0=writable");
  item("iface_reset", "Explicit application bus recovery while idle and settled");
  section("Explicit scratch tests");
  item("rw_suite <addr> <len> confirm", "Backup <=256 bytes, pattern/fill/update tests, restore+verify");
  item("xfer_demo <addr> <len> confirm", "One-pass cooperative write/readback/restore demonstration");
  item("stress_mix <addr> <len> N confirm", "Finite alternating patterns; 1..100 rounds, then restore");
  item("randbench <addr> <len> N confirm", "1..1000 seeded random verified byte writes, then restore");
  item("typed_demo <addr> confirm", "Fixed little-endian integer layout in 14 bytes, then restore");
  item("scratch / restore confirm", "Show retained backup / explicitly authorize restoration");
  print("Scratch tests consume endurance; failures/cancel retain backup and require explicit restore confirm.\n");
  print("\nNumbers: decimal or 0x hex. Write acceptance is not persistence; WP may silently suppress writes.\n");
  print("Address aliases may belong to one banked chip. ACK cannot identify manufacturer or capacity.\n");
}
void Cli::printSettings() {
  print("Staged: model=%s address=0x%02X timeout=%lu ms tx=%lu rx=%lu offline-threshold=%u\n",
        modelName(_config.variant), _config.i2cAddress, static_cast<unsigned long>(_config.i2cTimeoutMs),
        static_cast<unsigned long>(_config.maxTxBytes), static_cast<unsigned long>(_config.maxRxBytes),
        static_cast<unsigned>(_config.offlineThreshold));
  const auto staged = _config.variant == EEPROM24Cxx::DeviceVariant::CUSTOM ?
      _config.customGeometry : EEPROM24Cxx::geometryFor(_config.variant);
  print("Staged geometry: capacity=%lu page=%u word-address=%u bank-bits=%u bank-shift=%u write-cycle=%lu ms\n",
        static_cast<unsigned long>(staged.capacityBytes), static_cast<unsigned>(staged.pageSizeBytes),
        static_cast<unsigned>(staged.wordAddressBytes), static_cast<unsigned>(staged.bankAddressBits),
        static_cast<unsigned>(staged.bankAddressShift),
        static_cast<unsigned long>(_config.writeCycleMs ? _config.writeCycleMs : staged.writeCycleMs));
  const auto snapshot = _device.settingsSnapshot();
  if (!_device.isBound()) { print("Active: bound=no; staged settings take effect on bind/begin.\n"); return; }
  print("Active: bound=yes model=%s address=0x%02X capacity=%lu page=%u word-address=%u bank-bits=%u bank-shift=%u write-cycle=%lu ms\n",
        modelName(snapshot.variant), static_cast<unsigned>(snapshot.i2cAddress), static_cast<unsigned long>(snapshot.geometry.capacityBytes),
        static_cast<unsigned>(snapshot.geometry.pageSizeBytes), static_cast<unsigned>(snapshot.geometry.wordAddressBytes),
        static_cast<unsigned>(snapshot.geometry.bankAddressBits), static_cast<unsigned>(snapshot.geometry.bankAddressShift),
        static_cast<unsigned long>(snapshot.writeCycleMs));
  print("Active transport: timeout=%lu ms tx=%lu rx=%lu write-payload=%lu read-chunk=%lu clock=%s ack-polling=%s offline-threshold=%u\n",
        static_cast<unsigned long>(snapshot.i2cTimeoutMs), static_cast<unsigned long>(snapshot.maxTxBytes),
        static_cast<unsigned long>(snapshot.maxRxBytes), static_cast<unsigned long>(snapshot.maxWriteDataBytes),
        static_cast<unsigned long>(snapshot.maxReadDataBytes), snapshot.hasNowMsHook ? "yes" : "no",
        snapshot.hasAckPolling ? "yes" : "no", static_cast<unsigned>(snapshot.offlineThreshold));
  print("Current pointer: supported=%s known=%s address=0x%05lX; reset-settling=%s\n",
        snapshot.supportsCurrentAddressRead ? "yes" : "no", snapshot.currentAddressKnown ? "yes" : "no",
        static_cast<unsigned long>(snapshot.currentAddress), _interfaceWait ? "yes" : "no");
}
void Cli::printModels() {
  print("Profiles (configured geometry; never detected from ACK):\n");
  for (const auto& model : models) {
    const auto geometry = EEPROM24Cxx::geometryFor(model.variant);
    print("  %-10s capacity=%lu page=%u pointer=%u bank-bits=%u bank-shift=%u tWR=%lu ms\n",
          model.name, static_cast<unsigned long>(geometry.capacityBytes), static_cast<unsigned>(geometry.pageSizeBytes),
          static_cast<unsigned>(geometry.wordAddressBytes), static_cast<unsigned>(geometry.bankAddressBits),
          static_cast<unsigned>(geometry.bankAddressShift), static_cast<unsigned long>(geometry.writeCycleMs));
  }
  print("Generic 24cXX profiles require confirmation against the fitted manufacturer's datasheet.\n");
}
void Cli::printHeap() {
  if (!_platform.heapStats) {
    status(EEPROM24Cxx::Status::Error(EEPROM24Cxx::Err::UNSUPPORTED, "no application heap telemetry adapter"));
    return;
  }
  const auto heap = _platform.heapStats(_platform.user);
  print("Heap: free=%lu minimum-free=%lu largest-free-block=%lu bytes\n", static_cast<unsigned long>(heap.freeBytes),
        static_cast<unsigned long>(heap.minimumFreeBytes), static_cast<unsigned long>(heap.largestFreeBlock));
}
void Cli::printTransferStats() {
  if (!_platform.transferStats) { status(EEPROM24Cxx::Status::Error(EEPROM24Cxx::Err::UNSUPPORTED, "no transfer counters")); return; }
  const auto counters = _platform.transferStats(_platform.user);
  print("Transfers: attempts=%lu read=%lu write=%lu probe=%lu ok=%lu fail=%lu\n",
        static_cast<unsigned long>(counters.attempts), static_cast<unsigned long>(counters.readAttempts),
        static_cast<unsigned long>(counters.writeAttempts), static_cast<unsigned long>(counters.probeAttempts),
        static_cast<unsigned long>(counters.successes), static_cast<unsigned long>(counters.failures));
}
void Cli::printStats() {
  print("Core jobs consumed: total=%lu ok=%lu fail=%lu (stream buffers and scratch stages count separately)\n",
        static_cast<unsigned long>(_jobs), static_cast<unsigned long>(_jobSuccesses), static_cast<unsigned long>(_jobFailures));
  printHealth();
}
void Cli::printScratch() {
  const char* stage = "IDLE";
  switch (_scratchStage) {
    case ScratchStage::NONE: break;
    case ScratchStage::BACKUP: stage = "BACKUP"; break;
    case ScratchStage::PATTERN: stage = "PATTERN"; break;
    case ScratchStage::RESTORE_WAIT: stage = "RESTORE_WAIT"; break;
    case ScratchStage::RESTORING: stage = "RESTORING"; break;
  }
  print("Scratch: stage=%s address=0x%05lX length=%lu rounds=%lu/%lu backup=%s restore-required=%s primary=%s detail=%ld restore=%s detail=%ld\n",
        stage, static_cast<unsigned long>(_scratchAddress),
        static_cast<unsigned long>(_scratchLength), static_cast<unsigned long>(_scratchRound),
        static_cast<unsigned long>(_scratchRounds), _backupValid ? "retained" : "none", _scratchDirty ? "yes" : "no",
        EEPROM24Cxx::errorName(_scratchPrimary.code), static_cast<long>(_scratchPrimary.detail),
        EEPROM24Cxx::errorName(_scratchRestore.code), static_cast<long>(_scratchRestore.detail));
  if (_backupValid && _scratchStage == ScratchStage::NONE)
    print("Backup retained in RAM; use restore confirm when ready. End/cancel never restore or discard changed data.\n");
  print("Scratch metrics: primary-ms=%lu restore-ms=%lu verified-pattern-bytes=%lu",
        static_cast<unsigned long>(_scratchPrimaryElapsedMs), static_cast<unsigned long>(_scratchRestoreElapsedMs),
        static_cast<unsigned long>(_scratchVerifiedBytes));
  if (_platform.transferStats)
    print(" primary-read=%lu primary-write=%lu primary-probe=%lu", static_cast<unsigned long>(_scratchPrimaryBus.readAttempts),
          static_cast<unsigned long>(_scratchPrimaryBus.writeAttempts), static_cast<unsigned long>(_scratchPrimaryBus.probeAttempts));
  print(" (primary timing includes backup)\n");
}
EEPROM24Cxx::Status Cli::startScratch(ScratchMode mode, uint32_t address, uint32_t length, uint32_t rounds) {
  using namespace EEPROM24Cxx;
  if (!_device.isBound()) return Status::Error(Err::NOT_INITIALIZED, "bind driver first");
  if (_backupValid) return Status::Error(Err::BUSY, "restore retained backup first");
  if (!_platform.nowMs && !_config.nowMs) return Status::Error(Err::INVALID_CONFIG, "scratch writes require a clock");
  if (!length || length > sizeof(_backup) || !memory::fitsRange(_device.capacityBytes(), address, length))
    return Status::Error(Err::ADDRESS_OUT_OF_RANGE, "scratch range must fit configured capacity and 256-byte backup");
  const auto result = _device.startRead(address, _backup, length);
  if (!result.ok()) return result;
  _scratchMode = mode; _scratchAddress = address; _scratchLength = length;
  _scratchRounds = rounds; _scratchRound = 0; _scratchRandom = 0x24C02B01U;
  _scratchDirty = false; _scratchStage = ScratchStage::BACKUP;
  _scratchStartedMs = now(); _scratchPrimaryElapsedMs = 0; _scratchRestoreElapsedMs = 0; _scratchVerifiedBytes = 0;
  _scratchBusBefore = _platform.transferStats ? _platform.transferStats(_platform.user) : TransferStats{};
  _scratchPrimaryBus = {};
  _scratchPrimary = Status::Error(Err::IN_PROGRESS, "scratch test queued");
  _scratchRestore = Status::Error(Err::NO_RESULT, "restoration not attempted");
  _operation = true; _hasResult = false;
  print("Scratch test queued: backup, %lu verified pattern rounds, verified restore; EEPROM endurance is consumed.\n",
        static_cast<unsigned long>(rounds));
  return result;
}
void Cli::finishScratchPrimary(EEPROM24Cxx::Status result) {
  _scratchPrimary = result;
  _scratchPrimaryElapsedMs = now() - _scratchStartedMs;
  if (_platform.transferStats) {
    const auto current = _platform.transferStats(_platform.user);
    _scratchPrimaryBus.readAttempts = current.readAttempts - _scratchBusBefore.readAttempts;
    _scratchPrimaryBus.writeAttempts = current.writeAttempts - _scratchBusBefore.writeAttempts;
    _scratchPrimaryBus.probeAttempts = current.probeAttempts - _scratchBusBefore.probeAttempts;
  }
}
void Cli::abortScratch(EEPROM24Cxx::Status reason) {
  if (_scratchStage == ScratchStage::RESTORING || _scratchStage == ScratchStage::RESTORE_WAIT) {
    _scratchRestore = reason;
    _scratchRestoreElapsedMs = now() - _scratchRestoreStartedMs;
  } else finishScratchPrimary(reason);
  if (!_scratchDirty) _backupValid = false;
  _scratchStage = ScratchStage::NONE;
  printScratch();
}
void Cli::completeScratch(const EEPROM24Cxx::TransferResult& result) {
  status(result.status); printProgress(result);
  if (_scratchStage == ScratchStage::PATTERN) _scratchVerifiedBytes += static_cast<uint32_t>(result.bytesVerified);
  if (!result.status.ok()) { abortScratch(result.status); return; }
  if (_scratchStage == ScratchStage::BACKUP) {
    _backupValid = true;
    _scratchStage = ScratchStage::PATTERN;
  } else if (_scratchStage == ScratchStage::PATTERN) {
    ++_scratchRound;
    if (_scratchRound == _scratchRounds) {
      finishScratchPrimary(EEPROM24Cxx::Status::Ok());
      _scratchRestoreStartedMs = now();
      _scratchRestore = EEPROM24Cxx::Status::Error(EEPROM24Cxx::Err::IN_PROGRESS, "authorized restoration queued");
      _scratchStage = ScratchStage::RESTORE_WAIT;
    }
  } else if (_scratchStage == ScratchStage::RESTORING) {
    _scratchRestore = EEPROM24Cxx::Status::Ok();
    _scratchRestoreElapsedMs = now() - _scratchRestoreStartedMs;
    _backupValid = false; _scratchDirty = false; _scratchStage = ScratchStage::NONE;
    printScratch();
    print("Original scratch bytes observed by readback after restoration.\n");
  }
}
void Cli::scheduleScratch() {
  using namespace EEPROM24Cxx;
  if (_scratchStage == ScratchStage::NONE || _operation) return;
  if (_device.settingsSnapshot().writeCyclePending) return;
  Status result = Status::Error(Err::INVALID_PARAM, "invalid scratch stage");
  if (_scratchStage == ScratchStage::RESTORE_WAIT) {
    result = _device.startWrite(_scratchAddress, _backup, _scratchLength, true);
    if (result.ok()) _scratchStage = ScratchStage::RESTORING;
  } else if (_scratchStage == ScratchStage::PATTERN) {
    uint32_t address = _scratchAddress;
    uint32_t length = _scratchLength;
    for (uint32_t index = 0; index < length; ++index)
      _data[index] = static_cast<uint8_t>((address + index) ^ 0xA5U);
    if (_scratchMode == ScratchMode::MIX)
      std::memset(_data, (_scratchRound & 1U) ? 0x5A : 0xA5, length);
    if (_scratchMode == ScratchMode::RANDOM) {
      _scratchRandom ^= _scratchRandom << 13U; _scratchRandom ^= _scratchRandom >> 17U; _scratchRandom ^= _scratchRandom << 5U;
      address += _scratchRandom % length;
      _data[0] = static_cast<uint8_t>(_scratchRandom >> 16U); length = 1;
    }
    if (_scratchMode == ScratchMode::TYPED) {
      memory::encodeUint16Le(0xA55AU, _data);
      memory::encodeUint32Le(0x12345678U, _data + 2);
      memory::encodeUint64Le(UINT64_C(0x0123456789ABCDEF), _data + 6);
      print("Typed layout: uint16 LE @+0, uint32 LE @+2, uint64 LE @+6; 14 bytes.\n");
    }
    if (_scratchMode == ScratchMode::RW_SUITE && (_scratchRound == 1 || _scratchRound == 2))
      result = _device.startFill(address, _scratchRound == 1 ? 0x5A : 0xA5, length, true);
    else if (_scratchMode == ScratchMode::RW_SUITE && _scratchRound == 3) {
      std::memset(_data, 0xA5, length);
      result = _device.startUpdate(address, _data, length, true);
    } else result = _device.startWrite(address, _data, length, true);
    if (result.ok()) _scratchDirty = true;
  }
  if (result.ok()) { _operation = true; _hasResult = false; }
  else { status(result); abortScratch(result); }
}
void Cli::printHealth() {
  const auto snapshot = _device.settingsSnapshot();
  const auto state = snapshot.state;
  const unsigned shade = state == EEPROM24Cxx::DriverState::READY ? 32U :
                         state == EEPROM24Cxx::DriverState::OFFLINE ? 31U : 33U;
  print("Health: state=%s%s%s bound=%s consec=%u ok=%lu fail=%lu\n", color(shade),
        EEPROM24Cxx::driverStateName(state), color(0), _device.isBound() ? "yes" : "no",
        static_cast<unsigned>(snapshot.consecutiveFailures),
        static_cast<unsigned long>(snapshot.totalSuccess), static_cast<unsigned long>(snapshot.totalFailures));
  const uint64_t attempts = static_cast<uint64_t>(snapshot.totalSuccess) + snapshot.totalFailures;
  if (!attempts) print("  Success rate: %sn/a%s\n", color(90), color(0));
  else {
    const double rate = 100.0 * static_cast<double>(snapshot.totalSuccess) / static_cast<double>(attempts);
    print("  Success rate: %s%.1f%%%s\n", color(rate >= 99.9 ? 32U : rate >= 80.0 ? 33U : 31U), rate, color(0));
  }
  print("Last: ok=%lu ms error=%lu ms %s detail=%ld%s%s\n", static_cast<unsigned long>(snapshot.lastOkMs),
        static_cast<unsigned long>(snapshot.lastErrorMs), EEPROM24Cxx::errorName(snapshot.lastError.code),
        static_cast<long>(snapshot.lastError.detail), snapshot.lastError.msg && *snapshot.lastError.msg ? ": " : "",
        snapshot.lastError.msg ? snapshot.lastError.msg : "");
  print("  Binding permits owner-directed I/O; READY/DEGRADED/OFFLINE describe observed transport health.\n");
  print("EEPROM: write-busy-polls=%lu write-cycle-pending=%s ready-at=%lu ms transfer-active=%s result-pending=%s\n",
        static_cast<unsigned long>(snapshot.writeBusyPolls), snapshot.writeCyclePending ? "yes" : "no",
        static_cast<unsigned long>(snapshot.writeReadyAtMs), snapshot.transferActive ? "yes" : "no",
        snapshot.resultPending ? "yes" : "no");
  if (_platform.transferStats) {
    const auto bus = _platform.transferStats(_platform.user);
    print("Bus: attempts=%lu ok=%lu fail=%lu (includes scan/probe; independent of driver health)\n",
          static_cast<unsigned long>(bus.attempts), static_cast<unsigned long>(bus.successes),
          static_cast<unsigned long>(bus.failures));
  }
}
void Cli::printProgress(const EEPROM24Cxx::TransferResult& result) {
  print("Transfer: kind=%s state=%s address=0x%05lX requested=%lu accepted=%lu completed=%lu verified=%lu commit=%s last-chunk-commit=%s status=%s detail=%ld request-id=%lu\n",
        EEPROM24Cxx::transferKindName(result.kind), EEPROM24Cxx::transferStateName(result.state),
        static_cast<unsigned long>(result.address), static_cast<unsigned long>(result.bytesRequested),
        static_cast<unsigned long>(result.bytesAccepted), static_cast<unsigned long>(result.bytesCompleted),
        static_cast<unsigned long>(result.bytesVerified), EEPROM24Cxx::writeCommitName(result.writeCommit),
        EEPROM24Cxx::writeCommitName(result.lastChunkCommit), EEPROM24Cxx::errorName(result.status.code),
        static_cast<long>(result.status.detail), static_cast<unsigned long>(result.requestId));
  if (result.kind == EEPROM24Cxx::TransferKind::WRITE || result.kind == EEPROM24Cxx::TransferKind::FILL ||
      result.kind == EEPROM24Cxx::TransferKind::VERIFIED_WRITE || result.kind == EEPROM24Cxx::TransferKind::VERIFIED_FILL ||
      result.kind == EEPROM24Cxx::TransferKind::UPDATE || result.kind == EEPROM24Cxx::TransferKind::VERIFIED_UPDATE)
    print("Write: status=%s detail=%ld\n", EEPROM24Cxx::errorName(result.writeStatus.code),
          static_cast<long>(result.writeStatus.detail));
  if (result.comparisonAttempted)
    print("Update comparison: status=%s detail=%ld compared=%lu skipped=%lu\n", EEPROM24Cxx::errorName(result.compareStatus.code),
          static_cast<long>(result.compareStatus.detail), static_cast<unsigned long>(result.bytesCompared),
          static_cast<unsigned long>(result.bytesSkipped));
  if (result.verificationAttempted)
    print("Verification: status=%s detail=%ld match=%s\n", EEPROM24Cxx::errorName(result.verifyStatus.code),
          static_cast<long>(result.verifyStatus.detail), result.match ? "yes" : "no");
  if (result.status.code == EEPROM24Cxx::Err::VERIFY_MISMATCH)
    print("Mismatch: offset=%lu expected=0x%02X actual=0x%02X\n", static_cast<unsigned long>(result.mismatchOffset),
          static_cast<unsigned>(result.expected), static_cast<unsigned>(result.actual));
  if (!result.status.ok() && !result.status.inProgress())
    print("Failure: chunk-offset=%lu chunk-length=%lu\n", static_cast<unsigned long>(result.failedChunkOffset),
          static_cast<unsigned long>(result.failedChunkLength));
}
void Cli::printBytes(uint32_t address, size_t length) {
  for (size_t offset = 0; offset < length; offset += 16) {
    print("%05lX:", static_cast<unsigned long>(address + offset));
    for (size_t index = offset; index < length && index < offset + 16; ++index)
      print(" %02X", static_cast<unsigned>(_data[index]));
    for (size_t index = length - offset; index < 16; ++index) print("   ");
    print("  |");
    for (size_t index = offset; index < length && index < offset + 16; ++index)
      print("%c", _data[index] >= 32 && _data[index] <= 126 ? _data[index] : '.');
    print("|\n");
  }
}
EEPROM24Cxx::Status Cli::startReadView(ReadView view, uint32_t address, uint32_t length,
                                      uint32_t minimumStringLength) {
  using namespace EEPROM24Cxx;
  if (!_device.isBound()) return Status::Error(Err::NOT_INITIALIZED, "bind driver first");
  const auto capacity = _device.settingsSnapshot().capacityBytes;
  if (!length || address >= capacity || length > capacity - address)
    return Status::Error(Err::ADDRESS_OUT_OF_RANGE, "read range exceeds configured capacity");
  const auto result = _device.startRead(address, _data, length < sizeof(_data) ? length : sizeof(_data));
  if (!result.ok()) return result;
  _readView = view;
  _readRequestId = _device.transferSnapshot().requestId;
  _readAddress = address;
  _readLength = length;
  _readCompleted = 0;
  _crc = 0xFFFFFFFFU;
  _minimumStringLength = minimumStringLength;
  _stringLength = 0;
  _stringCount = 0;
  return result;
}
void Cli::finishString() {
  if (_stringLength >= _minimumStringLength) { print("\n"); ++_stringCount; }
  _stringLength = 0;
}
void Cli::printReadView(uint32_t address, size_t length) {
  if (_readView == ReadView::HEX_DUMP) { printBytes(address, length); return; }
  if (_readView == ReadView::CRC || _readView == ReadView::SELFTEST) {
    _crc = EEPROM24Cxx::memory::crc32Update(_crc, _data, length); return;
  }
  if (_readView == ReadView::TEXT) print("%05lX: \"", static_cast<unsigned long>(address));
  for (size_t index = 0; index < length; ++index) {
    const uint8_t value = _data[index];
    const bool printable = value >= 32 && value <= 126;
    if (_readView == ReadView::TEXT) {
      if (value == '\\' || value == '"') print("\\%c", value);
      else if (printable) print("%c", value);
      else print("\\x%02X", static_cast<unsigned>(value));
    } else if (_readView == ReadView::STRINGS) {
      if (!printable) { finishString(); continue; }
      if (!_stringLength) _stringAddress = address + static_cast<uint32_t>(index);
      if (_stringLength < _minimumStringLength) _stringPrefix[_stringLength] = static_cast<char>(value);
      ++_stringLength;
      if (_stringLength == _minimumStringLength)
        print("%05lX: %.*s", static_cast<unsigned long>(_stringAddress),
              static_cast<int>(_minimumStringLength), _stringPrefix);
      else if (_stringLength > _minimumStringLength) print("%c", value);
    }
  }
  if (_readView == ReadView::TEXT) print("\"\n");
}
void Cli::completeReadView(const EEPROM24Cxx::TransferResult& chunk) {
  using namespace EEPROM24Cxx;
  if (chunk.bytesCompleted) printReadView(_readAddress + _readCompleted, chunk.bytesCompleted);
  _lastResult = chunk;
  _lastResult.requestId = _readRequestId;
  _lastResult.address = _readAddress;
  _lastResult.bytesRequested = _readLength;
  _lastResult.failedChunkOffset += _readCompleted;
  _readCompleted += static_cast<uint32_t>(chunk.bytesCompleted);
  _lastResult.bytesCompleted = _readCompleted;
  if (chunk.status.ok() && _readCompleted < _readLength) {
    _lastResult.state = TransferState::ACTIVE;
    _lastResult.status = Status::Error(Err::IN_PROGRESS, "read-only range continues on next tick");
    return;
  }
  if (_readView == ReadView::STRINGS) {
    finishString();
    print("Strings: %lu found in %lu completed bytes%s\n", static_cast<unsigned long>(_stringCount),
          static_cast<unsigned long>(_readCompleted), chunk.status.ok() ? "" : " (partial range)");
  }
  if (_readView == ReadView::CRC && chunk.status.ok())
    print("CRC32/ISO-HDLC: address=0x%05lX length=%lu crc=0x%08lX\n", static_cast<unsigned long>(_readAddress),
          static_cast<unsigned long>(_readLength), static_cast<unsigned long>(_crc ^ 0xFFFFFFFFU));
  if (_readView == ReadView::SELFTEST) {
    print("Read-only selftest: %s bytes=%lu", chunk.status.ok() ? "PASS" : "FAIL", static_cast<unsigned long>(_readCompleted));
    if (chunk.status.ok()) print(" crc=0x%08lX", static_cast<unsigned long>(_crc ^ 0xFFFFFFFFU));
    print("; tests access only, not identity, capacity, WP or endurance.\n");
    printHealth();
  }
  _readView = ReadView::NONE;
  _hasResult = true;
  status(_lastResult.status);
  printProgress(_lastResult);
}
void Cli::complete() {
  EEPROM24Cxx::TransferResult result{};
  if (!_device.takeResult(result).ok()) return;
  _operation = false;
  if (_jobs != UINT32_MAX) ++_jobs;
  auto& counter = result.status.ok() ? _jobSuccesses : _jobFailures;
  if (counter != UINT32_MAX) ++counter;
  if (_readView != ReadView::NONE) { completeReadView(result); return; }
  _lastResult = result;
  _hasResult = true;
  if (_scratchStage != ScratchStage::NONE) { completeScratch(result); return; }
  if (!_stress || _watch || _verbose || !result.status.ok()) {
    status(result.status);
    printProgress(result);
    if ((result.kind == EEPROM24Cxx::TransferKind::READ || result.kind == EEPROM24Cxx::TransferKind::CURRENT_READ) && result.bytesCompleted)
      printBytes(result.address, result.bytesCompleted);
  }
  if (_stress) {
    if (result.status.ok()) ++_stressSuccess; else ++_stressFailures;
    if (_remaining) --_remaining;
    _nextMs = now() + _repeatInterval;
    if (!_remaining) stop();
  }
}
void Cli::stop() {
  const bool wasStress = _stress;
  const bool wasWatch = _watch;
  _stress = false;
  _remaining = 0;
  _scan = false;
  if (_operation) {
    const auto kind = _device.transferSnapshot().kind;
    const auto cancelled = _device.cancel();
    if (!cancelled.ok()) status(cancelled);
    complete();
    if (kind == EEPROM24Cxx::TransferKind::WRITE || kind == EEPROM24Cxx::TransferKind::FILL ||
        kind == EEPROM24Cxx::TransferKind::VERIFIED_WRITE || kind == EEPROM24Cxx::TransferKind::VERIFIED_FILL ||
        kind == EEPROM24Cxx::TransferKind::UPDATE || kind == EEPROM24Cxx::TransferKind::VERIFIED_UPDATE)
      print("Scheduling stopped; already-issued EEPROM programming cannot be undone.\n");
  } else if (_readView != ReadView::NONE) {
    EEPROM24Cxx::TransferResult cancelled{};
    cancelled.kind = EEPROM24Cxx::TransferKind::READ;
    cancelled.state = EEPROM24Cxx::TransferState::CANCELLED;
    cancelled.status = EEPROM24Cxx::Status::Error(EEPROM24Cxx::Err::CANCELLED, "read-only range cancelled");
    completeReadView(cancelled);
  }
  if (_scratchStage != ScratchStage::NONE)
    abortScratch(EEPROM24Cxx::Status::Error(EEPROM24Cxx::Err::CANCELLED, "scratch scheduling stopped"));
  if (wasStress) print("%s stopped: ok=%lu fail=%lu\n", wasWatch ? "Watch" : "Stress", static_cast<unsigned long>(_stressSuccess),
                       static_cast<unsigned long>(_stressFailures));
  _watch = false;
}
void Cli::feed(char value) {
  if (value == '\r' || value == '\n') {
    if (_overflow) print("%s[E]%s Input invalid or too long; command discarded\n", color(31), color(0));
    else if (_length) { _line[_length] = '\0'; processCommand(_line); }
    else return;
    _length = 0;
    _overflow = false;
    printPrompt();
  } else if (value == '\b' || value == 127) {
    if (!_overflow && _length) --_length;
  } else if (!_overflow) {
    if ((static_cast<unsigned char>(value) < 32U && value != '\t') || static_cast<unsigned char>(value) > 126U)
      _overflow = true;
    else if (_length + 1 < sizeof(_line)) _line[_length++] = value;
    else _overflow = true;
  }
}
void Cli::processCommand(const char* text) {
  using namespace EEPROM24Cxx;
  const auto invalid = [this]() { status(Status::Error(Err::INVALID_PARAM, "invalid command/arguments; see help")); };
  char buffer[sizeof(_line)]{};
  size_t length = 0;
  if (!text) { invalid(); return; }
  while (length < sizeof(buffer) && text[length]) {
    const auto value = static_cast<unsigned char>(text[length]);
    if ((value < 32U && value != '\t') || value > 126U) { invalid(); return; }
    ++length;
  }
  if (length == sizeof(buffer)) { invalid(); return; }
  std::memcpy(buffer, text, length + 1);
  char* args[34]{};
  size_t count = 0;
  char* cursor = buffer;
  while (*cursor) {
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    if (!*cursor) break;
    if (count == 34) { invalid(); return; }
    args[count++] = cursor;
    while (*cursor && *cursor != ' ' && *cursor != '\t') ++cursor;
    if (*cursor) *cursor++ = '\0';
  }
  if (!count) return;
  const char* command = args[0];
  if ((equals(command, "help") || equals(command, "?")) && count == 1) { printHelp(); return; }
  if ((equals(command, "version") || equals(command, "ver")) && count == 1) { printVersion(); return; }
  if (equals(command, "color") || equals(command, "verbose")) {
    if (count > 2) { invalid(); return; }
    bool& setting = equals(command, "color") ? _color : _verbose;
    if (count == 2) {
      if (equals(args[1], "on") || equals(args[1], "1")) setting = true;
      else if (equals(args[1], "off") || equals(args[1], "0")) setting = false;
      else { invalid(); return; }
    }
    print("%s: %s\n", equals(command, "color") ? "Color" : "Verbose", setting ? "on" : "off"); return;
  }
  if ((equals(command, "stop") || equals(command, "cancel")) && count == 1) { stop(); return; }
  if ((equals(command, "drv") || equals(command, "health") || equals(command, "state") || equals(command, "online")) && count == 1) { printHealth(); return; }
  if ((equals(command, "cfg") || equals(command, "settings") || equals(command, "snapshot")) && count == 1) { printSettings(); return; }
  if ((equals(command, "geometry") || equals(command, "timing")) && count == 1) { printSettings(); printHealth(); return; }
  if (equals(command, "scratch") && count == 1) { printScratch(); return; }
  if (equals(command, "page")) {
    uint32_t address = 0;
    if (count > 2 || (count == 2 && !integer(args[1], 0, UINT32_MAX, address))) { invalid(); return; }
    const auto geometry = _device.isBound() ? _device.settingsSnapshot().geometry :
        _config.variant == DeviceVariant::CUSTOM ? _config.customGeometry : geometryFor(_config.variant);
    if (geometry.wordAddressBytes != 1 && geometry.wordAddressBytes != 2) { status(Status::Error(Err::INVALID_CONFIG, "invalid geometry pointer width")); return; }
    if (!geometry.pageSizeBytes || address >= geometry.capacityBytes) { status(Status::Error(Err::ADDRESS_OUT_OF_RANGE, "page address exceeds capacity")); return; }
    print("Page: address=0x%05lX number=%lu offset=%lu remaining=%lu bank-remaining=%lu\n",
          static_cast<unsigned long>(address), static_cast<unsigned long>(address / geometry.pageSizeBytes),
          static_cast<unsigned long>(address % geometry.pageSizeBytes),
          static_cast<unsigned long>(memory::pageRemaining(geometry, address)),
          static_cast<unsigned long>(memory::bankRemaining(geometry, address))); return;
  }
  if (equals(command, "stats") || equals(command, "xfer_stats") || equals(command, "xfer_reset")) {
    const bool reset = equals(command, "xfer_reset") || (count == 2 && equals(args[1], "reset"));
    if (count > 2 || (count == 2 && (!equals(args[1], "reset") || equals(command, "xfer_reset")))) { invalid(); return; }
    if (reset) {
      if (_scratchStage != ScratchStage::NONE) { status(Status::Error(Err::BUSY, "finish scratch metrics before resetting counters")); return; }
      if (!_platform.resetTransferStats && !equals(command, "stats")) { status(Status::Error(Err::UNSUPPORTED, "no transfer-counter reset adapter")); return; }
      if (_platform.resetTransferStats) _platform.resetTransferStats(_platform.user);
      if (equals(command, "stats")) _jobs = _jobSuccesses = _jobFailures = 0;
      print("Diagnostic counters reset; passive driver health preserved.\n");
    }
    if (equals(command, "stats")) printStats(); else printTransferStats();
    return;
  }
  if (equals(command, "xfer_assert")) {
    uint32_t expected[4]{};
    if (count != 2 && count != 5) { invalid(); return; }
    for (size_t index = 1; index < count; ++index)
      if (!integer(args[index], 0, UINT32_MAX, expected[index - 1])) { invalid(); return; }
    if (!_platform.transferStats) { status(Status::Error(Err::UNSUPPORTED, "no transfer counters")); return; }
    const auto counters = _platform.transferStats(_platform.user);
    const bool match = counters.attempts == expected[0] && (count == 2 ||
        (counters.readAttempts == expected[1] && counters.writeAttempts == expected[2] && counters.probeAttempts == expected[3]));
    printTransferStats(); status(match ? Status::Ok() : Status::Error(Err::VERIFY_MISMATCH, "physical transfer counts differ")); return;
  }
  if (equals(command, "wp") && count == 1) {
    bool protectedState = false;
    const auto result = _platform.readWriteProtect ? _platform.readWriteProtect(protectedState, _platform.user) :
        Status::Error(Err::UNSUPPORTED, "no application WP input");
    status(result); if (result.ok()) print("WP pin: %s; EEPROM storage still requires readback verification.\n", protectedState ? "protected" : "writable"); return;
  }
  if (equals(command, "variants") && count == 1) { printModels(); return; }
  if (equals(command, "heap") && count == 1) { printHeap(); return; }
  if (equals(command, "size") && count == 1) {
    const auto geometry = _device.isBound() ? _device.settingsSnapshot().geometry :
        _config.variant == DeviceVariant::CUSTOM ? _config.customGeometry : geometryFor(_config.variant);
    print("%s configured capacity: %lu bytes (not detected)\n", _device.isBound() ? "Active" : "Staged",
          static_cast<unsigned long>(geometry.capacityBytes)); return;
  }
  if ((equals(command, "addr") || equals(command, "timeout") || equals(command, "model") || equals(command, "offline")) && count == 1) {
    printSettings();
    if (equals(command, "model")) printModels();
    return;
  }
  if ((equals(command, "progress") || equals(command, "status") || equals(command, "job") || equals(command, "result")) && count == 1) {
    auto result = !_operation && (_hasResult || _readView != ReadView::NONE) ? _lastResult : _device.transferSnapshot();
    if (_readView != ReadView::NONE && _operation) {
      result.requestId = _readRequestId;
      result.address = _readAddress;
      result.bytesRequested = _readLength;
      result.bytesCompleted += _readCompleted;
      result.failedChunkOffset += _readCompleted;
    }
    printProgress(result); if (_scratchStage != ScratchStage::NONE || _backupValid) printScratch(); return;
  }
  if (equals(command, "diag") && count == 1) { printVersion(); printSettings(); printHealth(); return; }
  if ((equals(command, "end") || equals(command, "unbind")) && count == 1) {
    stop(); _device.end(); print("Driver released; bus unchanged.\n"); return;
  }
  if (_operation || _scan || _stress || _readView != ReadView::NONE || _scratchStage != ScratchStage::NONE || _interfaceWait) { status(Status::Error(Err::BUSY, "work active or bus recovery settling")); return; }
  if ((equals(command, "scan") || equals(command, "discover")) && count == 1) {
    if (_device.settingsSnapshot().writeCyclePending) { status(Status::Error(Err::BUSY, "wait for physical write cycle before scanning")); return; }
    if (!_platform.probeAddress) { status(Status::Error(Err::INVALID_CONFIG, "no scan adapter")); return; }
    _scan = true; _scanNext = equals(command, "scan") ? 0x08 : 0x50;
    _scanLast = equals(command, "scan") ? 0x77 : 0x57; _scanFound = 0; _scanErrors = 0; return;
  }
  if (equals(command, "init") || equals(command, "begin")) {
    uint32_t address = _config.i2cAddress;
    if (count > 2 || (count == 2 && !integer(args[1], 0x50, 0x57, address))) { invalid(); return; }
    if (_backupValid && address != _config.i2cAddress) { status(Status::Error(Err::BUSY, "restore backup before changing target")); return; }
    auto candidate = _config; candidate.i2cAddress = static_cast<uint8_t>(address);
    const auto bound = _device.bind(candidate);
    if (bound.ok()) { _config = candidate; status(_device.recover()); } else status(bound);
    return;
  }
  if (equals(command, "bind") && count == 1) { status(_device.bind(_config)); return; }
  if (equals(command, "probe") && count == 1) { status(_device.probe()); return; }
  if (equals(command, "recover") && count == 1) { status(_device.recover()); return; }
  if (equals(command, "wp")) {
    uint32_t value = 0;
    if (count != 2 || !integer(args[1], 0, 1, value)) { invalid(); return; }
    if (_device.settingsSnapshot().writeCyclePending) { status(Status::Error(Err::BUSY, "wait for physical write cycle before changing WP")); return; }
    status(_platform.setWriteProtect ? _platform.setWriteProtect(value != 0, _platform.user) :
        Status::Error(Err::UNSUPPORTED, "no application WP output")); return;
  }
  if (equals(command, "iface_reset") && count == 1) {
    if (_device.settingsSnapshot().writeCyclePending) { status(Status::Error(Err::BUSY, "wait for physical write cycle before bus recovery")); return; }
    const bool bound = _device.isBound();
    if (!_platform.nowMs && !_config.nowMs) { status(Status::Error(Err::INVALID_CONFIG, "bus recovery settling requires a clock")); return; }
    const auto geometry = _config.variant == DeviceVariant::CUSTOM ? _config.customGeometry : geometryFor(_config.variant);
    const uint32_t settleMs = _config.writeCycleMs > geometry.writeCycleMs ? _config.writeCycleMs : geometry.writeCycleMs;
    if (!settleMs || settleMs > 1000) { status(Status::Error(Err::INVALID_CONFIG, "invalid post-recovery write-cycle time")); return; }
    const auto result = _platform.resetInterface ? _platform.resetInterface(_platform.user) :
        Status::Error(Err::UNSUPPORTED, "no application bus recovery adapter");
    status(result);
    if (_platform.resetInterface && !result.is(Err::UNSUPPORTED)) {
      _interfaceWait = true; _interfaceReadyAt = now() + settleMs + 1U;
      if (bound) { _device.end(); status(_device.bind(_config)); }
      print("Bus recovery settling for %lu ms; owner tick releases access.\n", static_cast<unsigned long>(settleMs + 1U));
    }
    return;
  }
  if (equals(command, "addr") || equals(command, "timeout") || equals(command, "model") || equals(command, "offline")) {
    if (count > 2) { invalid(); return; }
    if (_backupValid) { status(Status::Error(Err::BUSY, "restore retained backup before changing settings")); return; }
    if (_device.isBound()) { status(Status::Error(Err::BUSY, "end driver before changing settings")); return; }
    uint32_t value = 0;
    if (equals(command, "addr")) {
      if (!integer(args[1], 0x50, 0x57, value)) { invalid(); return; }
      _config.i2cAddress = static_cast<uint8_t>(value);
    } else if (equals(command, "timeout")) {
      if (!integer(args[1], 1, 1000, value)) { invalid(); return; }
      _config.i2cTimeoutMs = value;
    } else if (equals(command, "offline")) {
      if (!integer(args[1], 0, 255, value)) { invalid(); return; }
      _config.offlineThreshold = static_cast<uint8_t>(value);
    } else {
      bool found = false;
      for (const auto& model : models) if (equals(args[1], model.name)) { _config.variant = model.variant; found = true; break; }
      if (!found) { invalid(); return; }
    }
    printSettings(); return;
  }
  if (equals(command, "restore")) {
    if (count != 2 || !equals(args[1], "confirm")) { invalid(); return; }
    if (!_backupValid) { status(Status::Error(Err::NO_RESULT, "no retained scratch backup")); return; }
    if (!_device.isBound()) { status(Status::Error(Err::NOT_INITIALIZED, "bind original target before restoration")); return; }
    _scratchRestore = Status::Error(Err::IN_PROGRESS, "explicit restoration queued");
    _scratchRestoreStartedMs = now(); _scratchRestoreElapsedMs = 0;
    _scratchStage = ScratchStage::RESTORE_WAIT;
    print("Verified restoration queued; physical write barrier remains enforced.\n"); return;
  }
  if (equals(command, "rw_suite") || equals(command, "xfer_demo") || equals(command, "stress_mix") ||
      equals(command, "randbench") || equals(command, "typed_demo")) {
    const bool typed = equals(command, "typed_demo");
    const bool repeated = equals(command, "stress_mix") || equals(command, "randbench");
    const size_t expected = typed ? 3U : repeated ? 5U : 4U;
    uint32_t address = 0, n = typed ? 14 : 0, rounds = equals(command, "rw_suite") ? 4 : 1;
    if (count != expected || !equals(args[count - 1], "confirm") || !integer(args[1], 0, UINT32_MAX, address) ||
        (!typed && !integer(args[2], 1, sizeof(_backup), n)) ||
        (repeated && !integer(args[3], 1, equals(command, "randbench") ? 1000U : 100U, rounds))) { invalid(); return; }
    const auto mode = typed ? ScratchMode::TYPED : equals(command, "stress_mix") ? ScratchMode::MIX :
        equals(command, "randbench") ? ScratchMode::RANDOM : equals(command, "xfer_demo") ? ScratchMode::XFER : ScratchMode::RW_SUITE;
    status(startScratch(mode, address, n, rounds)); return;
  }
  if (equals(command, "stress") || equals(command, "watch")) {
    const bool watch = equals(command, "watch");
    uint32_t rounds = 100;
    uint32_t address = 0, n = _device.capacityBytes() < 16 ? _device.capacityBytes() : 16, interval = 10;
    if (watch) {
      rounds = 20; interval = 1000;
      if (count < 3 || count > 5 || !integer(args[1], 0, UINT32_MAX, address) || !integer(args[2], 1, sizeof(_data), n) ||
          (count >= 4 && !integer(args[3], 1, 10000, rounds)) ||
          (count == 5 && !integer(args[4], 1, 60000, interval))) { invalid(); return; }
    } else if (count > 2 || (count == 2 && !integer(args[1], 1, 10000, rounds))) { invalid(); return; }
    if (!_device.isBound()) { status(Status::Error(Err::NOT_INITIALIZED, "bind driver first")); return; }
    if (!memory::fitsRange(_device.capacityBytes(), address, n)) { status(Status::Error(Err::ADDRESS_OUT_OF_RANGE, "watch range exceeds capacity")); return; }
    if (!_platform.nowMs && !_config.nowMs) { status(Status::Error(Err::INVALID_CONFIG, "CLI stress requires a clock callback")); return; }
    _stress = true; _watch = watch; _remaining = rounds; _stressSuccess = 0; _stressFailures = 0; _nextMs = now();
    _repeatAddress = address; _repeatLength = n; _repeatInterval = interval;
    print("Read-only %s: %lu rounds, %lu ms interval; stop cancels.\n", watch ? "watch" : "stress",
          static_cast<unsigned long>(rounds), static_cast<unsigned long>(interval)); return;
  }
  if ((equals(command, "selftest") || equals(command, "selfcheck")) && count == 1) {
    const auto capacity = _device.settingsSnapshot().capacityBytes;
    const auto result = startReadView(ReadView::SELFTEST, 0, capacity);
    status(result);
    if (result.ok()) { _operation = true; _hasResult = false; printSettings(); print("Queued whole-array read-only selftest; one transaction maximum per tick.\n"); }
    return;
  }
  if (equals(command, "strings")) {
    uint32_t address = 0, n = _device.settingsSnapshot().capacityBytes, minimum = 4;
    if ((count != 1 && count != 3 && count != 4) ||
        (count >= 3 && (!integer(args[1], 0, UINT32_MAX, address) || !integer(args[2], 1, UINT32_MAX, n))) ||
        (count == 4 && !integer(args[3], 1, sizeof(_stringPrefix), minimum))) { invalid(); return; }
    const auto result = startReadView(ReadView::STRINGS, address, n, minimum);
    status(result);
    if (result.ok()) { _operation = true; _hasResult = false; print("Queued read-only strings scan; one transaction maximum per tick.\n"); }
    return;
  }
  if (equals(command, "current") || equals(command, "cur")) {
    uint32_t n = 1;
    if (count > 2 || (count == 2 && !integer(args[1], 1, sizeof(_data), n))) { invalid(); return; }
    const auto result = _device.startCurrentRead(_data, n);
    status(result); if (result.ok()) { _operation = true; _hasResult = false; } return;
  }
  uint32_t address = 0;
  if (count < 2 || !integer(args[1], 0, UINT32_MAX, address)) { invalid(); return; }
  const bool writing = equals(command, "write") || equals(command, "writebyte") || equals(command, "wverify") ||
      equals(command, "fill") || equals(command, "fillverify") || equals(command, "update") || equals(command, "uverify");
  if (writing && _backupValid) { status(Status::Error(Err::BUSY, "restore retained scratch backup before other writes")); return; }
  if (writing && !_platform.nowMs && !_config.nowMs) {
    status(Status::Error(Err::INVALID_CONFIG, "CLI writes require a clock callback for the write-cycle barrier")); return;
  }
  Status result = Status::Error(Err::INVALID_PARAM, "invalid command/arguments; see help");
  if (equals(command, "read") || equals(command, "dump") || equals(command, "hexdump") ||
      equals(command, "readbyte") || equals(command, "text") || equals(command, "crc")) {
    const bool byte = equals(command, "readbyte");
    uint32_t n = byte ? 1 : equals(command, "text") ? 64 : 16;
    if (count > (byte ? 2U : 3U) || (equals(command, "crc") && count != 3) ||
        (count == 3 && !integer(args[2], 1, UINT32_MAX, n))) { invalid(); return; }
    result = startReadView(equals(command, "text") ? ReadView::TEXT : equals(command, "crc") ? ReadView::CRC : ReadView::HEX_DUMP,
                           address, n);
  } else if (equals(command, "fill") || equals(command, "fillverify")) {
    uint32_t n = 0, value = 0;
    if (count != 4 || !integer(args[2], 0, 255, value) || !integer(args[3], 1, UINT32_MAX, n)) { invalid(); return; }
    result = _device.startFill(address, static_cast<uint8_t>(value), n, equals(command, "fillverify"));
  } else if (equals(command, "write") || equals(command, "writebyte") || equals(command, "wverify") || equals(command, "verify") ||
             equals(command, "update") || equals(command, "uverify")) {
    if (count < 3 || (equals(command, "writebyte") && count != 3)) { invalid(); return; }
    for (size_t index = 2; index < count; ++index) {
      uint32_t value = 0;
      if (!integer(args[index], 0, 255, value)) { invalid(); return; }
      _data[index - 2] = static_cast<uint8_t>(value);
    }
    result = equals(command, "verify") ? _device.startVerify(address, _data, count - 2) :
        (equals(command, "update") || equals(command, "uverify")) ? _device.startUpdate(address, _data, count - 2, equals(command, "uverify")) :
        _device.startWrite(address, _data, count - 2, equals(command, "wverify"));
  }
  status(result);
  if (result.ok()) { _operation = true; _hasResult = false; print("Queued; one physical transaction maximum per tick.\n"); }
}
void Cli::tick() {
  _device.tick(now());
  if (_interfaceWait) {
    if (static_cast<int32_t>(now() - _interfaceReadyAt) < 0) return;
    _interfaceWait = false;
  }
  if (_scan) {
    const auto result = _platform.probeAddress(_scanNext, _platform.user);
    if (result.ok()) {
      ++_scanFound;
      print("  Found 0x%02X%s\n", _scanNext, _scanNext >= 0x50 && _scanNext <= 0x57 ? " (EEPROM/bank candidate; identity unverified)" : "");
    } else if (!result.is(EEPROM24Cxx::Err::I2C_NACK_ADDR)) ++_scanErrors;
    if (_scanNext++ == _scanLast) {
      _scan = false; print("Scan complete: %u ACK, %u other transport errors.\n", _scanFound, _scanErrors);
    }
    return;
  }
  if (_stress && !_operation && static_cast<int32_t>(now() - _nextMs) >= 0) {
    const auto result = _device.startRead(_repeatAddress, _data, _repeatLength);
    if (result.ok()) { _operation = true; _hasResult = false; }
    else { status(result); ++_stressFailures; if (_remaining) --_remaining; _nextMs = now() + _repeatInterval; if (!_remaining) stop(); }
  }
  scheduleScratch();
  if (_readView != ReadView::NONE && !_operation) {
    const uint32_t remaining = _readLength - _readCompleted;
    const size_t length = remaining < sizeof(_data) ? remaining : sizeof(_data);
    const auto result = _device.startRead(_readAddress + _readCompleted, _data, length);
    if (result.ok()) _operation = true;
    else {
      EEPROM24Cxx::TransferResult failed{};
      failed.kind = EEPROM24Cxx::TransferKind::READ;
      failed.state = EEPROM24Cxx::TransferState::FAILED;
      failed.status = result;
      failed.failedChunkLength = length;
      completeReadView(failed);
    }
  }
  if (_operation) {
    const auto result = _device.poll(now(), 1);
    if (!result.inProgress()) complete();
  }
}
}  // namespace eeprom24cxx_cli
