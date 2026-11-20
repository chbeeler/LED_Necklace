import importlib.util, struct, sys
from pathlib import Path
sys.dont_write_bytecode=True
spec=importlib.util.spec_from_file_location('updater',Path(__file__).resolve().parents[1] / 'update_bsl.py')
u=importlib.util.module_from_spec(spec);spec.loader.exec_module(u)
class FakePort:
    timeout=3
    def __init__(self): self.rx=b'';self.commands=[]
    def reset_input_buffer(self): self.rx=b''
    def write(self,packet):
        payload=packet[3:-4];self.commands.append(payload[0])
        assert packet[-4:]==struct.pack('<I',u.crc(payload))
        self.rx=b'\0'
        if payload==b'\x19':
            info=bytearray(25);info[0]=0x31
            struct.pack_into('<H',info,11,1728)
            struct.pack_into('<II',info,17,1,1)
            self.rx+=b'\x08'+struct.pack('<H',25)+info+struct.pack('<I',u.crc(info))
        return len(packet)
    def read(self,n): result=self.rx[:n];self.rx=self.rx[n:];return result
p=FakePort();u.wait_for_bootloader(p,1)
assert p.commands==[0x12,0x19],p.commands
assert p.timeout==3
assert u.crc(b"\x12") == 0xde44613a
assert u.crc(b"\x21" + b"\xff" * 32) == 0x3df0aa02
print("PASS: one handshake, identity framing, restored timeout and TI CRC vectors")

# Reproduce an apparent connection ACK followed by the reported 0x12 error.
class ResetNoisePort(FakePort):
    def __init__(self):
        super().__init__()
        self.first_identity=True
    def write(self, packet):
        result=super().write(packet)
        if packet[3]==0x19 and self.first_identity:
            self.first_identity=False
            self.rx=b"\x12"
        return result
p=ResetNoisePort()
u.wait_for_bootloader(p,1)
assert p.commands==[0x12,0x19,0x12,0x19]
assert p.timeout==3
assert not any(cmd in p.commands for cmd in (0x21,0x23,0x20,0x30))
# Corrupt identity CRC must also be rejected and retried before any unlock.
class BadCrcPort(FakePort):
    corrupt=True
    def write(self, packet):
        result=super().write(packet)
        if packet[3]==0x19 and self.corrupt:
            self.corrupt=False
            self.rx=self.rx[:-1]+bytes([self.rx[-1]^1])
        return result
p=BadCrcPort();u.wait_for_bootloader(p,1)
assert p.commands==[0x12,0x19,0x12,0x19]
print("PASS: reset noise and corrupt identity retry without unlock or erase")
