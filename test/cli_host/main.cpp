// Host-only CLI transcript fixture. No serial port or physical device is used.
#include "../support/CliFixture.h"
#include <iostream>

int main() {
  Fixture fixture;
  eeprom24cxx_cli::Cli cli;
  for (size_t i = 0; i < sizeof(fixture.memory); ++i)
    fixture.memory[i] = static_cast<uint8_t>(i ^ 0x63U);
  cli.setup(fixture.platform(), fixture.config());
  std::cout << fixture.output;
  fixture.output.clear();
  std::string line;
  while (std::getline(std::cin, line)) {
    for (char value : line) cli.feed(value);
    cli.feed('\n');
    // Bounded simulated owner time, sufficient for the finite transcript suite.
    // Manual jobs remain parked because the real CLI controls auto polling.
    if (!fixture.run(cli, 10000)) {
      std::cerr << "CLI exceeded one physical callback per owner tick\n";
      return 1;
    }
    std::cout << fixture.output << std::flush;
    fixture.output.clear();
  }
  return 0;
}
