#!/usr/bin/env python3
"""Send 3 manual orders through the full TCP flow: 2 buyers + 1 seller."""

import socket
import struct
import time

WIRE_ORDER_FMT = "<QQ BB 8s 6x d Q"

def make_wire_order(order_id, client_id, side, otype, price, quantity, symbol="XYZ"):
    sym = symbol.encode("ascii")[:8].ljust(8, b"\x00")
    return struct.pack(WIRE_ORDER_FMT,
                       order_id, client_id, side, otype, sym, price, quantity)

def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.connect(("127.0.0.1", 9000))

    # side: 0=BUY, 1=SELL   type: 0=LIMIT

    print("Sending: BUY  10 @ 100")
    sock.sendall(make_wire_order(1, 1, 0, 0, 100.0, 10))
    time.sleep(0.1)

    print("Sending: BUY  15 @ 101")
    sock.sendall(make_wire_order(2, 2, 0, 0, 101.0, 15))
    time.sleep(0.1)

    print("Sending: SELL 10 @ 100")
    sock.sendall(make_wire_order(3, 3, 1, 0, 100.0, 10))
    time.sleep(0.5)

    sock.close()
    print("Done — check server output for TRADE messages")

if __name__ == "__main__":
    main()
