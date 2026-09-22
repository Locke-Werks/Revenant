# Convert a 24-bit PCM WAV to 32-bit float, optionally taking an excerpt.
#
# WHY THIS EXISTS. core/source/file_source.cpp reads 8-bit and 16-bit PCM and
# 32-bit float, and declines 24-bit because the host conversion pass it would
# need is the one the upload kernels exist to avoid. The HF recordings in
# docs/recordings.md are 24-bit, so nothing in this project can open them.
#
# This does the conversion once, offline, where a host pass costs nothing that
# matters. It is not part of the engine and is not on any sample path.
#
# WHY AN EXCERPT IS THE DEFAULT SHAPE. The recordings are 2 GiB each and a full
# conversion is 4/3 of that. A minute is 46 MB at 96 kS/s and is enough to ask
# the detector a question, which is what the grid work needs first.
#
#   python scripts/wav24_to_float32.py IN.wav OUT.wav --seconds 60 --start 300
#
# Scaling is by 2^23, so a full-scale 24-bit sample lands on 1.0 and the float
# file carries the same numbers in the range the rest of the tree assumes.
# Nothing is dithered and nothing is normalised: this is a width change and
# should be nothing else.

import argparse
import struct
import sys
from pathlib import Path


def find_chunks(data: bytes):
    """Every top-level RIFF chunk as (id, offset_of_body, size)."""
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise SystemExit("not a RIFF/WAVE file")
    out = []
    i = 12
    while i + 8 <= len(data):
        cid = data[i : i + 4]
        size = struct.unpack("<I", data[i + 4 : i + 8])[0]
        out.append((cid, i + 8, size))
        i += 8 + size + (size & 1)
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("source", type=Path)
    ap.add_argument("destination", type=Path)
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="length to take, 0 for the whole file")
    ap.add_argument("--start", type=float, default=0.0,
                    help="seconds to skip first")
    args = ap.parse_args()

    with args.source.open("rb") as f:
        head = f.read(64 * 1024)

        fmt = None
        data_at = None
        data_size = 0
        for cid, body, size in find_chunks(head):
            if cid == b"fmt ":
                fmt = head[body : body + size]
            elif cid == b"data":
                data_at, data_size = body, size
                break
        if fmt is None or data_at is None:
            raise SystemExit("no fmt or data chunk in the first 64 KiB")

        tag, channels, rate, _byte_rate, align, bits = struct.unpack("<HHIIHH", fmt[:16])
        if tag == 0xFFFE:
            # WAVE_FORMAT_EXTENSIBLE puts the real tag in the first two bytes
            # of the SubFormat GUID, which begins at offset 24 of the chunk.
            tag = struct.unpack("<H", fmt[24:26])[0]
        if tag != 1 or bits != 24:
            raise SystemExit(f"expected 24-bit PCM, got tag {tag} at {bits} bits")

        frame_bytes = channels * 3
        if align not in (0, frame_bytes):
            raise SystemExit(f"block align {align} is not {frame_bytes}")

        total_frames = data_size // frame_bytes
        first = int(args.start * rate)
        if first >= total_frames:
            raise SystemExit(f"--start is past the end: {total_frames} frames")
        count = total_frames - first
        if args.seconds > 0.0:
            count = min(count, int(args.seconds * rate))

        print(f"{args.source.name}: {channels} ch, {rate} S/s, 24-bit, "
              f"{total_frames} frames ({total_frames / rate:.1f} s)")
        print(f"taking {count} frames ({count / rate:.1f} s) from {args.start:.1f} s")

        f.seek(data_at + first * frame_bytes)

        out_bytes = count * channels * 4
        with args.destination.open("wb") as g:
            g.write(b"RIFF")
            g.write(struct.pack("<I", 4 + 8 + 16 + 8 + out_bytes))
            g.write(b"WAVE")
            g.write(b"fmt ")
            g.write(struct.pack("<IHHIIHH", 16, 3, channels, rate,
                                rate * channels * 4, channels * 4, 32))
            g.write(b"data")
            g.write(struct.pack("<I", out_bytes))

            # A frame at a time would be slow on 46 MB. One second at a time
            # keeps the peak allocation small and the loop short.
            chunk_frames = rate
            done = 0
            while done < count:
                take = min(chunk_frames, count - done)
                raw = f.read(take * frame_bytes)
                if len(raw) < take * frame_bytes:
                    raise SystemExit("file ended early")

                values = bytearray()
                for i in range(0, len(raw), 3):
                    # Little-endian signed 24-bit, sign extended.
                    v = raw[i] | (raw[i + 1] << 8) | (raw[i + 2] << 16)
                    if v & 0x800000:
                        v -= 0x1000000
                    values += struct.pack("<f", v / 8388608.0)
                g.write(values)
                done += take
                if done % (rate * 10) == 0:
                    print(f"  {done / rate:.0f} s")

    print(f"wrote {args.destination} ({out_bytes + 44} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
