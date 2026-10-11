#include "ConsoleOutput.h"
#include "../support/CliFixture.h"
#include <algorithm>

#define CHECK(x) do { if (!(x)) { std::printf("[FAIL] console/soak line %d: %s\n", __LINE__, #x); return 1; } } while (false)
namespace {
void append(eeprom24cxx_cli::ConsoleOutput& queue, const char* format, ...) {
  va_list args; va_start(args, format); queue.append(format, args); va_end(args);
}
struct Sink {
  std::string data;
  size_t limit = 0;
  unsigned calls = 0;
  static size_t write(const uint8_t* bytes, size_t count, void* user) {
    auto& sink = *static_cast<Sink*>(user); ++sink.calls;
    count = std::min(count, sink.limit);
    sink.data.append(reinterpret_cast<const char*>(bytes), count); return count;
  }
};
}
int main() {
  using eeprom24cxx_cli::ConsoleOutput;
  ConsoleOutput queue; queue.ready(true); Sink sink;
  std::string expected;
  for (unsigned n = 0; n < 50; ++n) {
    const std::string line(200, static_cast<char>('A' + n % 26));
    append(queue, "%s", line.c_str()); expected += line;
  }
  CHECK(!queue.outputReady() && queue.stats().queuedBytes == expected.size());
  queue.drain(UINT32_MAX - 500U, Sink::write, &sink);
  queue.drain(600U, Sink::write, &sink); queue.drain(1600U, Sink::write, &sink);
  CHECK(queue.stats().writeTimeouts == 1 && queue.stats().txDropped == 0 && sink.data.empty());
  sink.limit = 7; // Partial writes, including ring wrap, preserve exact order.
  while (queue.stats().queuedBytes) {
    const auto before = sink.data.size(); queue.drain(2000, Sink::write, &sink);
    CHECK(sink.data.size() - before <= ConsoleOutput::DRAIN_BUDGET);
  }
  CHECK(sink.data == expected && queue.outputReady());
  for (unsigned n = 0; n < 50; ++n) { append(queue, "%s", std::string(200, 'z').c_str()); expected.append(200, 'z'); }
  while (queue.stats().queuedBytes) queue.drain(2001, Sink::write, &sink);
  CHECK(sink.data == expected && queue.stats().txDropped == 0);
  append(queue, "%s", std::string(600, 'x').c_str());
  CHECK(queue.stats().formatOverflows == 1 && queue.stats().txDropped == 600 && queue.stats().queuedBytes == 0);
  for (unsigned n = 0; n < 90; ++n) append(queue, "%s", std::string(200, 'q').c_str());
  CHECK(queue.stats().queuedBytes <= ConsoleOutput::CAPACITY && queue.stats().txDropped > 600);

  Fixture help; eeprom24cxx_cli::Cli cli; cli.setup(help.platform(), help.config());
  help.output.clear(); const auto transfers = help.transfers;
  cli.processCommand("help");
  CHECK(help.output.size() <= ConsoleOutput::RESERVE); // Real ANSI help fits one reserved burst.
  for (const char* topic : {"help detection", "help tests", "help jobs", "help read", "help dump", "help uverify", "help quickstart"}) {
    help.output.clear(); cli.processCommand(topic);
    CHECK(help.output.find("INVALID_PARAM") == std::string::npos && !help.output.empty());
  }
  help.output.clear(); cli.processCommand("help nonsense");
  CHECK(help.output.find("INVALID_PARAM") != std::string::npos && help.transfers == transfers);
  cli.feed('w'); cli.feed('r'); cli.discardInput();
  for (char c : std::string("itebyte 0 7\n")) cli.feed(c);
  CHECK(help.transfers == transfers && help.writes == 0);

  struct Samples : Fixture { std::vector<uint32_t> offsets; bool corrupt = false; } samples;
  auto config = samples.config();
  config.i2cWriteRead = [](uint8_t bus, const uint8_t* tx, size_t n, uint8_t* out, size_t count, uint32_t timeout, void* user) {
    auto& f = static_cast<Samples&>(*static_cast<Fixture*>(user));
    f.offsets.push_back(f.address(bus, tx));
    auto result = Fixture::read(bus, tx, n, out, count, timeout, user);
    if (f.corrupt && f.reads == 2 && result.ok()) out[0] ^= 1;
    return result;
  };
  eeprom24cxx_cli::Cli sampled; sampled.setup(samples.platform(), config); sampled.processCommand("color off");
  samples.output.clear(); sampled.processCommand("stress 18 1"); CHECK(samples.run(sampled, 100));
  CHECK(samples.reads == 36 && samples.writes == 0);
  for (size_t n = 0; n < samples.offsets.size(); ++n) CHECK(samples.offsets[n] == (n / 2U * 16U) % 256U);
  CHECK(samples.output.find("target=18 completed=18 ok=18 fail=0 bytes=576") != std::string::npos);
  samples.output.clear(); sampled.processCommand("job"); const auto finished = samples.output;
  samples.ms += 10000; samples.output.clear(); sampled.processCommand("job");
  CHECK(samples.output == finished); // Retained elapsed time does not continue increasing.
  samples.corrupt = true; samples.reads = 0; samples.output.clear();
  sampled.processCommand("stress 2 1"); CHECK(samples.run(sampled));
  CHECK(samples.output.find("Stress stopped: ok=1 fail=1") != std::string::npos);
  CHECK(samples.output.find("outcome=FAILED status=VERIFY_MISMATCH") != std::string::npos);

  // Cancellation retains bytes already read without claiming a complete round.
  Fixture interrupted; eeprom24cxx_cli::Cli interruptedCli;
  interruptedCli.setup(interrupted.platform(), interrupted.config());
  interrupted.output.clear(); interruptedCli.processCommand("watch 0 32 3 1");
  CHECK(interrupted.run(interruptedCli, 1));
  interruptedCli.processCommand("stop");
  CHECK(interrupted.output.find("target=3 completed=0 ok=0 fail=0 bytes=16") != std::string::npos);
  CHECK(interrupted.output.find("outcome=CANCELLED") != std::string::npos);

  // Backpressure pauses new transactions, but cannot hide an elapsed deadline.
  Fixture paused; auto platform = paused.platform();
  platform.outputReady = [](void*) { return false; };
  eeprom24cxx_cli::Cli pausedCli; pausedCli.setup(platform, paused.config());
  pausedCli.processCommand("color off"); pausedCli.processCommand("deadline 5");
  const auto before = paused.transfers; paused.output.clear(); pausedCli.processCommand("read 0 16");
  CHECK(paused.run(pausedCli, 10) && paused.transfers == before);
  CHECK(paused.output.find("outcome=TIMED_OUT") != std::string::npos);
  std::puts("[PASS] bounded console, backpressure, help, rotating paired stress and retained timing");
  return 0;
}
