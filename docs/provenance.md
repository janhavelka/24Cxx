# Source provenance

The user's corrected target is Zetta ZD24C02B-MAGMT and the general 24Cxx I2C
EEPROM family. The repository folder remains `24Cxx`; the package, namespace
and include directory are named `EEPROM24Cxx` because C++ identifiers cannot
start with a digit.

The initial commit `0c1cb48` implemented TMP1x2 because of the original request.
That sensor API, implementation and tests are replaced by EEPROM code. The
initial commit remains available in Git history. Its reference material is
preserved separately under `docs/archive/tmp1x2-reference/` and is not an EEPROM
specification or build dependency.

The repository/build scaffold, version generator and terminal styling came
from the earlier setup, which reused sibling `Projects/TMP1x2` commit
`a3e567456160079124843aea5ad3ae4738474b6c`. The EEPROM driver follows the closer
MB85RC transport, memory-operation and write-effect conventions. The local
library inventory and API comparison are in [library-comparison.md](library-comparison.md).
No sibling directory is required at build or runtime.

The MIT license and original local author notices are retained. Manufacturer
PDFs, vendor source and example distributions retain their original notices
and licenses; reference code is not compiled or incorporated into the driver.
Current EEPROM sources, revisions, SHA-256 hashes and retrieval limitations
are recorded in [reference/README.md](reference/README.md) and its manifest.
