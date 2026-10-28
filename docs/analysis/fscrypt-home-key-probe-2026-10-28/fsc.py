#!/usr/bin/env python3
"""Minimal fscrypt v2 ioctl driver (stand-in for fscryptctl; mirrors what nostr-authd would do in C)."""
import fcntl, os, struct, sys, errno
SET_POLICY   = 0x800C6613  # _IOR('f',19,fscrypt_policy_v1)
GET_POLICY_EX= 0xC0096616  # _IOWR('f',22,__u8[9])
ADD_KEY      = 0xC0506617  # _IOWR('f',23,fscrypt_add_key_arg[80])
REMOVE_KEY   = 0xC0406618  # _IOWR('f',24,fscrypt_remove_key_arg[64])
REMOVE_ALL   = 0xC0406619  # _IOWR('f',25,...)
KEY_STATUS   = 0xC080661A  # _IOWR('f',26,fscrypt_get_key_status_arg[128])
SPEC_ID = 2
def spec(idhex): return struct.pack("<II", SPEC_ID, 0) + bytes.fromhex(idhex).ljust(32, b"\0")
def fd_of(p): return os.open(p, os.O_RDONLY | os.O_DIRECTORY)
def die(e): print(f"ERR {errno.errorcode.get(e.errno, e.errno)} ({e.strerror})"); sys.exit(1)
cmd = sys.argv[1]
try:
    if cmd == "add":
        raw = open(sys.argv[3], "rb").read()
        buf = bytearray(struct.pack("<II", SPEC_ID, 0) + b"\0"*32 + struct.pack("<III", len(raw), 0, 0) + b"\0"*28 + raw)
        fcntl.ioctl(fd_of(sys.argv[2]), ADD_KEY, buf)
        print(bytes(buf[8:24]).hex())
    elif cmd == "set":
        cm = int(sys.argv[4]) if len(sys.argv) > 4 else 1   # 1 = AES-256-XTS
        fm = int(sys.argv[5]) if len(sys.argv) > 5 else 4   # 4 = AES-256-CTS, 10 = AES-256-HCTR2
        fl = int(sys.argv[6], 0) if len(sys.argv) > 6 else 0x02  # 0x02 = PAD_16, 0x03 = PAD_32
        pol = struct.pack("<BBBBBBBB", 2, cm, fm, fl, 0, 0, 0, 0) + bytes.fromhex(sys.argv[3])
        fcntl.ioctl(fd_of(sys.argv[2]), SET_POLICY, pol); print("policy set")
    elif cmd == "get":
        buf = bytearray(struct.pack("<Q", 24) + b"\0"*24)
        fcntl.ioctl(fd_of(sys.argv[2]), GET_POLICY_EX, buf)
        v = buf[8]; print(f"v{v} contents={buf[9]} filenames={buf[10]} flags={buf[11]:#x} key_id={bytes(buf[16:32]).hex()}")
    elif cmd == "status":
        buf = bytearray(spec(sys.argv[3]) + b"\0"*24 + b"\0"*64)
        fcntl.ioctl(fd_of(sys.argv[2]), KEY_STATUS, buf)
        st, fl, uc = struct.unpack_from("<III", buf, 64)
        print(f"status={ {1:'ABSENT',2:'PRESENT',3:'INCOMPLETELY_REMOVED'}.get(st, st) } added_by_self={bool(fl & 1)} user_count={uc}")
    elif cmd == "remove":
        buf = bytearray(spec(sys.argv[3]) + b"\0"*24)
        fcntl.ioctl(fd_of(sys.argv[2]), REMOVE_ALL if "--all" in sys.argv else REMOVE_KEY, buf)
        fl = struct.unpack_from("<I", buf, 40)[0]
        print(f"removed; files_busy={bool(fl & 1)} other_users={bool(fl & 2)}")
except OSError as e: die(e)
