#!/usr/bin/env python3
"""STUN binding responder (RFC 5389 subset) for the netns chaos lab.

Answers every binding request with XOR-MAPPED-ADDRESS and MAPPED-ADDRESS for
the source tuple it arrived from, exactly what the daemons' STUN clients
need to learn their NAT-reflexive endpoint. IPv4 only, like the lab. Each
request is logged as `binding request from <ip>:<port>` so the harness can
tell whether a node's STUN traffic ever left its NAT.
"""
import argparse
import socket
import struct
import sys
import time

MAGIC = 0x2112A442


def reply(txn, addr):
    ip = socket.inet_aton(addr[0])
    cookie = MAGIC.to_bytes(4, 'big')
    xor_value = b'\x00\x01' + struct.pack('!H', addr[1] ^ (MAGIC >> 16)) + bytes(a ^ b for a, b in zip(ip, cookie))
    plain_value = b'\x00\x01' + struct.pack('!H', addr[1]) + ip
    attrs = struct.pack('!HH', 0x0020, len(xor_value)) + xor_value + struct.pack('!HH', 0x0001, len(plain_value)) + plain_value
    return struct.pack('!HHI', 0x0101, len(attrs), MAGIC) + txn + attrs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bind', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=3478)
    args = ap.parse_args()
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    print(f'{time.strftime("%H:%M:%S")} stun: listening on {args.bind}:{args.port}', flush=True)
    while True:
        data, remote = sock.recvfrom(2048)
        if len(data) < 20:
            continue
        msg_type, msg_len, cookie = struct.unpack('!HHI', data[:8])
        if msg_type != 0x0001 or cookie != MAGIC or msg_len != len(data) - 20:
            print(f'{time.strftime("%H:%M:%S")} stun: ignored non-binding packet from {remote[0]}:{remote[1]}', flush=True)
            continue
        sock.sendto(reply(data[8:20], remote), remote)
        print(f'{time.strftime("%H:%M:%S")} stun: binding request from {remote[0]}:{remote[1]}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
