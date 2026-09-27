#!/usr/bin/env python3
"""Hantek DSO2D15 signal generator over USBTMC/SCPI.

The generator's front panel can only fire a burst MANUALLY ("Trigger source | Manual" in Hantek's
spec table), which is why crank-synchronised placement is impossible from the panel. Over SCPI,
:DDS:BURSt:TRIGger is a remote button press — so a burst can at least be fired ON DEMAND from a
script, which is what makes an automated amplitude calibration possible.

Command set from Hantek's own SCPI Programmers Manual, section 13 (DDS subsystem):
    :DDS:SWITch ON|OFF          output enable
    :DDS:TYPE SINE|SQUAre|...   waveform
    :DDS:FREQ <Hz>              frequency
    :DDS:AMP <V>                amplitude (peak-to-peak)
    :DDS:OFFSet <V>             DC offset
    :DDS:BURSt:SWITch ON|OFF    burst mode
    :DDS:BURSt:TYPE N_CYCLE|INFInit
    :DDS:BURSt:CNT <n>          cycles per burst (1..1024)
    :DDS:BURSt:TRIGger          fire one burst

NOTE ON OFFSET: leave it at 0. The ECU's knock input biases an AC signal to mid-rail itself — the
measured samples sit centred on ~2019 counts with the generator at zero offset. Adding 1.65 V here
would be fighting a bias network that is already doing the job.

The node is root-owned by default; `sudo chmod 666 /dev/usbtmc0` (or a udev rule for 049f:505e)
makes it usable. Re-plugging the scope resets that.
"""
import time


class Dso2d15:
    def __init__(self, dev="/dev/usbtmc0", settle=0.15):
        self.f = open(dev, "r+b", buffering=0)
        self.settle = settle

    def write(self, cmd):
        self.f.write((cmd + "\n").encode())
        time.sleep(self.settle)

    def query(self, cmd, n=200):
        self.write(cmd)
        try:
            return self.f.read(n).decode(errors="replace").strip()
        except OSError as ex:
            return f"<{ex}>"

    def idn(self):
        return self.query("*IDN?")

    # ---- generator ----
    def output(self, on):        self.write(f":DDS:SWITch {'ON' if on else 'OFF'}")
    def wave(self, t="SINE"):    self.write(f":DDS:TYPE {t}")
    def freq(self, hz):          self.write(f":DDS:FREQ {hz}")
    def amp(self, vpp):          self.write(f":DDS:AMP {vpp}")
    def offset(self, v):         self.write(f":DDS:OFFSet {v}")

    def burst_setup(self, cycles):
        """N-cycle burst, fired only when trigger() is called."""
        self.write(":DDS:BURSt:SWITch ON")
        self.write(":DDS:BURSt:TYPE N_CYCLE")
        self.write(f":DDS:BURSt:CNT {int(cycles)}")

    def burst_off(self):         self.write(":DDS:BURSt:SWITch OFF")
    def trigger(self):           self.write(":DDS:BURSt:TRIGger")

    def state(self):
        return {k: self.query(q) for k, q in [
            ("out", ":DDS:SWITch?"), ("type", ":DDS:TYPE?"), ("freq", ":DDS:FREQ?"),
            ("amp", ":DDS:AMP?"), ("offset", ":DDS:OFFSet?"),
            ("burst", ":DDS:BURSt:SWITch?"), ("cnt", ":DDS:BURSt:CNT?")]}

    def close(self):
        try:
            self.f.close()
        except Exception:
            pass


if __name__ == "__main__":
    d = Dso2d15()
    print(d.idn())
    for k, v in d.state().items():
        print(f"  {k:8s} {v}")
    d.close()
