#!/usr/bin/env python3
"""Assert that a WITH_COCO_ROMS=OFF binary really carries no Tandy ROMs.

This is the test that makes the COMPLIANCE.md claim checkable rather than
aspirational: it takes a distinctive slice out of each ROM image in
tools/xroar/roms and greps the built binary for it. Anything found is a ROM
that got compiled in despite the option, which would make the artifact
non-redistributable.

The HDB-DOS DW3 pair (hdbdw3bck.rom, hdbdw3bc3.rom) is expected to be present:
those are freely redistributable and the FujiNet link does not work without
them.

usage: no_embedded_roms.py <binary> <rom-dir>
"""

import sys
from pathlib import Path

EXPECTED_PRESENT = {"hdbdw3bck.rom", "hdbdw3bc3.rom"}
PROBE = 64


def probe_bytes(data):
    """A slice distinctive enough that finding it means something.

    Reject any chunk dominated by a single byte value and require real
    variety, so a run of 0x00 or 0xFF padding -- which occurs in almost any
    binary of a reasonable size -- can never be mistaken for a match. Scan the
    whole file rather than a few fixed offsets. (Inherited from the Apple II
    port, where an earlier version that only rejected mostly-zero chunks let a
    16KB run of 0xFF through and reported a false leak.)
    """
    max_run = PROBE // 2      # no single byte value may cover half the slice
    min_distinct = 12

    for start in range(0, max(1, len(data) - PROBE), PROBE):
        chunk = data[start:start + PROBE]
        if len(chunk) != PROBE:
            continue
        if max(chunk.count(b) for b in set(chunk)) > max_run:
            continue
        if len(set(chunk)) < min_distinct:
            continue
        return chunk
    return None


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    binary = Path(sys.argv[1]).read_bytes()
    romdir = Path(sys.argv[2])

    checked = 0
    leaked = []
    missing_expected = []
    unprobeable = []

    for f in sorted(romdir.iterdir()):
        if not f.is_file() or f.suffix.lower() not in (".rom", ".bin"):
            continue
        data = f.read_bytes()
        if len(data) < 128:
            continue
        chunk = probe_bytes(data)
        if chunk is None:
            # Nothing distinctive enough to search for -- report it rather
            # than quietly treating it as checked.
            unprobeable.append(f.name)
            continue
        checked += 1
        present = chunk in binary
        if f.name in EXPECTED_PRESENT:
            if not present:
                missing_expected.append(f.name)
        elif present:
            leaked.append(f.name)

    if checked == 0:
        sys.exit("no_embedded_roms: no ROM images found in "
                 f"{romdir} -- the test would pass vacuously")

    for name in leaked:
        print(f"FAIL: {name} is embedded in a WITH_COCO_ROMS=OFF build")
    for name in missing_expected:
        print(f"FAIL: {name} should be embedded but is not")

    if leaked or missing_expected:
        return 1
    if unprobeable:
        print("no_embedded_roms: NOT checked (no distinctive slice): "
              + ", ".join(sorted(unprobeable)))
    print(f"no_embedded_roms: {checked} ROM images checked; "
          f"only {', '.join(sorted(EXPECTED_PRESENT))} embedded, as intended")
    return 0


if __name__ == "__main__":
    sys.exit(main())
