<img src="assets/project-mark.svg" alt="Skyrim Neural Renderer" width="680" />

# Skyrim Neural Renderer

An experimental neural rendering integration for **Skyrim Special Edition** on Windows. It preserves the working DXL Present path and adds capture of Skyrim's native D3D11 depth and motion buffers.

**Experimental prototype** · Vanilla / AE Helgen tested · 2560×1440 borderless · F8 toggle

> Native depth and motion capture work in gameplay, and both inputs reach the neural runtime. Controlled tests show that motion affects neural output. **Depth's effect on neural output is still unproven.**

## What works today

- Skyrim loads into gameplay with the neural effect active.
- F8 switches the effect off and on in real time.
- Native depth and motion are finite, non-zero and match the moving Helgen scene.
- The latest native-input gameplay test recorded **at least 9,851 neural calls**, with **zero failed or skipped calls**, and a clean Quit to Desktop.
- The original display-image implementation remains a known-good fallback.
- Separate logs expose input validity, formats, resolution, resource forwarding, failed/skipped calls and GPU timings.

## A real gameplay capture

![Helgen frame with display color, device depth, motion visualization and pre-TAA color](assets/helgen-buffer-inspection.png)

Left to right: display color at the capture point, raw device depth, UV motion visualization, and native pre-TAA color. Captured during the Helgen carriage sequence at 2560×1440 on 6 October 2026. The display capture is taken before that frame's neural processing. **This is a buffer inspection, not an effect ON/OFF comparison.**

## Verified results

| Check | Result |
|---|---|
| Native depth | Matching gameplay geometry; R32_FLOAT supplied to the neural runtime. |
| Native motion | Matching frame identities and current-to-previous UV convention; R16G16_FLOAT supplied. |
| Motion correspondence | Three larger adjacent-frame comparisons improve alignment error by about 58–65% over zero motion. Reversed and scaled alternatives perform worse. |
| Motion use | Changing motion changes neural output in a controlled fixture, with byte-identical native repeat controls. |
| Depth use | Replacing depth with zeros produces no measurable RGBA8 output change in the tested probes, including retained history. Still unproven. |
| Controls and exit | F8 visibly confirmed by the tester; the latest native-input run exited normally. |

Read the [validation report](docs/VALIDATION.md) for scope, timings and evidence. Successful resource getters are recorded separately from proof of model use.

## How it fits together

The game stays on D3D11. DXL v0.7 owns the existing Present/display-image route and shares color/output with its private D3D12 neural device. The optional capture module snapshots native depth and motion at Skyrim's TAA boundary, then supplies shared resources through a parameter wrapper. It reuses the existing fence ordering and adds no second Present hook.

Missing or stale inputs preserve the original parameter path. A subsequent repair resets neural history when moving between native and fallback inputs; this repair passed a synthetic transition test and has not had another gameplay test.

## Current limits

- Depth's effect on the model is not demonstrated.
- Exact subpixel depth registration to the final display is unresolved.
- An earlier observe-mode run hit an engine shutdown error. The latest native-input run exited cleanly; the earlier cause remains unresolved.
- Validation covers a short vanilla/AE Helgen test at 1440p with one neural feature. Other scenes, long sessions, resize, HDR, later HUD/transparency and multiple True NR Layers are not validated.
- **ENB compatibility has not been worked on.**

The core overlay's original Depth/Motion labels may still show zero. The external capture module reports its inputs through `temporal.jsonl` and `probe.log`; core call counts, skips, master state and neural timings are recorded in `core_status.jsonl`.

## Getting started

This publication mock-up contains documentation, a local repository preview and a gameplay inspection image. It is **not a runnable mod download**. The implementation remains in the existing development workspace, with machine-specific build/runtime paths.

The intended development workflow uses a copied, sacrificial Skyrim installation, discovered paths, per-run profiles, backed-up staging and automatic DLL restoration. The real Steam installation was not modified in the recorded tests. See the [development guide](docs/DEVELOPMENT.md) and [draft release notes](releases/v0.1.0-prototype.md).

## Next work

1. Establish whether and how the runtime uses depth.
2. Resolve exact depth/display registration.
3. Retest the history-reset repair in gameplay and investigate the earlier shutdown error.
4. Broaden vanilla gameplay validation before investigating ENB.

## Credits

This is an independent experiment. The neural loader/runtime integration builds on [LCPD15's DXL v0.7](https://github.com/LCPD15/DXL/tree/8644a875e61a9ecb8379f78ce9fd3ef3bc4853a4). The prototype uses MinHook for process-local hooks. Skyrim, NVIDIA, DLSS and other names belong to their respective owners; this project does not imply their endorsement.

The mock-up does not choose a license for the project's own code or redistribute game files, vendor runtime DLLs or upstream binaries. Upstream license texts and notices remain with their respective dependencies. See [publication contents](PUBLICATION.md).
