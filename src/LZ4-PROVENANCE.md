<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Provenance — `lz4.c` / `lz4.h`

**Origin:** [`github.com/lz4/lz4`](https://github.com/lz4/lz4), tag `v1.10.0`, `lib/` folder.

**Fetched on:** 2026-08-31.

**Fetch command:**

```bash
curl -fsSL -o libgm/src/lz4.c https://raw.githubusercontent.com/lz4/lz4/v1.10.0/lib/lz4.c
curl -fsSL -o libgm/src/lz4.h https://raw.githubusercontent.com/lz4/lz4/v1.10.0/lib/lz4.h
```

**SHA-256 fingerprints, verified at integration:**

| file | bytes | SHA-256 |
|---|---|---|
| `lz4.c` | 118 145 | `9396f7de527bc8435de9c7569fb7998e56545a84b4f3c2d808c0235c01774539` |
| `lz4.h` | 46 014 | `26b82efc53d1570f3b54eef02e9c4764c1ad374ff03cac04e2ced5ea4d4c552f` |

Any mismatch on these fingerprints must stop any further integration: third-party code is
never embedded here without knowing exactly what it is.

**License:** BSD-2-Clause for the contents of `lib/` (these two files). The `lz4/lz4`
repository's CLI and test programs are GPL-2.0-or-later and are **not** embedded here —
only `lib/lz4.c` and `lib/lz4.h` are.

**Why the source is embedded, and not an MSYS2 package or one more DLL.**
A locked decision: `libgm.dll` must be dropped next to `retroarch.exe`, and that spot has
already cost a frozen bench once — a stale copy of `libgm.dll` made a launch stop on a
modal `csrss` dialog, with no log line at all. One more moving piece in that spot (an
MSYS2 `liblz4.dll` to deploy and verify separately) is a known risk, not a theoretical
one. Also ruled out: static linking against the MSYS2 package's `.a`, which would make
the build depend on a hand-installed, unversioned package.

**Accepted consequence:** the lz4 version is pinned to `v1.10.0` and its update is
manual — rerun the two `curl` commands above against a newer tag, then update the
fingerprints and the date in this file.

**Format compatibility with the receiver:** the receiver uses `liblz4` 1.10.0 as a system
package. Same upstream version
on both sides of the wire, so there is no risk of an LZ4 block format mismatch between the
encoder embedded here and the receiver's decoder.
