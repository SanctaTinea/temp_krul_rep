# Krul

Krul is the canonical shared protocol repository used by ARK and BKU. It owns
Surge serialization, transport framing, Krul discovery and dispatch, Starset,
the protocol specifications, and generic host examples. Board-specific firmware
and emulators remain in their respective repositories.

ARK and BKU select this checkout through the KRUL_ROOT CMake cache path. The
default workspace layout places ARK, BKU, and Krul in sibling directories.

## Native verification

With Ninja and a configured C compiler:

    cmake -S . -B build/Debug -G Ninja -DBUILD_TESTING=ON
    cmake --build build/Debug
    ctest --test-dir build/Debug --output-on-failure

The equivalent repository presets are:

    cmake --preset PC_Debug
    cmake --build --preset PC_Debug
    ctest --preset PC_Debug

Generate Doxygen documentation with the preset build tree:

    cmake --build --preset PC_Debug --target docs

## Starset

Install the desktop client dependencies and run the canonical entry point:

    python -m pip install -r "Python GUI/requirements.txt"
    python "Python GUI/Starset.py"

Run its tests with:

    python -m pip install -r "Python GUI/requirements-dev.txt"
    python -m pytest -q "Python GUI/tests"

Normative protocol documents are under `docs/`.
