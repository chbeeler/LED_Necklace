"""Capture pre-AEQ audio statistics; compare quiet music with a quiet-room CSV."""
import argparse
import csv
from datetime import datetime
import math
from pathlib import Path
import time

FIELDS = ('seq ms opa_mean opa_rms opa_peak opa_min opa_max '
          'raw_mean raw_rms raw_peak raw_min raw_max missed adc_overrun trigger_overrun tx_dropped').split()
SCALED = {'opa_mean','opa_rms','opa_peak','raw_mean','raw_rms','raw_peak'}


def parse(line):
    parts = line.strip().split(',')
    if len(parts) != len(FIELDS)+1 or parts[0] != 'N1':
        return None
    values = [int(value) for value in parts[1:]]
    if any(value < 0 or value > 0xffffffff for value in values):
        raise ValueError('Invalid measurement value')
    return {key: value/1000 if key in SCALED else value
            for key,value in zip(FIELDS,values)}


def load(path):
    with path.open(newline='') as f:
        reader=csv.DictReader(f)
        if reader.fieldnames != FIELDS:
            raise ValueError('Not a noise capture CSV')
        rows=[{k:float(v) for k,v in row.items()} for row in reader]
    if not rows:
        raise ValueError('Empty capture')
    return rows


def rms(rows, channel):
    return math.sqrt(sum(row[channel+'_rms']**2 for row in rows)/len(rows))


def health(rows):
    gaps=sum(max(0,int(b['seq']-a['seq']-1)) for a,b in zip(rows,rows[1:]))
    counters={key:int(rows[-1][key]) for key in FIELDS[-4:]}
    print(f'Frames: {len(rows)}; missing UART summaries: {gaps}; counters since boot: {counters}')
    if gaps or any(counters.values()):
        print('WARNING: acquisition/serial errors occurred; reset and repeat before interpreting margins.')


def summarize(rows, quiet=None):
    health(rows)
    for channel in ('opa','raw'):
        total=rms(rows,channel)
        peak=max(r[channel+'_peak'] for r in rows)
        mean=sum(r[channel+'_mean'] for r in rows)/len(rows)
        low=min(r[channel+'_min'] for r in rows)
        high=max(r[channel+'_max'] for r in rows)
        print(f'{channel.upper()}: RMS {total:.3f} counts, peak-window RMS {peak:.3f}, '
              f'DC {mean:.1f}, ADC range {low:.0f}..{high:.0f}')
        if (channel=='opa' and (low<256 or high>3967)) or low<16 or high>4079:
            print('  WARNING: reached clipping/range-switch margins; avoid interpreting this as a clean gain/noise test.')
        if quiet is not None:
            noise=rms(quiet,channel)
            quiet_peak=max(r[channel+'_peak'] for r in quiet)
            signal=math.sqrt(max(0,total*total-noise*noise))
            fraction=sum(r[channel+'_peak']>quiet_peak for r in rows)/len(rows)
            print(f'  Quiet RMS {noise:.3f}; quiet peak-window RMS {quiet_peak:.3f}; '
                  f'{100*fraction:.0f}% of music summaries contain a window above that quiet peak.')
            if signal>0 and noise>0:
                ratio=signal/noise
                print(f'  Estimated music/noise: {20*math.log10(ratio):.1f} dB '
                      f'({math.log2(ratio):.2f} amplitude bits above noise; NOT ADC ENOB).')
            else:
                print('  No positive signal/noise estimate resolved from these captures.')
    if quiet is not None:
        print('Estimate assumes unchanged, independent background noise; this is whole-system noise, not OPA-only noise.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port',help='e.g. COM45')
    parser.add_argument('label',choices=['quiet','music'])
    parser.add_argument('--seconds',type=float,default=20)
    parser.add_argument('--output',type=Path)
    parser.add_argument('--compare',type=Path,help='quiet-room CSV, recorded with identical hardware')
    args=parser.parse_args()
    if not 2 <= args.seconds <= 600:
        parser.error('--seconds must be between 2 and 600')
    quiet=load(args.compare) if args.compare else None
    if quiet is not None:
        print('Reference capture:');health(quiet)
    output=args.output or Path('captures')/f'{args.label}-{datetime.now():%Y%m%d-%H%M%S}.csv'
    output.parent.mkdir(parents=True,exist_ok=True)
    # Refuse overwriting a previous measurement before touching the board.
    with output.open('x',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=FIELDS);writer.writeheader()
        import serial
        rows=[]
        with serial.Serial(args.port,115200,timeout=0.5,write_timeout=2) as port:
            port.write(b'X');time.sleep(0.4);port.reset_input_buffer()
            print(f'Capturing {args.label} for {args.seconds:g} seconds. Keep the mic still; do not press buttons.',flush=True)
            port.write(b'M')
            start=time.monotonic()
            try:
                while time.monotonic()-start < args.seconds:
                    line=port.readline().decode('ascii',errors='replace')
                    if not line:
                        if not rows and time.monotonic()-start>3:
                            raise RuntimeError('No measurement stream. Flash the new firmware and check COM port/wiring.')
                        continue
                    row=parse(line)
                    if row is None:
                        if line.strip(): print('Other UART output:',line.strip())
                        continue
                    rows.append(row);writer.writerow(row);f.flush()
            finally:
                port.write(b'X');port.flush()
        if not rows:
            raise RuntimeError('No measurement frames received')
    print('Saved:',output.resolve())
    summarize(rows,quiet)


if __name__=='__main__':
    main()
