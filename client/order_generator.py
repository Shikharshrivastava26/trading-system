#!/usr/bin/env python3
"""Send packed WireOrder structs to the matching engine over TCP."""

import socket
import struct
import sys
import time
import random

# WireOrder layout: uint64 order_id, uint64 client_id,
#   uint8 side, uint8 type, 6 bytes padding, double price, uint64 quantity
# Total 40 bytes, native little-endian (assuming x86).
WIRE_ORDER_FMT = "<QQ BB 6s d Q"
WIRE_ORDER_SIZE = struct.calcsize(WIRE_ORDER_FMT)
assert WIRE_ORDER_SIZE == 40, f"Expected 40, got {WIRE_ORDER_SIZE}"

PADDING = b"\x00" * 6


def make_wire_order(order_id: int, client_id: int, side: int, otype: int,
                    price: float, quantity: int) -> bytes:
    return struct.pack(WIRE_ORDER_FMT,
                       order_id, client_id, side, otype, PADDING, price, quantity)


def main() -> None:
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    host  = sys.argv[2] if len(sys.argv) > 2 else "127.0.0.1"
    port  = int(sys.argv[3]) if len(sys.argv) > 3 else 9000

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect((host, port))

    start = time.monotonic()

    for i in range(1, count + 1):
        side  = random.randint(0, 1)          # BUY or SELL
        otype = 0                             # LIMIT
        price = round(100.0 + random.uniform(-5, 5), 2)
        qty   = random.randint(1, 100)
        data  = make_wire_order(i, 1, side, otype, price, qty)
        sock.sendall(data)

    elapsed = time.monotonic() - start
    sock.close()

    print(f"Sent {count} orders in {elapsed:.4f}s "
          f"({count / elapsed:.0f} orders/s)" if elapsed > 0 else
          f"Sent {count} orders")


if __name__ == "__main__":
    main()
