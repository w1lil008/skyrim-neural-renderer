# Validation status

The recorded results below distinguish valid gameplay inputs from demonstrated model behavior. Measurements are from 6 October 2026; the publication draft was prepared on 7 October 2026.

## Actual native-input gameplay

| Item | Evidence |
|---|---|
| Scene | Vanilla / AE Helgen carriage, new game. |
| Display | 2560×1440 borderless. |
| Duration | 268.44 seconds for the full run, including menus/loading. |
| Captures | Eight moving frames, arranged as four adjacent pairs. |
| Neural calls | At least 9,851 recorded in sampled core telemetry; the final exact total is not claimed. |
| Failed / skipped calls | Zero recorded. |
| F8 | Tester visually confirmed normal ON/OFF behavior. |
| Quit | Tester used Skyrim's Quit to Desktop; process exit code 0. |
| Frame provenance | Exact TAA shader and source identities, with consecutive copy and TAA serials within every pair. |
| Resource handoff | Both native parameters read, matching private-device identity, no native-failure latch. |

The reviewed depth silhouettes match the character, carriage, trees and terrain in the captured color images. Native inputs are finite and non-zero. This verifies scene correspondence, but does not establish exact subpixel registration to the final display.

## Motion correspondence

The analyzer uses native pre-TAA color and a common comparison mask for every hypothesis. It compares current-to-previous UV motion against zero, opposite, half/double scale and axis flips.

| Adjacent bridge copies | Common pixels | Improvement over zero motion |
|---|---:|---:|
| 7802 → 7803 | 2,687,668 | 57.69% |
| 8041 → 8042 | 1,665,307 | 61.27% |
| 8281 → 8282 | 16,188 | 74.28% |
| 8521 → 8522 | 670,659 | 64.80% |

The expected convention beats the sign/scale alternatives in all four pairs. The third pair has a much smaller mask and carries less weight. Raster jitter and unrepresented transparent/HUD pixels remain limits of this analysis.

## Does the neural model use the inputs?

**Motion: demonstrated in the controlled fixture.** Zero-motion substitution changed output in matched captures. Eight native-versus-native repeat controls were byte-identical with identical RGB and matched neural history age. The frozen vectors are an intentional diagnostic stimulus, not physical motion in a repeated still image.

**Depth: unproven.** Zero-depth substitution produced identical RGBA8 output in analytic RGB and Helgen-photo RGB probes. The photo probe also remained identical with history retained after the initial reset. This is consistent with an unused input or an insensitive scene/model state; output equality cannot exclude differences below 8-bit quantization.

The runtime reads the resource parameters. Direct GPU shader reads were not witnessed by the optional observer. Neither getter success nor a successful neural call is treated as proof of depth use.

## Formats and GPU cost

Native source depth is R24G8_TYPELESS; the supplied neural depth is R32_FLOAT. Source and supplied motion are R16G16_FLOAT. Motion values are current-to-previous UV displacement, with width/height scale parameters converting to pixels. Raw motion dumps are normalized to float32 RG for analysis.

The gameplay log identifies the test adapter as **NVIDIA GeForce RTX 4060 Ti**. Timings describe this one machine and configuration; other hardware has not been validated.

| Measurement | Sample median |
|---|---:|
| Full neural pass | 17.59 ms |
| Core optical flow | 0.39 ms |
| Native snapshots | 0.15 ms |
| Native conversion | 0.20 ms |
| Diagnostic staging readback | 2.24 ms |

These are separate sample timestamps, not total incremental overhead. Readback is scheduled once per 60 bridge copies plus bounded image captures. The small optical-flow number is not the full neural-pass cost.

## Recovery and open issues

A later synthetic test withheld native inputs for copies 240/241. At copy 240, fallback resources and Reset were requested/read; at 242, restored native resources and Reset were requested/read. The test had zero neural failures and a normal exit. Pending Reset is retained across failed native/reset evaluations until success; that failure edge is source-reviewed rather than induced against the actual runtime.

The latest reset repair has not had another gameplay run. An earlier observe-mode Quit raised an engine face-resource database error; this did not recur in the native-input run, and its cause remains unresolved. Validation is limited to one neural feature. Multiple True NR Layers, other scenes, long sessions, resize, HDR and later HUD/transparency are outside the current evidence.

The [sanitized evidence summary](evidence/verification.json) retains run identifiers and relevant binary fingerprints without the development machine's absolute paths. Full raw captures and logs remain in the private workspace.
