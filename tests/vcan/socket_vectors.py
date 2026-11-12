"""Separate peer validates bytes and injects frames on an existing virtual CAN network."""
import socket
import struct
import subprocess
import sys

exe, interface = sys.argv[1:]
with socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as peer:
    peer.bind((interface,))
    peer.settimeout(1)
    proc=subprocess.Popen([exe, interface])
    try:
        ident, size, data=struct.unpack("=IB3x8s",peer.recv(16))
        assert (ident,size,data[:7])==(0x201,7,b'\0'*6+b'\x08')
        for other_id in (0x182, 0x80000181, 0x40000181):
            peer.send(struct.pack("=IB3x8s",other_id,6,b'ignored'))
        peer.send(struct.pack("=IB3x8s",0x181,6,b'\x27\0\xfc\xff\xff\xff'))
        assert proc.wait(timeout=2)==0
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait()
