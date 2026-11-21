import contextlib
import importlib.util
import io
import math
from pathlib import Path

spec=importlib.util.spec_from_file_location("capture_noise",Path(__file__).resolve().parents[1]/"capture_noise.py")
c=importlib.util.module_from_spec(spec);spec.loader.exec_module(c)
line="N1,1,256,1400000,3000,3000,1390,1410,1401000,4000,4000,1380,1420,0,0,0,0"
quiet=c.parse(line)
assert quiet["opa_mean"]==1400 and quiet["opa_rms"]==3
assert c.parse("SENS: 50") is None
assert c.rms([quiet],"raw")==4
music=dict(quiet,opa_rms=5,opa_peak=8,raw_rms=5,raw_peak=8)
output=io.StringIO()
with contextlib.redirect_stdout(output): c.summarize([music],[quiet])
assert "2.5 dB" in output.getvalue() # sqrt(5^2-3^2)/3 = 4/3
assert "NOT ADC ENOB" in output.getvalue()
with contextlib.redirect_stdout(io.StringIO()): c.summarize([quiet],[music])
assert math.isfinite(c.rms([quiet,music],"opa"))
print("PASS: CSV units, menu filtering, power subtraction and below-noise handling")

line2=line.replace("N1,", "N2,").rsplit(",0,0,0,0",1)[0]+",1402000,5000,6000,1370,1430,0,0,0,0"
row2=c.parse(line2)
assert row2['gpamp_rms']==5 and row2['gpamp_peak']==6
with contextlib.redirect_stdout(io.StringIO()): c.summarize([row2],[quiet])
print('PASS: N2 GPAMP fields and mixed-version reference handling')
