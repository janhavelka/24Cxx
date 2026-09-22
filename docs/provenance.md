# Source provenance and repository identity

This repository was initialized in the user-selected workspace
`C:/Users/Honza/Documents/Projects/24Cxx` on 2026-09-22. The requested device is
TI TMP102/TMP112, so its library name, C++ namespace and public include path
remain `TMP1x2`. The existing folder name `24Cxx` does not change that device
scope; this repository does not implement a 24Cxx EEPROM driver.

The sibling-library survey found an exact implementation already present in
`C:/Users/Honza/Documents/Projects/TMP1x2`. Its tracked source files were reused
as the starting point:

- Source repository: local `Projects/TMP1x2`.
- Source commit: `a3e567456160079124843aea5ad3ae4738474b6c` (`a3e5674`).
- Commit subject: `Initialize TMP1x2 temperature sensor library`.
- Source manifest version: `1.0.0`.
- Scope: tracked library, examples, tests, build/contract tools, documentation
  and TI reference artifacts. Source `.git` history and untracked build outputs
  were not imported.

The reused driver implements the exact sensor family and already follows the
local callback, Status, typed-enum, health and CLI conventions. Reusing it
preserves tested protocol handling, including configuration readback, partial
write diagnostics and the TI-documented extended-format transition. Changes
made after the initial import belong to this repository's own Git history.

Among the other sibling libraries, OPT4001 is the closest overall API and
terminal-presentation template; ADS1115 is the closest four-register and address
layout analogue; SHT3x provides the shared framework-neutral CLI pattern.
[library-comparison.md](library-comparison.md) records the complete inspected
inventory, source-file evidence and differences in operation scheduling and
health semantics. These are conventions and implementation references, not
runtime dependencies on sibling directories.

The root MIT license covers the independently authored library/example source
under its retained copyright notices. TI documents, reference distributions and
Linux driver snapshots keep their original copyright and licensing terms.
[reference/README.md](reference/README.md), `reference/manifest.json` and
`reference/SHA256SUMS` record artifact origins, immutable source revisions where
available and checksums. Reference code is not linked into the library.

Validation recorded in the source repository is historical evidence. Current
repository checks and their limitations are recorded in
[validation.md](validation.md); initialization alone does not establish physical
sensor validation or a newly completed firmware build.
