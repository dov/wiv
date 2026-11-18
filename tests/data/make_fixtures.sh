#!/bin/sh
# Regenerates the small binary fixtures used by tests/test_plugin_metadata.cpp.
# Needs ncgen (netcdf) and python3 with h5py + numpy. Run from this directory.
set -e
# The global "history" is 12000 characters long: a regression case for long text attributes.
python3 -c "import sys; print(open('meta_test.cdl.in').read().replace('@LONG_HISTORY@', 'h' * 12000))" > meta_test.cdl
ncgen -b -k classic meta_test.cdl -o meta_test.nc
rm meta_test.cdl
python3 - <<'PY'
import h5py, numpy as np
with h5py.File("meta_test.h5", "w") as f:
    f.attrs["instrument"] = "wiv test rig"
    f.attrs["exposures"] = np.array([1, 2, 3], dtype="i4")
    g = f.create_group("scan")
    g.attrs["operator"] = "nobody"
    d = g.create_dataset("image", data=np.arange(32 * 16, dtype="f4").reshape(16, 32),
                         chunks=(8, 8), compression="gzip", compression_opts=4, shuffle=True)
    d.attrs["units"] = "counts"
    d.attrs["gain"] = 1.5
    d.attrs["label"] = "raw frame"
    f.create_dataset("thumb", data=np.zeros((4, 4), dtype="u2"))
PY
python3 -c "import numpy as np; np.save('meta_test.npy', np.zeros((3, 5), dtype='<f4'))"
