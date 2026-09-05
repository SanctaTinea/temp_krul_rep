# Code review Krul — 2026-09-05

## Scope and baseline

Reviewed the current `master` worktree: changes from `origin/master`
(`81a0d13`) through `HEAD` (`72c0136`), plus the uncommitted changes in
`transport/src/krul_transport.c`. The historical review
`docs/code_review_2025_08_28.md` was used as a baseline, not as an active
backlog. Generated build output and board-specific ARK/BKU sources were out of
scope.

## Findings

## Previous-review status

| Previous item | Status in this iteration |
| --- | --- |
| Missing root build/README/CI | Resolved: root CMake project, presets, README and GitHub Actions workflow are present. |
| README pointed to missing `main.py` and stale `Libs/docs` paths | Resolved; the documented entry point is `Starset.py` and documentation paths use `docs/`. |
| `Starset.py` monolith (~2900 lines) | Partially resolved: transport, wire codec, widgets, theme and configuration are separate modules; `Starset.py` is now about 1306 lines. |
| Unknown response IDs were silently discarded | Resolved: `Starset.py:539-547` validates the ID and emits a warning for unmatched responses; covered by a test. |
| Client timeout reused an undocumented protocol error | Resolved/clarified: `CLIENT_ERROR_TIMEOUT` is explicitly client-only and timeout objects carry `source: "client"`. |
| Serial UTF-8 decoding used `errors="replace"`; GUI lacked a 10 KiB frame limit | Resolved by the framed `krul_wire.FrameParser`: strict decoding is used and `MAX_PAYLOAD_SIZE` is 10 KiB. |
| JSON token sizing was undocumented; only JSON codec existed | Resolved: sizing guidance is documented and BSON/CBOR implementations and tests are present. |
| `KRUL_TYPE_CONSOLE_STRING` had an undecided public status | Still present, but the informal removal note is gone and the type now has a stable-looking API description. A compatibility decision is still advisable before the next major protocol version. |
| Duplicate console/severity mapping in `krul.c` and `krul_discovery.c` | Unchanged; low-impact maintenance duplication. |
| Linear command/field lookup and a single response buffer | Unchanged documented design constraints, not correctness defects at the current scale. |
| JSON writer depth 12 vs Krul result depth 8 | Unchanged and not a defect; the stricter application-layer limit remains valid. |
| Missing fuzz coverage and repository license | Not addressed in this iteration. |

## Verification

- `python -m pytest "Python GUI/tests"`: **85 passed** (one environment-only
  warning because pytest could not create `.pytest_cache`).
- Current-tree native MSVC build from an initialized Visual Studio environment:
  **failed** at `transport/src/krul_transport.c:82-90`, before CTest.
- BKU CM7 consumer build with ARM GCC 15.2.1: **failed** at
  `transport/src/krul_transport.c:82-90`, confirming the P1 finding.
- A clean archive of Krul `HEAD` (without the uncommitted transport edit),
  built with MSVC through the BKU host simulator: **built successfully; 6/6
  CTest tests passed** (`serde_json`, `serde_bson`, `serde_cbor`, transport,
  Krul host and BKU simulator).

Hardware and serial-device testing were not performed.
