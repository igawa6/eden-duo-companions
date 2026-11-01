#!/usr/bin/env python3
"""Generate the OT0 game-thread heartbeat probe or native healing service.

This is a separate format-2 load plan, not selected by the shipped manifest.
The default preserves native Tick and increments only its own mailbox.
The explicit --query-helper option now includes QUERY/FIND/APPLY SP skills.
"""
import argparse
import hashlib
import json
import struct
import subprocess
import tempfile
from pathlib import Path

TITLE = "01005270232F2000"
BUILD = "9FE60A212CFF9B9A3656AB63D4F882E500000000000000000000000000000000"
IMAGE_SHA256 = "876009a32838a30ecf83b1d89f67bb2436f6be027a900faa6309b1bc67bd6a4e"
SEGMENTS = [(0, 86432112), (86433792, 8982528), (95416320, 13632048)]
BSS = 0x2387DD0
HOOK, TICK = 0x2EA2564, 0x2EAAD10
# stp fp/lr; save x19/x20; native Tick; increment own counter; restore/return.
WORDS = [0xA9BE7BFD, 0xA90153F3, 0x910003FD,
         0x90000010, 0x91000210, 0xD63F0200,
         0x90000013, 0x91000273, 0xB9400268, 0x11000508,
         0xB9000268, 0xD503201F, 0xA94153F3, 0xA8C27BFD, 0xD65F03C0]


def generate(query_code=None):
    words = WORDS
    if query_code is not None:
        if not query_code or len(query_code) % 4:
            raise ValueError("unaligned native query helper")
        words = WORDS[:11] + [0xAA1303E0, 0x90000001, 0x91000021, 0x94000006,
                             0xA94153F3, 0xA8C27BFD, 0xD65F03C0, 0xD503201F, 0xD503201F]
    code = struct.pack("<" + "I" * len(words), *words)
    if query_code is not None:
        code += query_code
    relocs = []
    for offset, kind, target, addend in [
        (12, "aarch64_adrp", "main", TICK),
        (16, "aarch64_add_lo12", "main", TICK),
        (24, "aarch64_adrp", "mailbox", 0),
        (28, "aarch64_add_lo12", "mailbox", 0),
    ]:
        relocs.append(dict(offset=offset, kind=kind, target=target, addend=addend))
    if query_code is not None:
        relocs += [dict(offset=48, kind="aarch64_adrp", target="main", addend=0),
                   dict(offset=52, kind="aarch64_add_lo12", target="main", addend=0)]
    return dict(format=2, title_id=TITLE, build_id=BUILD, module="main",
                layout=dict(segments=[dict(location=p, size=n) for p, n in SEGMENTS], bss_size=BSS),
                mailbox_size=4096, mailbox_epoch_offset=4,
                patches=[dict(offset=HOOK, expected="eb210094", replacement="00000094",
                              relocations=[dict(offset=0, kind="aarch64_branch26", target="code", addend=0)])],
                payloads=[], guest_code=dict(bytes=code.hex(), relocations=relocs))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verify-image", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--query-helper", action="store_true",
                        help="Include native QUERY/FIND/APPLY Healing Skills service")
    args = parser.parse_args()
    image = args.verify_image.read_bytes()
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256 or image[HOOK:HOOK+4].hex() != "eb210094":
        raise SystemExit("Unsupported main.img or native engine-loop pin mismatch")
    query_code = None
    if args.query_helper:
        source = Path(__file__).resolve().parents[2] / "native/guest/ot0_heal_query.c"
        with tempfile.TemporaryDirectory(prefix="ot0-heal-query-") as temporary:
            obj, binary = Path(temporary)/"query.o", Path(temporary)/"query.bin"
            subprocess.run(["clang", "--target=aarch64-none-elf", "-O2", "-ffreestanding",
                            "-fno-builtin", "-fno-jump-tables", "-fno-stack-protector",
                            "-fno-unwind-tables", "-fno-asynchronous-unwind-tables",
                            "-Wall", "-Wextra", "-Werror", "-c", str(source), "-o", str(obj)], check=True)
            symbols = subprocess.check_output(["llvm-nm", "--defined-only", str(obj)], text=True)
            if "0000000000000000 T ot0_heal_query" not in symbols:
                raise SystemExit("Native query entry moved")
            relocations = subprocess.check_output(["llvm-readelf", "-r", str(obj)], text=True)
            if "There are no relocations" not in relocations:
                raise SystemExit("Native query contains unresolved relocations")
            subprocess.run(["llvm-objcopy", "-O", "binary", "--only-section=.text", str(obj), str(binary)], check=True)
            query_code = binary.read_bytes()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(generate(query_code), indent=2) + "\n")
    print(f"Wrote research plan (healing service={args.query_helper}): {args.output}")


if __name__ == "__main__":
    main()
