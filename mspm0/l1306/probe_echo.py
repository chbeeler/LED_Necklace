"""Read-only UART echo diagnostic. Sends one BSL connection frame; never writes flash."""
import argparse
import serial
import time

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('port')
args=parser.parse_args()
packet=bytes.fromhex('80 01 00 12 3a 61 44 de')
with serial.Serial(args.port,9600,timeout=1,write_timeout=2) as port:
    input('Hold RESET down and keep it held. Press Enter here while holding RESET: ')
    port.reset_input_buffer()
    port.write(packet)
    port.flush()
    received=port.read(64)
    print('TX:',packet.hex(' '))
    print('RX:',received.hex(' ') if received else '(none)')
    if packet in received:
        print('Exact echo detected while MCU is held reset; check adapter/wiring path.')
    elif received:
        print('Bytes received while MCU is held reset; investigate the serial path before flashing.')
    else:
        print('No echo with RESET held. Next check should examine actual BSL entry/replies.')
print('Release RESET. No flash commands were sent.')
