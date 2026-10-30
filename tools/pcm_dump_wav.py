import argparse
import struct
import sys
import wave
from pathlib import Path

MAGIC = b"PCM1"
RATE = 16000
COUNT = 160000
PCM_BYTES = COUNT * 2


def pack_frame(rate, count, payload):
    return MAGIC + struct.pack("<II", rate, count) + payload


class DumpParser:
    def __init__(self):
        self.buf = bytearray()

    def idle(self):
        self.buf.clear()

    def feed(self, data):
        self.buf += data
        out = []
        while True:
            i = self.buf.find(MAGIC)
            if i < 0:
                if len(self.buf) > 3:
                    del self.buf[:-3]
                break
            del self.buf[:i]
            if len(self.buf) < 12:
                break
            rate, count = struct.unpack_from("<II", self.buf, 4)
            if rate != RATE or count != COUNT:
                del self.buf[:1]
                continue
            if len(self.buf) < 12 + PCM_BYTES:
                break
            out.append(bytes(self.buf[12 : 12 + PCM_BYTES]))
            del self.buf[: 12 + PCM_BYTES]
        return out


def write_wav(path, pcm):
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(RATE)
        handle.writeframes(pcm)


def next_wav(out_dir):
    n = 1
    while True:
        path = Path(out_dir) / f"dump-{n:03d}.wav"
        if not path.exists():
            return path
        n += 1


def self_check():
    payload = bytes(i & 0xFF for i in range(PCM_BYTES))
    frame = pack_frame(RATE, COUNT, payload)
    if DumpParser().feed(frame) != [payload]:
        return False
    parser = DumpParser()
    parser.feed(frame[:100])
    parser.idle()
    return parser.feed(frame) == [payload]


def main():
    if "--self-check" in sys.argv:
        raise SystemExit(0 if self_check() else 1)
    args = argparse.ArgumentParser()
    args.add_argument("port")
    args.add_argument("--out", default=".")
    opt = args.parse_args()
    import serial

    ser = serial.Serial(opt.port, 921600, timeout=1)
    parser = DumpParser()
    while True:
        chunk = ser.read(4096)
        if not chunk:
            parser.idle()
            continue
        for pcm in parser.feed(chunk):
            path = next_wav(opt.out)
            write_wav(path, pcm)
            print(path)


if __name__ == "__main__":
    main()
