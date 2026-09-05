# Krul repository guidance

Krul is the canonical shared command stack and desktop client for ARK and BKU.
It owns serialization, transport, command-interface, Python GUI, docs, and
generic host examples in Tools.

Do not add ARK- or BKU-specific commands, hardware access, or board emulators
here. Preserve Krul v4 and transport v1 unless a change explicitly updates the
implementation, Starset, specifications, and compatibility tests together.
Keep payloads within 10 KiB.

Native verification from a compiler-enabled shell:

    cmake -S . -B build/Debug -G Ninja -DBUILD_TESTING=ON
    cmake --build build/Debug
    ctest --test-dir build/Debug --output-on-failure

Python verification:

    python -m pytest "Python GUI/tests"

ARK and BKU consume this repository via KRUL_ROOT. Do not introduce source
path dependencies on either consumer.
