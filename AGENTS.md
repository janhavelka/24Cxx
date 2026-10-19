# EEPROM24Cxx repository conventions

## CI and local verification

- Work with CI in mind from the start: inspect the relevant workflows and checks
  before implementation, reuse their commands, and keep changes compatible with
  their platforms, pinned dependencies, generated files and test registration.
- Run focused local checks for each change. Reserve remote CI dispatch and waiting
  for major changes: substantial behavior/API changes, cross-module integration,
  persistence/security changes, build/toolchain/dependency/workflow changes, and
  release or major integration milestones. Judge impact, not just diff size.
- Small documentation, comment, formatting and low-risk isolated edits need
  appropriate local verification, not a CI run after every commit or push. Batch
  related work before requesting CI; do not repeatedly dispatch unchanged suites.
- For major changes, use existing CI and verify the exact final commit through
  terminal results; dispatch it if needed. Fix confirmed failures without weakening
  checks. Report checks not run, blocked or failed honestly; never claim an older
  green run verifies a new commit. Honor required merge/release checks and existing
  hooks; do not change workflow triggers or bypass checks to avoid CI.

## Git workflow

- Before editing, fetch and inspect the current branch and upstream; fast-forward
  only when safe.
- Keep the whole promptset on its existing branch. Do not create a branch or
  worktree per prompt or agent unless the user asks.
- Assume concurrent agents. Preserve unrelated changes; never stash, discard,
  overwrite, or commit them. A dirty worktree alone is not a blocker.
- After relevant checks pass, stage only your own files or hunks, then commit
  and push each meaningful completed chunk, including multiple times within
  one prompt and before finishing. No further permission is needed.
- When using subagents, the lead owns Git mutations for their shared work.
- Confirm upstream synchronization. Follow the CI and local verification policy
  above; report conflicts or sync failures and never force-push to resolve them.

## Project guidance

- This repository implements 24Cxx I2C EEPROMs, defaulting to the exact Zetta
  ZD24C02B-MAGMT geometry. The former TMP1x2 implementation is Git history only;
  its vendor references are preserved under `docs/archive/`.
- Public headers belong in `include/EEPROM24Cxx/`, implementation in `src/`.
  The C++17 core has no Arduino, ESP-IDF, logging, allocation, tasks, bus handles,
  platform delays, pin initialization or internal bus ownership.
- Follow MB85RC's typed terminal transport and write-effect evidence conventions,
  structured Status, typed enums, passive four-state health and memory vocabulary.
  Application callbacks own serialization, timeouts, pins, clock and recovery;
  callbacks must not retain borrowed buffers, retry secretly or re-enter.
- Bind, operation admission, cancellation, result consumption and teardown are
  bus-silent. Owner-driven poll performs bounded transfers. Compatibility probe,
  begin and recover must clearly document their synchronous transfer behavior.
- EEPROM writes must split at physical page, address-bank and transport limits.
  Do not wrap ranges, guess capacity by writing, or infer identity from an ACK.
  Address-pin meanings, page sizes and upper bank bits vary by manufacturer.
- STOP starts a write cycle. Preserve its wait barrier through failures,
  cancellation and teardown. Never automatically replay an ambiguous write.
  ACK/elapsed write time does not prove contents changed: WP may suppress writes.
  Distinguish accepted bytes, completed work and readback-verified bytes.
- Expected address NACKs during a known ACK-poll window are busy observations,
  not transport-health failures. Generic NACKs and ordinary access failures must
  not be silently reclassified. Probes bypass tracked health.
- No fictitious register map, chip ID, reset, erase or security-lock commands.
  Raw main-array byte addressing is the common protocol; vendor extras need an
  explicitly documented implementation and must never run implicitly.
- Examples share a framework-neutral CLI with the sibling help layout, ANSI
  colors, strict parsers and diagnostics. Startup and ordinary diagnostics never
  program EEPROM. Explicit write/fill commands are the mutation paths.
- Use `library.json` as version source; regenerate with
  `python scripts/generate_version.py sync`, never edit Version.h manually.
- On Windows use `scripts/pio.cmd`, the existing VS Code-managed PlatformIO.
  Do not install another PlatformIO Core.
- Run native protocol/fault/CLI tests, framework-boundary checks and ESP32-S2/S3
  Arduino/native-IDF builds after relevant changes. Record actual results;
  mocks, syntax checks and CI configuration are not physical hardware evidence.
- Vendor reference artifacts retain original licenses. Preserve source URLs,
  revisions and SHA-256 checksums. Reference source is excluded from builds.
