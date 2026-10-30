#!/usr/bin/env python3
"""Talk to the KagigataDisk FIDO2 key (example/002) over Linux hidraw.

No dependencies: CTAPHID is spoken directly on /dev/hidrawN. The key is found
by USB VID:PID (KGFIDO_VID / KGFIDO_PID, default cafe:4004) and the FIDO
usage page.

  ./tools/kgfido.py list               # hidraw nodes of attached keys
  ./tools/kgfido.py info               # firmware line + CTAP2 getInfo
  ./tools/kgfido.py wink               # flash the LEDs
  ./tools/kgfido.py touch [--watch]    # touch sensor readings
  ./tools/kgfido.py bootsel            # erase credentials + PIN, then the UF2 bootloader
                                       # (hold the card contacts 3 s while all LEDs are lit)

Access: systemd's fido_id tags FIDO hidraw nodes for the desktop user
(uaccess); tools/install.sh adds a plugdev rule for other sessions.
"""
import argparse
import glob
import os
import select
import sys
import time

VID = int(os.environ.get("KGFIDO_VID", "cafe"), 16)
PID = int(os.environ.get("KGFIDO_PID", "4004"), 16)

BROADCAST = 0xFFFFFFFF
CMD_INIT, CMD_WINK, CMD_CBOR = 0x06, 0x08, 0x10
CMD_KEEPALIVE, CMD_ERROR = 0x3B, 0x3F
KG_BOOTSEL, KG_TOUCH, KG_INFO = 0x70, 0x71, 0x72

CTAP_STATUS = {
    0x2D: "cancelled",
    0x2F: "no touch in time",
    0x7F: "error",
}
HID_ERRORS = {1: "invalid command", 2: "invalid parameter", 3: "invalid length",
              4: "invalid sequence", 5: "timeout", 6: "channel busy",
              0x0B: "invalid channel", 0x7F: "other"}


class KeyFailure(Exception):
    pass


def find_devices():
    found = []
    for d in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        try:
            with open(d + "/device/uevent") as f:
                uevent = f.read()
            with open(d + "/device/report_descriptor", "rb") as f:
                desc = f.read()
        except OSError:
            continue
        ids = [l.split("=", 1)[1] for l in uevent.splitlines() if l.startswith("HID_ID=")]
        if not ids:
            continue
        _, vid, pid = (int(x, 16) for x in ids[0].split(":"))
        # Usage page 0xF1D0 (FIDO) as the first item.
        if (vid, pid) == (VID, PID) and desc.startswith(b"\x06\xd0\xf1"):
            found.append("/dev/" + os.path.basename(d))
    return found


class Ctaphid:
    def __init__(self, path):
        try:
            self.fd = os.open(path, os.O_RDWR)
        except PermissionError:
            raise KeyFailure(f"no access to {path}: run tools/install.sh, or use a desktop session")
        self.cid = BROADCAST
        nonce = os.urandom(8)
        data = self.call(CMD_INIT, nonce)
        if data[:8] != nonce:
            raise KeyFailure("INIT: nonce mismatch")
        self.cid = int.from_bytes(data[8:12], "big")

    def close(self):
        os.close(self.fd)

    def _write(self, pkt):
        os.write(self.fd, b"\0" + pkt.ljust(64, b"\0"))  # report ID 0 + 64 bytes

    def _read(self, timeout):
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            raise KeyFailure("no answer from the key")
        return os.read(self.fd, 64)

    def send(self, cmd, data):
        cid = self.cid.to_bytes(4, "big")
        self._write(cid + bytes([0x80 | cmd]) + len(data).to_bytes(2, "big") + data[:57])
        rest, seq = data[57:], 0
        while rest:
            self._write(cid + bytes([seq]) + rest[:59])
            rest, seq = rest[59:], seq + 1

    def recv(self, timeout, on_keepalive=None):
        deadline = time.monotonic() + timeout
        while True:
            pkt = self._read(max(0.0, deadline - time.monotonic()))
            if int.from_bytes(pkt[:4], "big") != self.cid or not pkt[4] & 0x80:
                continue
            cmd, n = pkt[4] & 0x7F, int.from_bytes(pkt[5:7], "big")
            if cmd == CMD_KEEPALIVE:
                if on_keepalive:
                    on_keepalive(pkt[7])
                continue
            data, seq = pkt[7:7 + n], 0
            while len(data) < n:
                pkt = self._read(1.0)
                if int.from_bytes(pkt[:4], "big") != self.cid or pkt[4] != seq:
                    raise KeyFailure("broken response")
                data += pkt[5:5 + n - len(data)]
                seq += 1
            if cmd == CMD_ERROR:
                raise KeyFailure("CTAPHID error: " + HID_ERRORS.get(data[0], hex(data[0])))
            return data

    def call(self, cmd, data=b"", timeout=5.0, on_keepalive=None):
        self.send(cmd, data)
        return self.recv(timeout, on_keepalive)


# --- minimal CBOR decoding, for getInfo ----------------------------------
def cbor_decode(b, i=0):
    ib = b[i]
    major, ai = ib >> 5, ib & 31
    i += 1
    if ai < 24:
        v = ai
    elif ai <= 27:
        n = 1 << (ai - 24)
        v = int.from_bytes(b[i:i + n], "big")
        i += n
    else:
        raise ValueError("indefinite length")
    if major == 0:
        return v, i
    if major == 1:
        return -1 - v, i
    if major in (2, 3):
        s = b[i:i + v]
        return (bytes(s) if major == 2 else s.decode()), i + v
    if major == 4:
        out = []
        for _ in range(v):
            x, i = cbor_decode(b, i)
            out.append(x)
        return out, i
    if major == 5:
        out = {}
        for _ in range(v):
            k, i = cbor_decode(b, i)
            out[k], i = cbor_decode(b, i)
        return out, i
    if major == 6:
        return cbor_decode(b, i)
    return {20: False, 21: True, 22: None}.get(v, v), i


def touch_hint(status):
    if status == 2 and not touch_hint.shown:
        print("hold a finger on the gold contacts on the card edge for 3 s (all LEDs lit)...",
              flush=True)
        touch_hint.shown = True


touch_hint.shown = False


def cmd_info(key, _args):
    print(key.call(KG_INFO).decode())
    resp = key.call(CMD_CBOR, b"\x04")
    if resp[0]:
        raise KeyFailure(f"getInfo: status 0x{resp[0]:02x}")
    info, _ = cbor_decode(resp, 1)
    names = {1: "versions", 2: "extensions", 3: "aaguid", 4: "options", 5: "maxMsgSize",
             6: "pinProtocols"}
    for k, v in info.items():
        if isinstance(v, bytes):
            v = v.hex()
        print(f"  {names.get(k, k)}: {v}")


def cmd_wink(key, _args):
    key.call(CMD_WINK)


def read_touch(key):
    r = key.call(KG_TOUCH)
    pins = []
    for i in range(r[1]):
        q = r[2 + 7 * i: 9 + 7 * i]
        pins.append((q[0], int.from_bytes(q[1:3], "little"), int.from_bytes(q[3:5], "little"),
                     int.from_bytes(q[5:7], "little")))
    return bool(r[0]), pins


def cmd_touch(key, args):
    while True:
        touched, pins = read_touch(key)
        line = "  ".join(f"gpio{g}: raw={raw:5d} base={base:5d} (+{thr})" for g, raw, base, thr in pins)
        print(("TOUCH " if touched else "      ") + line, flush=True)
        if not args.watch:
            return
        time.sleep(0.1)


def cmd_bootsel(key, args):
    print("every credential, the PIN and the master secret will be erased")
    # The byte selects "wipe" on firmware built before keep mode was dropped.
    st = key.call(KG_BOOTSEL, b"\x01", timeout=40.0, on_keepalive=touch_hint)[0]
    if st:
        raise KeyFailure("bootsel refused: " + CTAP_STATUS.get(st, hex(st)))
    print("credentials erased; rebooting into the UF2 bootloader")


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--device", help="hidraw node (default: the first key found)")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("info")
    sub.add_parser("wink")
    t = sub.add_parser("touch")
    t.add_argument("--watch", action="store_true", help="keep printing (Ctrl-C to stop)")
    sub.add_parser("bootsel")
    args = p.parse_args()

    devices = find_devices()
    if args.cmd == "list":
        print("\n".join(devices) if devices else f"no {VID:04x}:{PID:04x} FIDO key found")
        return 0 if devices else 1
    path = args.device or (devices[0] if devices else None)
    if not path:
        print(f"no {VID:04x}:{PID:04x} FIDO key found (firmware not running?)", file=sys.stderr)
        return 1
    try:
        key = Ctaphid(path)
        try:
            {"info": cmd_info, "wink": cmd_wink, "touch": cmd_touch, "bootsel": cmd_bootsel}[args.cmd](key, args)
        finally:
            key.close()
    except KeyFailure as e:
        print(f"{path}: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
