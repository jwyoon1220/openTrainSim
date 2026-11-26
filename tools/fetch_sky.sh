#!/bin/sh
# Downloads the sky maps used by the physical sky into data/sky/ (not redistributed).
#   NASA SVS Deep Star Maps 2020, 8K OpenEXR, J2000 equatorial (https://svs.gsfc.nasa.gov/4851)
#   ESA/Gaia/DPAC EDR3 colour sky map, 4K PNG, galactic, CC BY-SA 3.0 IGO (A. Moitinho)
set -e
dir="$(cd "$(dirname "$0")/.." && pwd)/data/sky"
mkdir -p "$dir"
cd "$dir"
fetch() { [ -s "$1" ] || curl --http1.1 -fL --retry 5 -C - -o "$1" "$2"; }
fetch starmap_2020_8k.exr "https://svs.gsfc.nasa.gov/vis/a000000/a004800/a004851/starmap_2020_8k.exr"
fetch Gaia_EDR3_flux_cartesian_4k.png "https://sci.esa.int/documents/33580/35361/Gaia_EDR3_flux_cartesian_4k.png/4648ec09-6c63-47a5-d0da-3ab3e0b42afe?version=1.0&t=1606986362680&download=true"
echo "sky maps in $dir"
