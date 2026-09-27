# Measurements

Stock vs. our firmware, measured through the mixer loopback rig (TrueMix600
mixer, USB to the Mac, pedal inline). Each run appends a section below.

How to run:

```bash
.venv/bin/python tools/measure.py run-all --device "FB200" --note "48 kHz, default preset"
.venv/bin/python tools/measure.py run-all --device "TrueMix600" --note "analog loopback, pedal inline"
```

Analysis code is covered by `tools/measure.py selftest` (synthetic signals)
which `tests/test_measure.py` runs.

No runs recorded yet.
