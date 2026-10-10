The imported XNU tree retains its Apple Public Source License 2.0 and other original per-file notices. Additional Apple open-source IOKit family sources retain their original notices. Upstream origin: https://github.com/HTC-Leo-Revival-Project/xnu, commit d5c85d9b54bc27c338e69ff75dce2386dcfd723a. Newly authored port files without an explicit license notice are provided for inspection; no additional blanket license is asserted for those files.

The newly authored Nexus7 platform files `xnu/pexpert/arm/pe_tegra30.c` and
`xnu/pexpert/arm/nexus7_runtime.c` carry BSD-2-Clause SPDX identifiers. The MIG93
host build uses external source and ILP32 shim headers at configurable local
paths; those inputs are not vendored here and retain their original notices.
