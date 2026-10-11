#include "Eeprom24CxxCli.h"
#include "EEPROM24Cxx/MemoryHelpers.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace eeprom24cxx_cli {
namespace {
using namespace EEPROM24Cxx;
bool equals(const char* a, const char* b) { return std::strcmp(a, b) == 0; }
const char* canonicalCommand(const char* command) {
  static constexpr struct { const char* alias; const char* command; } aliases[] = {
    {"?", "help"}, {"ver", "version"},
    {"drv", "health"}, {"state", "health"}, {"online", "health"},
    {"cfg", "settings"}, {"snapshot", "settings"}, {"timing", "geometry"},
    {"status", "progress"}, {"job", "progress"}, {"result", "progress"},
    {"cancel", "stop"}, {"unbind", "end"}, {"init", "begin"},
    {"dump", "read"}, {"hexdump", "read"}, {"cur", "current"},
    {"selfcheck", "selftest"}
  };
  for (const auto& alias : aliases) if (equals(command, alias.alias)) return alias.command;
  return command;
}
bool writesMemory(TransferKind kind) {
  return kind == TransferKind::WRITE || kind == TransferKind::FILL ||
      kind == TransferKind::VERIFIED_WRITE || kind == TransferKind::VERIFIED_FILL ||
      kind == TransferKind::UPDATE || kind == TransferKind::VERIFIED_UPDATE;
}
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
  using namespace EEPROM24Cxx;
  const auto previous = _device.settingsSnapshot();
  if (_configured && (_operation || _scan || _stress || _readView != ReadView::NONE ||
      _scratchStage != ScratchStage::NONE || _backupValid || _interfaceWait ||
      previous.transferActive || previous.resultPending || previous.writeCyclePending)) {
    status(Status::Error(Err::BUSY, "CLI setup cannot replace active work, settling, or retained scratch backup"));
    return;
  }
  // bind validates the replacement before mutating the old driver's context.
  // A failed replacement must not redirect restore buffers or owner clocks.
  const auto bound = _device.bind(config);
  if (_configured && !bound.ok()) { status(bound); return; }
  _platform = platform;
  _config = config;
  _adapterTxLimit = config.maxTxBytes; _adapterRxLimit = config.maxRxBytes;
  _adapterProbe = config.i2cProbe;
  _configured = true;
  _length = 0; _overflow = false;
  _hasResult = false; _lastResult = {};
  _hasScratchPrimaryResult = false; _hasScratchRestoreResult = false;
  _scratchPrimary = {}; _scratchRestore = {};
  _scratchAddress = _scratchLength = _scratchRounds = _scratchRound = 0;
  _scratchReadRound = 0; _scratchStep = 0;
  _scratchChecks = _scratchCheckFailures = _scratchOperationCheckFailures = _scratchCurrentSkipped = 0;
  std::memset(_scratchOperations, 0, sizeof(_scratchOperations));
  _scratchPrimaryElapsedMs = _scratchRestoreElapsedMs = _scratchVerifiedBytes = 0;
  _scratchPrimaryBus = {};
  _jobs = _jobSuccesses = _jobFailures = 0;
  _sessionActive = false; _hasSessionResult = false; _autoPoll = true;
  _sessionId = 0; std::strcpy(_sessionCommand, "none");
  _deadlineMs = 0; _sessionBudgetMs = 0;
  printVersion();
  print("Diagnostic CLI; application owns the bus. Type 'help' for commands.\n");
  print("EEPROM has no standard identity register: select the exact geometry before writing.\n");
  status(bound.ok() ? _device.recover() : bound);
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
  item("job auto [0|1] / job step [0|1]", "Control owner polling; step uses at most one transfer");
  item("job cancel / timeout / result", "End scheduling or inspect the last complete CLI job");
  item("stop / cancel", "Cancel future work; an issued EEPROM write continues");
  section("Configuration");
  item("cfg / settings / snapshot", "Cached staged settings and geometry; no I2C");
  item("bus", "Cached application pins, rate and WP configuration");
  item("timeout [1..1000]", "Set transaction timeout while ended");
  item("limits [tx rx]", "Set chunk limits while ended; cannot exceed adapter limits");
  item("writecycle [0..1000]", "Set tWR while ended; 0=preset, never shorten preset");
  item("ackpoll [0|1]", "Set ACK polling while ended; 0 uses timed waiting");
  item("deadline [ms]", "Whole CLI job deadline; 0=disabled, maximum 2147483647");
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
  item("xfer_demo <addr> <len> confirm", "Admission/budget/owner/result checks, write/read/restore");
  item("stress_mix <addr> <len> N confirm", "1..100 cycles of nine memory operations, then restore");
  item("randbench <addr> <len> N confirm", "1..1000 random writes, independent reads, window check");
  item("typed_demo <addr> confirm", "28-byte unsigned/signed/IEEE/bool round trip, then restore");
  item("scratch / restore confirm", "Show retained backup / explicitly authorize restoration");
  print("Scratch tests consume endurance; failures/cancel retain backup and require explicit restore confirm.\n");
  print("\nNumbers: decimal or 0x hex. Write acceptance is not persistence; WP may silently suppress writes.\n");
  print("Address aliases may belong to one banked chip. ACK cannot identify manufacturer or capacity.\n");
  print("Automation: hilmark <token> echoes an alphanumeric/_/- token (1..32 chars); prompts do not mean job completion.\n");
}
bool Cli::workActive() const {
  return _operation || _scan || _stress || _readView != ReadView::NONE ||
      _scratchStage != ScratchStage::NONE;
}
void Cli::beginSession(const char* command) {
  _sessionActive = true;
  _sessionId = _nextSessionId++;
  if (!_nextSessionId) _nextSessionId = 1;
  std::strncpy(_sessionCommand, command, sizeof(_sessionCommand) - 1);
  _sessionCommand[sizeof(_sessionCommand) - 1] = '\0';
  _sessionStatus = EEPROM24Cxx::Status::Ok();
  _sessionStartedAt = now(); _sessionBudgetMs = _deadlineMs;
  print("CLI accepted: command=%s session=%lu\n", _sessionCommand, static_cast<unsigned long>(_sessionId));
}
void Cli::printJobResult() {
  if (!_hasSessionResult) { status(EEPROM24Cxx::Status::Error(EEPROM24Cxx::Err::NO_RESULT, "no complete CLI job")); return; }
  const auto result = _completedStatus;
  const char* outcome = result.ok() ? "SUCCESS" : result.is(EEPROM24Cxx::Err::CANCELLED) ? "CANCELLED" :
      result.is(EEPROM24Cxx::Err::TIMEOUT) ? "TIMED_OUT" : "FAILED";
  print("CLI result: command=%s session=%lu outcome=%s status=%s code=%u detail=%ld\n",
        _completedCommand, static_cast<unsigned long>(_completedSessionId), outcome,
        EEPROM24Cxx::errorName(result.code), static_cast<unsigned>(result.code), static_cast<long>(result.detail));
}
void Cli::printJob() {
  print("CLI job: active=%u command=%s session=%lu auto=%u\n", _sessionActive ? 1U : 0U,
        _sessionCommand, static_cast<unsigned long>(_sessionId), _autoPoll ? 1U : 0U);
  print("CLI timing: deadline-ms=%lu elapsed-ms=%lu\n", static_cast<unsigned long>(_sessionBudgetMs),
        static_cast<unsigned long>(_sessionId ? now() - _sessionStartedAt : 0));
}
bool Cli::expireSession() {
  if (!_sessionActive || !_sessionBudgetMs || now() - _sessionStartedAt < _sessionBudgetMs) return false;
  stop(EEPROM24Cxx::Status::Error(EEPROM24Cxx::Err::TIMEOUT, "whole CLI job deadline expired"));
  return true;
}
void Cli::finishSession() {
  if (!_sessionActive || workActive()) return;
  if (_sessionStatus.ok() && equals(_sessionCommand, "restore")) _sessionStatus = _scratchRestore;
  else if (_sessionStatus.ok() && (equals(_sessionCommand, "rw_suite") || equals(_sessionCommand, "xfer_demo") ||
           equals(_sessionCommand, "stress_mix") || equals(_sessionCommand, "randbench") ||
           equals(_sessionCommand, "typed_demo"))) {
    if (!_scratchPrimary.ok()) _sessionStatus = _scratchPrimary;
    else if (!_scratchRestore.ok()) _sessionStatus = _scratchRestore;
  }
  _sessionActive = false;
  _completedSessionId = _sessionId;
  std::memcpy(_completedCommand, _sessionCommand, sizeof(_completedCommand));
  _completedStatus = _sessionStatus;
  _hasSessionResult = true;
  printJobResult();
}
void Cli::printSettings() {
  print("Staged: model=%s address=0x%02X timeout=%lu ms tx=%lu rx=%lu offline-threshold=%u\n",
        modelName(_config.variant), _config.i2cAddress, static_cast<unsigned long>(_config.i2cTimeoutMs),
        static_cast<unsigned long>(_config.maxTxBytes), static_cast<unsigned long>(_config.maxRxBytes),
        static_cast<unsigned>(_config.offlineThreshold));
  print("Staged protocol: ack-polling=%s write-cycle-override=%lu ms adapter-tx=%lu adapter-rx=%lu cli-deadline=%lu ms\n",
        _config.i2cProbe ? "yes" : "no", static_cast<unsigned long>(_config.writeCycleMs),
        static_cast<unsigned long>(_adapterTxLimit), static_cast<unsigned long>(_adapterRxLimit),
        static_cast<unsigned long>(_deadlineMs));
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
void Cli::printBus() {
  print("Bus config: sda=%d scl=%d frequency-hz=%lu wp-pin=%d\n", _platform.sdaPin,
        _platform.sclPin, static_cast<unsigned long>(_platform.frequencyHz), _platform.writeProtectPin);
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
const char* Cli::scratchModeName() const {
  switch (_scratchMode) {
    case ScratchMode::RW_SUITE: return "rw_suite";
    case ScratchMode::MIX: return "stress_mix";
    case ScratchMode::RANDOM: return "randbench";
    case ScratchMode::TYPED: return "typed_demo";
    case ScratchMode::XFER: return "xfer_demo";
  }
  return "unknown";
}
bool Cli::scratchCheck(const char* name, bool passed) {
  ++_scratchChecks;
  if (!passed) ++_scratchCheckFailures;
  print("Scratch check: name=%s result=%s\n", name, passed ? "PASS" : "FAIL");
  return passed;
}
bool Cli::checkScratchBounds() {
  using namespace EEPROM24Cxx;
  const auto before = _platform.transferStats ? _platform.transferStats(_platform.user).attempts : 0;
  const auto read = _device.startRead(_device.capacityBytes(), _data, 1);
  const auto write = _device.startWrite(_device.maxAddress(), _scratchExpected, 2);
  const auto verify = _device.startVerify(_device.maxAddress(), _scratchExpected, 2);
  const auto after = _platform.transferStats ? _platform.transferStats(_platform.user).attempts : 0;
  const bool rejected = scratchCheck(_platform.transferStats ? "out-of-range-no-transfer" : "out-of-range-rejected",
      read.is(Err::ADDRESS_OUT_OF_RANGE) &&
      write.is(Err::ADDRESS_OUT_OF_RANGE) && verify.is(Err::ADDRESS_OUT_OF_RANGE) && before == after);
  if (!rejected && (_device.isTransferBusy() || _device.settingsSnapshot().resultPending)) {
    // A broken admission guard must not leave a diagnostic write queued.
    (void)_device.cancel();
    TransferResult discarded{};
    (void)_device.takeResult(discarded);
  }
  return rejected;
}
void Cli::checkScratchAdmission() {
  using namespace EEPROM24Cxx;
  const auto before = _scratchAdmissionAttempts;
  const auto active = _device.transferSnapshot();
  const uint32_t stale = active.requestId == 1 ? 2 : 1;
  scratchCheck("zero-budget", _device.pollTransfer(active.requestId, now(), 0).inProgress() &&
      _device.transferSnapshot().bytesCompleted == 0);
  scratchCheck("busy-admission", _device.startRead(_scratchAddress, _data, 1).is(Err::BUSY));
  scratchCheck("stale-owner", _device.pollTransfer(stale, now(), 1).is(Err::BUSY) &&
      _device.cancelTransfer(stale).is(Err::BUSY));
  const auto after = _platform.transferStats ? _platform.transferStats(_platform.user).attempts : 0;
  scratchCheck("admission-no-transfer", before == after && _device.transferSnapshot().requestId == active.requestId);
  if (_scratchCheckFailures != _scratchOperationCheckFailures) (void)_device.cancel();
}
void Cli::checkScratchRetainedResult() {
  using namespace EEPROM24Cxx;
  const auto before = _platform.transferStats ? _platform.transferStats(_platform.user).attempts : 0;
  const auto retained = _device.transferSnapshot();
  TransferResult untouched{};
  untouched.requestId = 123;
  const uint32_t stale = retained.requestId == 1 ? 2 : 1;
  scratchCheck("retained-blocks-admission", _device.startRead(_scratchAddress, _data, 1).is(Err::BUSY));
  scratchCheck("retained-owner", _device.takeTransferResult(stale, untouched).is(Err::BUSY) && untouched.requestId == 123);
  TransferResult copy{};
  const auto after = _platform.transferStats ? _platform.transferStats(_platform.user).attempts : 0;
  scratchCheck("retained-no-transfer", _device.getTransferProgress(copy).ok() && copy.requestId == retained.requestId &&
      copy.status.code == retained.status.code && before == after);
}
void Cli::printScratch(bool includeResults) {
  const char* stage = "IDLE";
  switch (_scratchStage) {
    case ScratchStage::NONE: break;
    case ScratchStage::BACKUP: stage = "BACKUP"; break;
    case ScratchStage::PATTERN: stage = "PATTERN"; break;
    case ScratchStage::RANDOM_READS: stage = "RANDOM_READS"; break;
    case ScratchStage::INSPECT: stage = "INSPECT"; break;
    case ScratchStage::RESTORE_WAIT: stage = "RESTORE_WAIT"; break;
    case ScratchStage::RESTORING: stage = "RESTORING"; break;
  }
  print("Scratch: stage=%s address=0x%05lX length=%lu rounds=%lu/%lu backup=%s restore-required=%s primary=%s detail=%ld restore=%s detail=%ld\n",
        stage, static_cast<unsigned long>(_scratchAddress),
        static_cast<unsigned long>(_scratchLength), static_cast<unsigned long>(_scratchRound),
        static_cast<unsigned long>(_scratchRounds), _backupValid ? "retained" : "none", _scratchDirty ? "yes" : "no",
        EEPROM24Cxx::errorName(_scratchPrimary.code), static_cast<long>(_scratchPrimary.detail),
        EEPROM24Cxx::errorName(_scratchRestore.code), static_cast<long>(_scratchRestore.detail));
  print("Scratch suite: mode=%s checks=%lu failures=%lu\n", scratchModeName(),
        static_cast<unsigned long>(_scratchChecks), static_cast<unsigned long>(_scratchCheckFailures));
  print("Scratch operations: reads=%lu writes=%lu fills=%lu updates=%lu verifies=%lu probes=%lu current=%lu current-skipped=%lu\n",
        static_cast<unsigned long>(_scratchOperations[0]), static_cast<unsigned long>(_scratchOperations[1]),
        static_cast<unsigned long>(_scratchOperations[2]), static_cast<unsigned long>(_scratchOperations[3]),
        static_cast<unsigned long>(_scratchOperations[4]), static_cast<unsigned long>(_scratchOperations[5]),
        static_cast<unsigned long>(_scratchOperations[6]), static_cast<unsigned long>(_scratchCurrentSkipped));
  if (_backupValid && _scratchStage == ScratchStage::NONE)
    print("Backup retained in RAM; use restore confirm when ready. End/cancel never restore or discard changed data.\n");
  print("Scratch metrics: primary-ms=%lu restore-ms=%lu verified-pattern-bytes=%lu",
        static_cast<unsigned long>(_scratchPrimaryElapsedMs), static_cast<unsigned long>(_scratchRestoreElapsedMs),
        static_cast<unsigned long>(_scratchVerifiedBytes));
  if (_platform.transferStats)
    print(" primary-read=%lu primary-write=%lu primary-probe=%lu", static_cast<unsigned long>(_scratchPrimaryBus.readAttempts),
          static_cast<unsigned long>(_scratchPrimaryBus.writeAttempts), static_cast<unsigned long>(_scratchPrimaryBus.probeAttempts));
  print(" (primary timing includes backup)\n");
  // These are the last terminal core stages, not aggregate progress across
  // pattern rounds. Keep their effects independent of later diagnostics and
  // of the separately authorized restoration attempt.
  if (includeResults && _hasScratchPrimaryResult) {
    print("Last primary stage (retained independently of restoration):\n");
    printProgress(_scratchPrimaryResult);
  }
  if (includeResults && _hasScratchRestoreResult) {
    print("Last restoration stage:\n");
    printProgress(_scratchRestoreResult);
  }
}
EEPROM24Cxx::Status Cli::startScratch(ScratchMode mode, uint32_t address, uint32_t length, uint32_t rounds) {
  using namespace EEPROM24Cxx;
  if (!_device.isBound()) return Status::Error(Err::NOT_INITIALIZED, "bind driver first");
  if (_backupValid) return Status::Error(Err::BUSY, "restore retained backup first");
  if (mode == ScratchMode::XFER && !_platform.transferStats)
    return Status::Error(Err::UNSUPPORTED, "transfer contract checks require application bus counters");
  const auto idle = _device.settingsSnapshot();
  if (idle.transferActive || idle.resultPending || idle.writeCyclePending)
    return Status::Error(Err::BUSY, "scratch checks require an idle, settled driver");
  if (!_platform.nowMs && !_config.nowMs) return Status::Error(Err::INVALID_CONFIG, "scratch writes require a clock");
  if (!length || length > sizeof(_backup) || !memory::fitsRange(_device.capacityBytes(), address, length))
    return Status::Error(Err::ADDRESS_OUT_OF_RANGE, "scratch range must fit configured capacity and 256-byte backup");
  _scratchMode = mode; _scratchAddress = address; _scratchLength = length;
  _scratchRounds = mode == ScratchMode::RW_SUITE ? 8U : mode == ScratchMode::XFER ? 4U :
      mode == ScratchMode::TYPED ? 2U : rounds;
  _scratchRound = 0; _scratchReadRound = 0; _scratchStep = 0; _scratchRandom = 0x24C02B01U;
  _scratchChecks = _scratchCheckFailures = _scratchCurrentSkipped = 0;
  std::memset(_scratchOperations, 0, sizeof(_scratchOperations));
  _scratchDirty = false; _scratchStage = ScratchStage::NONE;
  _scratchStartedMs = now(); _scratchPrimaryElapsedMs = 0; _scratchRestoreElapsedMs = 0; _scratchVerifiedBytes = 0;
  _scratchBusBefore = _platform.transferStats ? _platform.transferStats(_platform.user) : TransferStats{};
  _scratchPrimaryBus = {};
  _scratchPrimary = Status::Error(Err::IN_PROGRESS, "scratch test queued");
  _hasScratchPrimaryResult = false; _hasScratchRestoreResult = false;
  _scratchRestore = Status::Error(Err::NO_RESULT, "restoration not attempted");
  if (mode == ScratchMode::TYPED && length != 28)
    return Status::Error(Err::INVALID_PARAM, "typed layout requires 28 bytes");
  if (!checkScratchBounds()) {
    finishScratchPrimary(Status::Error(Err::VERIFY_MISMATCH, "out-of-range operation was not rejected without I2C"));
    return _scratchPrimary;
  }
  _scratchAdmissionAttempts = _platform.transferStats ? _platform.transferStats(_platform.user).attempts : 0;
  const auto result = _device.startRead(address, _backup, length);
  if (!result.ok()) { finishScratchPrimary(result); return result; }
  _scratchStage = ScratchStage::BACKUP;
  _scratchAction = ScratchAction::READ; _scratchCheckName = "backup";
  ++_scratchOperations[static_cast<unsigned>(ScratchAction::READ)];
  _operation = true; _hasResult = false;
  _scratchOperationCheckFailures = _scratchCheckFailures;
  if (mode == ScratchMode::XFER) checkScratchAdmission();
  print("Scratch test queued: mode=%s backup, %lu rounds, independent readback, verified restore; EEPROM endurance is consumed.\n",
        scratchModeName(), static_cast<unsigned long>(_scratchRounds));
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
  printScratch(false);
}
void Cli::completeScratch(const EEPROM24Cxx::TransferResult& result) {
  using namespace EEPROM24Cxx;
  if (_scratchStage == ScratchStage::RESTORING) {
    _scratchRestoreResult = result; _hasScratchRestoreResult = true;
  } else {
    _scratchPrimaryResult = result; _hasScratchPrimaryResult = true;
  }
  status(result.status); printProgress(result);
  if (_scratchMode == ScratchMode::XFER) {
    TransferResult consumed{};
    scratchCheck("exactly-once-result", _device.takeResult(consumed).is(Err::NO_RESULT));
  }
  if (_scratchStage != ScratchStage::RESTORING) _scratchVerifiedBytes += static_cast<uint32_t>(result.bytesVerified);
  if (!result.status.ok()) { abortScratch(result.status); return; }
  if (_scratchCheckFailures != _scratchOperationCheckFailures) { abortScratch(Status::Error(Err::VERIFY_MISMATCH, "scratch contract check failed")); return; }
  if (_scratchStage == ScratchStage::BACKUP) {
    _backupValid = true;
    std::memcpy(_scratchExpected, _backup, _scratchLength);
    if (!scratchCheck("backup", result.bytesCompleted == _scratchLength)) {
      abortScratch(Status::Error(Err::VERIFY_MISMATCH, "scratch backup is incomplete")); return;
    }
    _scratchStage = ScratchStage::PATTERN;
    return;
  }
  if (_scratchStage == ScratchStage::RESTORING) {
    _scratchRestore = Status::Ok();
    _scratchRestoreElapsedMs = now() - _scratchRestoreStartedMs;
    _backupValid = false; _scratchDirty = false; _scratchStage = ScratchStage::NONE;
    printScratch(false);
    print("Original scratch bytes observed by readback after restoration.\n");
    return;
  }
  bool correct = result.bytesCompleted == result.bytesRequested;
  if (_scratchAction == ScratchAction::READ || _scratchAction == ScratchAction::CURRENT)
    correct = correct && std::memcmp(_data, _scratchExpected + _scratchReadOffset, _scratchReadLength) == 0;
  else if (_scratchAction == ScratchAction::WRITE || _scratchAction == ScratchAction::FILL || _scratchAction == ScratchAction::UPDATE)
    correct = correct && result.bytesVerified == result.bytesRequested;
  else if (_scratchAction == ScratchAction::VERIFY)
    correct = correct && result.match && result.bytesVerified == result.bytesRequested;
  if (_scratchAction == ScratchAction::UPDATE && std::strcmp(_scratchCheckName, "unchanged-update") == 0)
    correct = correct && result.bytesAccepted == 0 && result.bytesSkipped == result.bytesRequested;
  if (!scratchCheck(_scratchCheckName, correct)) {
    // A later restoration must not hide a failed independent data comparison.
    _scratchPrimaryResult.status = Status::Error(Err::VERIFY_MISMATCH, "scratch readback or operation evidence differs");
    _scratchPrimaryResult.state = TransferState::FAILED;
    abortScratch(_scratchPrimaryResult.status); return;
  }
  if (_scratchMode == ScratchMode::TYPED && _scratchStage == ScratchStage::PATTERN && _scratchAction == ScratchAction::READ) {
    if (_scratchRound == 0) {
      const bool decoded = _data[0] == 0x7E && memory::decodeUint16Le(_data + 1) == 0x1234U &&
          memory::decodeInt32Le(_data + 3) == -1234567 &&
          memory::decodeUint64Le(_data + 7) == UINT64_C(0x1122334455667788) &&
          memory::decodeFloatLe(_data + 15) == 1.25F && memory::decodeDoubleLe(_data + 19) == -42.5 && _data[27] == 1;
      print("Typed values: uint8=0x%02X uint16=0x%04X int32=%ld uint64=0x%08lX%08lX float32=%.2f float64=%.2f bool=%s\n",
            static_cast<unsigned>(_data[0]), static_cast<unsigned>(memory::decodeUint16Le(_data + 1)),
            static_cast<long>(memory::decodeInt32Le(_data + 3)),
            static_cast<unsigned long>(memory::decodeUint64Le(_data + 7) >> 32U),
            static_cast<unsigned long>(memory::decodeUint64Le(_data + 7) & UINT32_MAX),
            static_cast<double>(memory::decodeFloatLe(_data + 15)), memory::decodeDoubleLe(_data + 19), _data[27] == 1 ? "true" : "false");
      if (!scratchCheck("typed-le-values", decoded)) { abortScratch(Status::Error(Err::VERIFY_MISMATCH, "typed decode differs")); return; }
    } else {
      const bool decoded = memory::decodeUint16Be(_data) == 0xA55A && memory::decodeUint32Be(_data + 2) == 0x12345678U &&
          memory::decodeUint64Be(_data + 6) == UINT64_C(0x0123456789ABCDEF) &&
          memory::decodeInt64Le(_data + 14) == -INT64_C(0x0123456789ABCDEF) &&
          memory::decodeUint32Le(_data + 22) == 0xCAFEBABEU && _data[26] == 0 && _data[27] == 0x81;
      print("Typed values: uint16be=0x%04X uint32be=0x%08lX uint64be=0x%08lX%08lX int64le=%s uint32le=0x%08lX bool=%s uint8=0x%02X\n",
            static_cast<unsigned>(memory::decodeUint16Be(_data)), static_cast<unsigned long>(memory::decodeUint32Be(_data + 2)),
            static_cast<unsigned long>(memory::decodeUint64Be(_data + 6) >> 32U),
            static_cast<unsigned long>(memory::decodeUint64Be(_data + 6) & UINT32_MAX),
            memory::decodeInt64Le(_data + 14) == -INT64_C(0x0123456789ABCDEF) ? "match" : "mismatch",
            static_cast<unsigned long>(memory::decodeUint32Le(_data + 22)), _data[26] == 0 ? "false" : "invalid",
            static_cast<unsigned>(_data[27]));
      if (!scratchCheck("typed-be-signed-values", decoded)) { abortScratch(Status::Error(Err::VERIFY_MISMATCH, "typed decode differs")); return; }
    }
  }
  if (_scratchStage == ScratchStage::INSPECT) {
    finishScratchPrimary(Status::Ok());
    _scratchRestoreStartedMs = now();
    _scratchRestore = Status::Error(Err::IN_PROGRESS, "authorized restoration queued");
    _scratchStage = ScratchStage::RESTORE_WAIT;
    return;
  }
  if (_scratchStage == ScratchStage::RANDOM_READS) {
    if (++_scratchReadRound == _scratchRounds) _scratchStage = ScratchStage::INSPECT;
    return;
  }
  if (_scratchMode == ScratchMode::MIX) {
    if (++_scratchStep < 9) return;
    _scratchStep = 0;
  } else if (_scratchMode == ScratchMode::TYPED) {
    if (++_scratchStep < 2) return;
    _scratchStep = 0;
  }
  if (++_scratchRound == _scratchRounds) {
    _scratchStage = _scratchMode == ScratchMode::RANDOM ? ScratchStage::RANDOM_READS : ScratchStage::INSPECT;
    _scratchRandom = 0x13579BDFU; // Independent read addresses, not the write sequence.
  }
}
void Cli::scheduleScratch() {
  using namespace EEPROM24Cxx;
  if (_scratchStage == ScratchStage::NONE || _operation) return;
  if (_device.settingsSnapshot().writeCyclePending) return;
  Status result = Status::Error(Err::INVALID_PARAM, "invalid scratch stage");
  _scratchAdmissionAttempts = _platform.transferStats ? _platform.transferStats(_platform.user).attempts : 0;
  if (_scratchStage == ScratchStage::RESTORE_WAIT) {
    result = _device.startWrite(_scratchAddress, _backup, _scratchLength, true);
    if (result.ok()) _scratchStage = ScratchStage::RESTORING;
  } else {
    uint32_t offset = 0, length = _scratchLength;
    _scratchAction = ScratchAction::READ;
    _scratchCheckName = "final-window";
    const auto random = [this]() {
      _scratchRandom ^= _scratchRandom << 13U;
      _scratchRandom ^= _scratchRandom >> 17U;
      _scratchRandom ^= _scratchRandom << 5U;
      return _scratchRandom;
    };
    if (_scratchStage == ScratchStage::RANDOM_READS) {
      offset = random() % length; length = 1;
      _scratchCheckName = "random-read";
    } else if (_scratchStage == ScratchStage::PATTERN) {
      if (_scratchMode == ScratchMode::RANDOM) {
        offset = random() % length; length = 1;
        // A changed value ensures the test does not pass solely on preexisting bytes.
        _scratchExpected[offset] ^= static_cast<uint8_t>((random() & 0x7FU) + 1U);
        _scratchAction = ScratchAction::WRITE; _scratchCheckName = "random-write";
      } else if (_scratchMode == ScratchMode::TYPED && _scratchStep == 1) {
        _scratchCheckName = "typed-read";
      } else if (_scratchMode == ScratchMode::TYPED && _scratchRound == 0) {
        _scratchExpected[0] = 0x7E;
        memory::encodeUint16Le(0x1234U, _scratchExpected + 1);
        memory::encodeInt32Le(-1234567, _scratchExpected + 3);
        memory::encodeUint64Le(UINT64_C(0x1122334455667788), _scratchExpected + 7);
        memory::encodeFloatLe(1.25F, _scratchExpected + 15);
        memory::encodeDoubleLe(-42.5, _scratchExpected + 19);
        _scratchExpected[27] = 1;
        _scratchAction = ScratchAction::WRITE; _scratchCheckName = "typed-write";
        print("Typed layout: uint8 @+0, uint16 LE @+1, int32 LE @+3, uint64 LE @+7, float32 LE @+15, float64 LE @+19, bool @+27; 28 bytes.\n");
      } else if (_scratchMode == ScratchMode::TYPED) {
        memory::encodeUint16Be(0xA55A, _scratchExpected);
        memory::encodeUint32Be(0x12345678U, _scratchExpected + 2);
        memory::encodeUint64Be(UINT64_C(0x0123456789ABCDEF), _scratchExpected + 6);
        memory::encodeInt64Le(-INT64_C(0x0123456789ABCDEF), _scratchExpected + 14);
        memory::encodeUint32Le(0xCAFEBABEU, _scratchExpected + 22);
        _scratchExpected[26] = 0; _scratchExpected[27] = 0x81;
        _scratchAction = ScratchAction::WRITE; _scratchCheckName = "typed-write";
        print("Typed layout: uint16 BE @+0, uint32 BE @+2, uint64 BE @+6, int64 LE @+14, uint32 LE @+22, bool @+26, uint8 @+27; 28 bytes.\n");
      } else if (_scratchMode == ScratchMode::MIX) {
        switch (_scratchStep) {
          case 0:
            offset = _scratchRound % length; length = 1; _scratchExpected[offset] ^= 0xFF;
            _scratchAction = ScratchAction::WRITE; _scratchCheckName = "mixed-byte-write"; break;
          case 1: _scratchCheckName = "mixed-read"; break;
          case 2:
            std::memset(_scratchExpected, (_scratchRound & 1U) ? 0x5A : 0xA5, length);
            _scratchAction = ScratchAction::FILL; _scratchCheckName = "mixed-fill"; break;
          case 3: _scratchAction = ScratchAction::VERIFY; _scratchCheckName = "mixed-verify"; break;
          case 4:
            _scratchExpected[0] ^= 0xFF;
            _scratchAction = ScratchAction::UPDATE; _scratchCheckName = "changed-update"; break;
          case 5: _scratchAction = ScratchAction::UPDATE; _scratchCheckName = "unchanged-update"; break;
          case 6: _scratchAction = ScratchAction::PROBE; _scratchCheckName = "presence-probe"; break;
          case 7: length = 1; _scratchCheckName = "current-pointer-seed"; break;
          default: {
            const auto pointer = _device.settingsSnapshot();
            length = 1;
            if (pointer.supportsCurrentAddressRead && pointer.currentAddressKnown &&
                pointer.currentAddress >= _scratchAddress && pointer.currentAddress - _scratchAddress < _scratchLength) {
              offset = pointer.currentAddress - _scratchAddress;
              _scratchAction = ScratchAction::CURRENT; _scratchCheckName = "current-read";
            } else {
              ++_scratchCurrentSkipped;
              _scratchCheckName = "current-fallback-read";
              print("Scratch current-read: SKIP (transport capability or pointer outside scratch); addressed read used.\n");
            }
            break;
          }
        }
      } else {
        // Both suites share real read/write/fill/verify operations. rw_suite
        // additionally checks changed and wear-saving unchanged updates.
        switch (_scratchRound) {
          case 0:
            for (uint32_t index = 0; index < length; ++index) _scratchExpected[index] = static_cast<uint8_t>(~_backup[index]);
            _scratchAction = ScratchAction::WRITE; _scratchCheckName = "pattern-write"; break;
          case 1: _scratchCheckName = "pattern-read"; break;
          case 2:
            std::memset(_scratchExpected, 0x5A, length);
            _scratchAction = ScratchAction::FILL; _scratchCheckName = "fill-5a"; break;
          case 3: _scratchAction = ScratchAction::VERIFY; _scratchCheckName = "pattern-verify"; break;
          case 4:
            std::memset(_scratchExpected, 0xA5, length);
            _scratchAction = ScratchAction::FILL; _scratchCheckName = "fill-a5"; break;
          case 5:
            std::memset(_scratchExpected, 0x3C, length);
            _scratchAction = ScratchAction::UPDATE; _scratchCheckName = "changed-update"; break;
          case 6: _scratchAction = ScratchAction::UPDATE; _scratchCheckName = "unchanged-update"; break;
          default: _scratchCheckName = "update-read"; break;
        }
      }
    }
    _scratchReadOffset = offset; _scratchReadLength = length;
    const uint32_t address = _scratchAddress + offset;
    switch (_scratchAction) {
      case ScratchAction::READ: result = _device.startRead(address, _data, length); break;
      case ScratchAction::WRITE: result = _device.startWrite(address, _scratchExpected + offset, length, true); break;
      case ScratchAction::FILL: result = _device.startFill(address, _scratchExpected[offset], length, true); break;
      case ScratchAction::UPDATE: result = _device.startUpdate(address, _scratchExpected + offset, length, true); break;
      case ScratchAction::VERIFY: result = _device.startVerify(address, _scratchExpected + offset, length); break;
      case ScratchAction::CURRENT: result = _device.startCurrentRead(_data, length); break;
      case ScratchAction::PROBE:
        // This step alone owns this tick's transaction; do not queue another.
        result = _device.probe();
        ++_scratchOperations[static_cast<unsigned>(ScratchAction::PROBE)];
        status(result);
        scratchCheck(_scratchCheckName, result.ok());
        if (!result.ok()) abortScratch(result); else ++_scratchStep;
        return;
    }
    if (result.ok()) {
      ++_scratchOperations[static_cast<unsigned>(_scratchAction)];
      if (_scratchAction == ScratchAction::WRITE || _scratchAction == ScratchAction::FILL || _scratchAction == ScratchAction::UPDATE)
        _scratchDirty = true;
    }
  }
  if (result.ok()) {
    _operation = true; _hasResult = false;
    _scratchOperationCheckFailures = _scratchCheckFailures;
    if (_scratchMode == ScratchMode::XFER) checkScratchAdmission();
  }
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
  if (writesMemory(result.kind))
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
  if (!length || !memory::fitsRange(capacity, address, length))
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
  if (!chunk.status.ok() && _sessionStatus.ok()) _sessionStatus = chunk.status;
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
  if (_scratchMode == ScratchMode::XFER && _scratchStage != ScratchStage::NONE && !_device.isTransferBusy())
    checkScratchRetainedResult();
  if (!_device.takeResult(result).ok()) return;
  if (!result.status.ok() && _sessionStatus.ok()) _sessionStatus = result.status;
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
    if (!_remaining) {
      print("%s stopped: ok=%lu fail=%lu\n", _watch ? "Watch" : "Stress", static_cast<unsigned long>(_stressSuccess),
            static_cast<unsigned long>(_stressFailures));
      _stress = false; _watch = false;
    }
  }
}
void Cli::stop(EEPROM24Cxx::Status reason) {
  if (_sessionActive) _sessionStatus = reason;
  const bool wasStress = _stress;
  const bool wasWatch = _watch;
  _stress = false;
  _remaining = 0;
  _scan = false;
  if (_operation) {
    const auto kind = _device.transferSnapshot().kind;
    if (_device.isTransferBusy()) {
      const auto cancelled = reason.is(EEPROM24Cxx::Err::TIMEOUT) ?
          _device.timeoutTransfer(_device.transferSnapshot().requestId) : _device.cancel();
      if (!cancelled.ok()) status(cancelled);
    }
    complete();
    if (writesMemory(kind))
      print("Scheduling stopped; already-issued EEPROM programming cannot be undone.\n");
  } else if (_readView != ReadView::NONE) {
    EEPROM24Cxx::TransferResult cancelled{};
    cancelled.kind = EEPROM24Cxx::TransferKind::READ;
    cancelled.state = reason.is(EEPROM24Cxx::Err::TIMEOUT) ? EEPROM24Cxx::TransferState::TIMED_OUT : EEPROM24Cxx::TransferState::CANCELLED;
    cancelled.status = reason;
    completeReadView(cancelled);
  }
  if (_scratchStage != ScratchStage::NONE)
    abortScratch(reason);
  if (wasStress) print("%s stopped: ok=%lu fail=%lu\n", wasWatch ? "Watch" : "Stress", static_cast<unsigned long>(_stressSuccess),
                       static_cast<unsigned long>(_stressFailures));
  _watch = false;
  finishSession();
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
  const char* command = canonicalCommand(args[0]);
  if (equals(command, "hilmark")) {
    if (count != 2 || std::strlen(args[1]) > 32) { invalid(); return; }
    for (const char* value = args[1]; *value; ++value)
      if (!((*value >= 'a' && *value <= 'z') || (*value >= 'A' && *value <= 'Z') ||
            (*value >= '0' && *value <= '9') || *value == '_' || *value == '-')) { invalid(); return; }
    print("CLI marker: token=%s\n", args[1]); return;
  }
  if (equals(args[0], "result") && count == 1) printJobResult();
  if (equals(args[0], "job") && count > 1) {
    const char* action = args[1];
    if (equals(action, "result") && count <= 2) { printJobResult(); return; }
    if (equals(action, "auto")) {
      uint32_t enabled = _autoPoll ? 1U : 0U;
      if (count > 3 || (count == 3 && !integer(args[2], 0, 1, enabled))) { invalid(); return; }
      _autoPoll = enabled != 0; printJob(); return;
    }
    if (equals(action, "step")) {
      uint32_t budget = 1;
      if (count > 3 || (count == 3 && !integer(args[2], 0, 1, budget))) { invalid(); return; }
      if (budget) advance(); else _device.tick(now());
      if (!expireSession()) finishSession();
      printJob(); return;
    }
    if ((equals(action, "cancel") || equals(action, "timeout")) && count == 2) {
      if (!_sessionActive) { status(Status::Error(Err::NO_RESULT, "no active CLI job")); return; }
      stop(Status::Error(equals(action, "timeout") ? Err::TIMEOUT : Err::CANCELLED,
                        "owner ended CLI job")); return;
    }
    invalid(); return;
  }
  if (equals(command, "help") && count == 1) { printHelp(); return; }
  if (equals(command, "version") && count == 1) { printVersion(); return; }
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
  if (equals(command, "stop") && count == 1) { stop(); return; }
  if (equals(command, "health") && count == 1) { printHealth(); return; }
  if (equals(command, "settings") && count == 1) { printSettings(); return; }
  if (equals(command, "bus") && count == 1) { printBus(); return; }
  if (equals(command, "deadline")) {
    uint32_t value = _deadlineMs;
    if (count > 2 || (count == 2 && !integer(args[1], 0, 0x7FFFFFFFU, value))) { invalid(); return; }
    if (count == 2) {
      if (workActive()) { status(Status::Error(Err::BUSY, "finish CLI job before changing its deadline")); return; }
      if (value && !_platform.nowMs && !_config.nowMs) { status(Status::Error(Err::INVALID_CONFIG, "CLI deadline requires a clock")); return; }
      _deadlineMs = value;
    }
    print("Deadline: ms=%lu (whole CLI job; 0=disabled)\n", static_cast<unsigned long>(_deadlineMs)); return;
  }
  if (equals(command, "geometry") && count == 1) { printSettings(); printHealth(); return; }
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
  if ((equals(command, "addr") || equals(command, "timeout") || equals(command, "model") || equals(command, "offline") ||
       equals(command, "limits") || equals(command, "writecycle") || equals(command, "ackpoll")) && count == 1) {
    printSettings();
    if (equals(command, "model")) printModels();
    return;
  }
  if (equals(command, "progress") && count == 1) {
    printJob();
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
  if (equals(command, "diag") && count == 1) { printVersion(); printBus(); printSettings(); printHealth(); return; }
  if (equals(command, "end") && count == 1) {
    stop(); _device.end(); print("Driver released; bus unchanged.\n"); return;
  }
  if (_operation || _scan || _stress || _readView != ReadView::NONE || _scratchStage != ScratchStage::NONE || _interfaceWait) { status(Status::Error(Err::BUSY, "work active or bus recovery settling")); return; }
  if ((equals(command, "scan") || equals(command, "discover")) && count == 1) {
    if (_device.settingsSnapshot().writeCyclePending) { status(Status::Error(Err::BUSY, "wait for physical write cycle before scanning")); return; }
    if (!_platform.probeAddress) { status(Status::Error(Err::INVALID_CONFIG, "no scan adapter")); return; }
    _scan = true; _scanNext = equals(command, "scan") ? 0x08 : 0x50;
    _scanLast = equals(command, "scan") ? 0x77 : 0x57; _scanFound = 0; _scanErrors = 0;
    beginSession(command); return;
  }
  if (equals(command, "begin")) {
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
  if (equals(command, "limits") || equals(command, "writecycle") || equals(command, "ackpoll")) {
    if (count != (equals(command, "limits") ? 3U : 2U)) { invalid(); return; }
    if (_backupValid || _device.isBound() || _device.settingsSnapshot().writeCyclePending) {
      status(Status::Error(Err::BUSY, "end and settle driver without a retained backup before changing protocol settings")); return;
    }
    const auto geometry = _config.variant == DeviceVariant::CUSTOM ? _config.customGeometry : geometryFor(_config.variant);
    uint32_t first = 0, second = 0;
    if (equals(command, "limits")) {
      if (!integer(args[1], static_cast<uint32_t>(geometry.wordAddressBytes) + 1U, UINT32_MAX, first) ||
          !integer(args[2], 1, UINT32_MAX, second) || first > _adapterTxLimit || second > _adapterRxLimit) { invalid(); return; }
      _config.maxTxBytes = first; _config.maxRxBytes = second;
    } else if (equals(command, "writecycle")) {
      if (!integer(args[1], 0, 1000, first) || (first && first < geometry.writeCycleMs)) { invalid(); return; }
      _config.writeCycleMs = first;
    } else {
      if (!integer(args[1], 0, 1, first)) { invalid(); return; }
      if (first && !_adapterProbe) { status(Status::Error(Err::UNSUPPORTED, "application has no ACK-poll callback")); return; }
      _config.i2cProbe = first ? _adapterProbe : nullptr;
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
    beginSession(command);
    print("Verified restoration queued; physical write barrier remains enforced.\n"); return;
  }
  if (equals(command, "rw_suite") || equals(command, "xfer_demo") || equals(command, "stress_mix") ||
      equals(command, "randbench") || equals(command, "typed_demo")) {
    const bool typed = equals(command, "typed_demo");
    const bool repeated = equals(command, "stress_mix") || equals(command, "randbench");
    const size_t expected = typed ? 3U : repeated ? 5U : 4U;
    uint32_t address = 0, n = typed ? 28 : 0, rounds = equals(command, "rw_suite") ? 4 : 1;
    if (count != expected || !equals(args[count - 1], "confirm") || !integer(args[1], 0, UINT32_MAX, address) ||
        (!typed && !integer(args[2], 1, sizeof(_backup), n)) ||
        (repeated && !integer(args[3], 1, equals(command, "randbench") ? 1000U : 100U, rounds))) { invalid(); return; }
    const auto mode = typed ? ScratchMode::TYPED : equals(command, "stress_mix") ? ScratchMode::MIX :
        equals(command, "randbench") ? ScratchMode::RANDOM : equals(command, "xfer_demo") ? ScratchMode::XFER : ScratchMode::RW_SUITE;
    const auto result = startScratch(mode, address, n, rounds);
    status(result); if (result.ok()) beginSession(command); return;
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
    beginSession(command);
    print("Read-only %s: %lu rounds, %lu ms interval; stop cancels.\n", watch ? "watch" : "stress",
          static_cast<unsigned long>(rounds), static_cast<unsigned long>(interval)); return;
  }
  if (equals(command, "selftest") && count == 1) {
    const auto capacity = _device.settingsSnapshot().capacityBytes;
    const auto result = startReadView(ReadView::SELFTEST, 0, capacity);
    status(result);
    if (result.ok()) { _operation = true; _hasResult = false; beginSession(command); printSettings(); print("Queued whole-array read-only selftest; one transaction maximum per tick.\n"); }
    return;
  }
  if (equals(command, "strings")) {
    uint32_t address = 0, n = _device.settingsSnapshot().capacityBytes, minimum = 4;
    if ((count != 1 && count != 3 && count != 4) ||
        (count >= 3 && (!integer(args[1], 0, UINT32_MAX, address) || !integer(args[2], 1, UINT32_MAX, n))) ||
        (count == 4 && !integer(args[3], 1, sizeof(_stringPrefix), minimum))) { invalid(); return; }
    const auto result = startReadView(ReadView::STRINGS, address, n, minimum);
    status(result);
    if (result.ok()) { _operation = true; _hasResult = false; beginSession(command); print("Queued read-only strings scan; one transaction maximum per tick.\n"); }
    return;
  }
  if (equals(command, "current")) {
    uint32_t n = 1;
    if (count > 2 || (count == 2 && !integer(args[1], 1, sizeof(_data), n))) { invalid(); return; }
    const auto result = _device.startCurrentRead(_data, n);
    status(result); if (result.ok()) { _operation = true; _hasResult = false; beginSession(command); } return;
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
  if (equals(command, "read") ||
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
  if (result.ok()) { _operation = true; _hasResult = false; beginSession(command); print("Queued; one physical transaction maximum per tick.\n"); }
}
void Cli::tick() {
  if (expireSession()) return;
  if (_autoPoll) advance();
  else _device.tick(now()); // Passive physical settling continues while owner polling is paused.
  if (!expireSession()) finishSession();
}
void Cli::advance() {
  _device.tick(now());
  if (expireSession()) return;
  if (_interfaceWait) {
    if (static_cast<int32_t>(now() - _interfaceReadyAt) < 0) return;
    _interfaceWait = false;
  }
  if (_scan) {
    const auto result = _platform.probeAddress(_scanNext, _platform.user);
    if (result.ok()) {
      ++_scanFound;
      print("  Found 0x%02X%s\n", _scanNext, _scanNext >= 0x50 && _scanNext <= 0x57 ? " (EEPROM/bank candidate; identity unverified)" : "");
    } else if (!result.is(EEPROM24Cxx::Err::I2C_NACK_ADDR)) {
      ++_scanErrors;
      if (_sessionStatus.ok()) _sessionStatus = result;
    }
    if (_scanNext++ == _scanLast) {
      _scan = false; print("Scan complete: %u ACK, %u other transport errors.\n", _scanFound, _scanErrors);
    }
    return;
  }
  if (_stress && !_operation && static_cast<int32_t>(now() - _nextMs) >= 0) {
    const auto result = _device.startRead(_repeatAddress, _data, _repeatLength);
    if (result.ok()) { _operation = true; _hasResult = false; }
    else {
      status(result);
      if (_sessionStatus.ok()) _sessionStatus = result;
      ++_stressFailures; if (_remaining) --_remaining; _nextMs = now() + _repeatInterval;
      if (!_remaining) {
        print("%s stopped: ok=%lu fail=%lu\n", _watch ? "Watch" : "Stress", static_cast<unsigned long>(_stressSuccess),
              static_cast<unsigned long>(_stressFailures));
        _stress = false; _watch = false;
      }
    }
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
