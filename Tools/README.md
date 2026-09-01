# Host tools

This directory contains native development tools, not firmware libraries.

## Ownership

| Target | Purpose | Long-term home |
| --- | --- | --- |
| `krul_host_server` | Generic Krul/Surge/transport integration example | Krul repository, under `examples/host-server` |
| `ark_emulator_server` | Stateful emulator of the ARK command surface | ARK repository |
| `ark_emulator_test` | Contract test for the ARK emulator | ARK repository |

The generic server is kept here until the Krul repository is extracted. Set
`ARK_BUILD_GENERIC_KRUL_SERVER=OFF` when only the ARK-specific emulator is
needed.

The Python protocol simulator belongs to the Starset repository and currently
lives at `Python GUI/krul_simulator.py`.

## Build and test

From the ARK repository root:

```powershell
cmake --preset PC_Debug
cmake --build --preset PC_Debug
ctest --test-dir build/PC_Debug --output-on-failure
```
