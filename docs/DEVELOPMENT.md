# Development workflow

This document describes the existing prototype. The publication mock-up is not a source distribution or an installer, and the commands below refer to the working development workspace.

## Preserve the working setup

Inspect the existing workspace recursively before staging anything. Discover the reference DLSS package, copied Skyrim directory, `SkyrimSE.exe` and any already-present integration files. Record resolved absolute paths in `DEVELOPMENT_NOTES.md` and the launcher's path record.

Use the copied Skyrim installation as the sacrificial test target. Keep the reference package intact. The recorded tests did not modify the real Steam installation. Launching the copy still requires working Steam IPC.

The original gameplay-tested proxy and launcher remain a separate baseline. A second checkpoint preserves the native-fed Helgen binary before the later history-reset repair.

## Available workspace launchers

| Launcher | Purpose |
|---|---|
| `Launch-Neural-Test.ps1` | Preserved display-image fallback. |
| `Launch-Temporal-Test.ps1` | Observe native buffers without changing neural input parameters. |
| `Launch-Native-Inputs-Test.ps1` | Experimental feeding using the reviewed Helgen profile. |

The launchers use the discovered copied-game path, 2560×1440 borderless and isolated per-run profiles. F8 toggles the master effect; F9 opens the core panel. Keep the launcher running until the game exits so restoration can finish.

Each run takes an exclusive staging lock, backs up affected DLLs, records hashes, and restores staged files after exit. Game-created AE content changes in the sacrificial copy remain inventoried. Reusing a prior test content catalog is best-effort and may not eliminate installation prompts.

## Build and dependencies

The workspace uses Zig 0.14.1, Python, MinHook and the pinned DXL v0.7 source/release. The author's core and supplied neural runtime are reused without patching. The compiler and Python paths in the current scripts are machine-specific; a public source release needs portable dependency/path setup first.

```powershell
# From the existing development workspace:
.\tools\build.ps1 -Temporal
.\Launch-Temporal-Test.ps1
# After reviewed gameplay capture approval:
.\Launch-Native-Inputs-Test.ps1
```

These commands do not work from this documentation-only mock-up.

## Evidence and rollback

`temporal.jsonl` and `probe.log` record native formats, resolution, finite/non-zero status, frozen TAA/copy serials, supplied resources, parameter reads, validation age and capture/conversion/readback timings. `core_status.jsonl` records failed/skipped neural calls, master state and full neural/optical timings.

The prebuilt core overlay's original Depth/Motion labels do not track the external wrapper and may remain zero. Inspect the external logs when checking native inputs.

The workspace includes bounded capture, temporal analysis, sensitivity analysis and guarded rollback tools. Normal completed runs restore automatically; recovery refuses active test PIDs and unexpected hashes. The sensitivity module only accepts the synthetic fixture executable, so zero-input substitution is not enabled in Skyrim.

ENB is not part of this workflow. Native feeding remains experimental until the unresolved depth, registration and broader stability questions are answered.
