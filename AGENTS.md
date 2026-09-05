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

## Task for the next agent: iteration code review

Perform a code review of the current Krul iteration against the previous
iteration documented in `docs/code_review_2025_08_28.md`.

- Treat that document as a historical baseline, not as a current backlog.
- Re-check every prior finding against the current implementation and classify
  it as fixed, still present, changed, or no longer applicable.
- Review the current diff and surrounding code for regressions and new issues,
  with particular attention to public API/protocol compatibility, memory and
  buffer bounds, transport behavior, and test coverage.
- Report findings first, ordered by severity. For every finding, cite the
  current file and line, explain the concrete impact, and give a reproducible
  scenario or supporting evidence. Do not report style-only preferences as
  defects.
- Record the review in a new dated file under `docs/`; do not overwrite the
  historical review. Include the verification commands run and their results,
  plus a short comparison summary covering resolved, remaining, and newly
  introduced findings.
