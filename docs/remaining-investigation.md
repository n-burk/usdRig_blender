# Rig investigation status (2026-10-07)

The original Snow teeth/gums, Gamma tail/foot chains, and hidden Spline IK guide fixes are preserved. This continuation fixed the Tail test's clipped tap, installed the missing iOS texture-cache runtime, and optimized a measured connected-frame evaluation hotspot. Physical picker/direct-curve manipulation and texture-cache acceptance now pass. Strict numerical Blender parity and the 33 ms control-drag performance target are **not yet closed**.

Evidence from this continuation is under `build/remaining-investigation/` and `build/drag-perf/` (ignored local artifacts). Earlier evidence remains in `build/final-translation/MOTION-AUDIT.md`. The three delivered USDZ files are unchanged.

## 1. Gamma Tail selection: cause found and physical mesh drag passes

The old canvas-local tap `(442,24)` was on the clipped scroll viewport edge. The canvas reports its entire 544-point content width, not its visible hit area. Its button geometry and target were valid, but the gesture handler received no touch. Moving the point into the visible button interior, panel coordinates `(400,12)`, produced `tap Tail 400,12 hits button_2` and selected `/Rig/Controls/Tail_SPIK03_eeb4503c/Control` on BD1.

In the sibling iOS checkout:

- `Sources/RigExec/RigPickerView.swift` now exposes the scroll viewport's accessibility identifier. Temporary `PICKER_TAP` logging and the temporary status assignment are removed.
- `UITests/AppUITests.swift` asserts that the tap lies inside the visible scroll viewport, retains the exact selected-control assertion, and verifies a real touch drag.
- `Sources/App/BlenderPickerProbe.swift` and the debug inspection methods in `Sources/Engine/UsdViewEngine+View.{h,mm}` read rendered Hydra geometry without evaluating or authoring it. The test compares body-mesh digests before and after the touch drag, so a moving control alone cannot pass.

`testBlenderGammaTailControl` passed on physical BD1 with this mesh assertion (`/tmp/curve-mesh-device-test.log`, test result `Test-usdRigIosUITests-2026.10.07_10-57-11--0700.xcresult`). A Delta hand has no visible guide in the source; the direct-curve test therefore uses Delta's foot. The direct-curve test exposed an ambiguous UI query: the render menu and viewport tool both have a Select button. Opening the menu caused the background tap to dismiss it rather than clear selection. The corrected test chooses Select in the Move toolbar row and waits for the selection predicate. The final indexed-runtime build passes Tail and Snow tumble/root-drag on BD1 in `/tmp/remaining-index-device-test.log` (`Test-usdRigIosUITests-2026.10.07_11-37-37--0700.xcresult`). Snow tumble changes the camera epoch without changing the rig generation or body digest; its subsequent root drag changes the body. After BD1 was unlocked, both corrected direct-curve tests passed: `testBlenderDeltaFootCurveControl` and `testBlenderGammaFootControl`. Each clears the picker selection, selects the exact editable control through an ordinary viewport curve tap, and verifies body deformation after a real gizmo drag. The final Snow texture-loading test also passed. The three-test run has zero failures in `device-final-tests.log`, with attachments in `device-final-attachments/` and Xcode result `Test-usdRigIosUITests-2026.10.07_11-42-09--0700.xcresult`. This closes the outstanding physical curve acceptance.

## 2. Performance: camera isolated; connected-frame cost reduced

The benchmark now sends real Alt-left drag events through usdview. Calling `FreeCamera.Tumble()` directly bypasses the viewer's navigation guard and allows synchronous idle warming into the timing: an exploratory run warmed 249 Snow frames. Those exploratory camera numbers are invalid as user-gesture latency. The final gesture runs record unchanged published generations and zero completed warming frames during navigation.

Same Apple M4 Max, Storm, 1280 × 960 physical viewport, low complexity, current frame, sequential runs. Camera timing includes event delivery and synchronous paint, not physical display scanout. Each camera result has 30 measured moves after 10 warmups. Each control has 24 measured moves after two warmup gestures. The Biped is the current `examples/biped/Biped_stack.usda` (4,062 prims), so these measurements supersede the older 734-prim comparison rather than claiming a controlled comparison with that older run.

Before the runtime optimization:

| Rig | Tumble median / p95 (ms) | Root/body native preview median / p95 (ms) |
| --- | --- | --- |
| Biped | 6.71 / 7.08 | 22.43 / 25.26 |
| Delta | 6.55 / 6.76 | 121.92 / 128.59 |
| Gamma | 7.11 / 7.38 | 166.30 / 171.04 |
| Snow | 7.39 / 8.03 | 221.72 / 227.48 |

Stage-open measurements without renderer/evaluator creation were approximately 52 / 114 / 125 / 119 ms respectively; traversal added 1–3 ms. This is separate from native rig compilation and first evaluation. See `stage-population.json`, `gesture-benchmark/*-camera.tsv`, and each control's `*-drag.tsv`.

A separate 12-sample native evaluator profile excludes Hydra and distinguishes repeated evaluation, interactive overrides, and authored edits (`*-evaluator.json`). Snow refreshes 98 connected-pose batches and calls 24 single-chain IK constraints per evaluation. Its unchanged replay uses the first-frame and authoritative snapshot caches (about 0.32 and 0.07 ms per evaluation), yet the full replay still cost about 124 ms. A CPU sample (`Snow-cpu-sample.txt`) placed 815 of 1,205 evaluator samples under connected-provider refresh, including substantial ordered `SdfPath` set insertion/comparison work outside the named solver scopes. This identifies a measured runtime cost, not merely a large package.

The sibling `usdRig/libs/rigExec/rigEvaluatorDynamic.cpp` now uses hash sets for membership-only connected refresh sets and indexes immutable frame overrides once per evaluation. Each provider selects its original override indices and sorts those indices, preserving traversal order, duplicate precedence and numerical operations. The removed-frame closure check uses the index instead of constructing a large complement set for every provider. No controls, expressions or picker targets were deleted.

**Correction to the standalone root profiles:** the original name-only lookup selected hidden `META-Snow` construction root `/Rig/Controls/root_2b5ff16e/Control`, not the visible animator root `/Rig/Controls/root_841143c9/Control`. The recorded 124.12 → 40.38 ms replay and 151.41 → 66.78 ms override measurements describe that construction control and must not be used as animator-root drag evidence. The viewer benchmarks select the correct visible control and remain valid. The corrected standalone harness checks guide visibility and records the exact driven path. Both indexed native iOS library and signed app built successfully; their desktop measurements are in `after-index/` and `indexed-viewer-summary.json`.

Post-change viewer native-preview timings (median / p95 ms):

| Rig | Root/body | Left hand | Left foot |
| --- | --- | --- | --- |
| Biped | 23.05 / 24.86 | 14.31 / 14.70 | 13.83 / 14.80 |
| Delta | 118.93 / 124.27 | 59.30 / 63.44 | 65.06 / 66.79 |
| Gamma | 130.73 / 135.08 | 73.63 / 75.50 | 74.52 / 75.67 |
| Snow | 145.16 / 148.53 | 92.62 / 107.09 | 84.91 / 101.12 |

All four viewer interaction reports pass, including Gamma tail/foot mesh motion and Snow tooth/gum root following. In the final run camera medians were about 17 ms (p95 21–24 ms), with unchanged generations and no warming; the earlier run measured 7–8 ms. These end-to-end Qt/paint timings include display scheduling variability. Snow root manipulation improves substantially but still exceeds a 33 ms frame budget; this is a measured improvement, not complete interactive-performance acceptance. `indexed-viewer-summary.json` contains the final input-to-paint and native timing distributions; `viewer-before-after.json` records the intermediate hash-only run.

The four focused runtime tests (constraints, baked parity, imaging, and non-authoring) and all 11 converter CTests pass. All 41 production pose measurements are identical before/after this optimization (`hash-pose-equivalence.json`, `indexed-pose-equivalence.json`). The runtime improvement does not make numerical Blender parity pass.

### Drag-performance follow-up: deployed and physically verified

The shared runtime now avoids redundant work without changing the authored rig:

- Connected-pose batches reuse validated results, and seed comparisons/closure walks reuse per-evaluation work.
- Space switches with no downstream consumers patch their exact seed outputs instead of reevaluating the complete seed request.
- Authoritative snapshots use frame outputs already pinned by the pose walk and the native rest-to-pose matrix operation; residual outputs still execute through stock OpenExec. Overlap workers retain their original evaluation path.
- Connected refresh requests use **16 bounded execution partitions**, plus a seed partition. Stock OpenExec invalidates downstream nodes outside the current request; partitioning limits that fan-out. An executor-per-output prototype was rejected because it duplicated too much upstream state. All partitions participate in prim-removal retirement, and supplied-result tests cover overrides, time changes, authored rest changes, and remove/redefine.

These changes are in the sibling runtime's `rigEvaluatorDynamic.cpp`, `rigEvaluatorCompile.cpp`, and `tapSet.{h,cpp}`, with regression coverage in `testRigExecDefaultSpaces.cpp`. OpenUSD itself is unchanged. Drag preview remains non-authoring; release commits retain their existing behavior.

A controlled Snow comparison reran the original runtime from the start of this follow-up and the final runtime with the same current stage, viewer, imaging library, disabled profiling, and 24 measured moves per control. Evidence: `build/drag-perf/comparison.json`, `controlled-baseline/Snow/`, and `final-viewer/Snow/`.

| Snow control | Before native median (ms) | Final native median (ms) | Reduction | Final input-to-paint median (ms) |
| --- | --- | --- | --- | --- |
| Visible animator root | 148.63 | 83.38 | 43.9% | 104.00 |
| Left hand IK | 93.73 | 55.48 | 40.8% | 83.00 |
| Left foot IK | 84.66 | 50.95 | 39.8% | 72.86 |

Final desktop native medians for root/body, hand, and foot respectively are Biped **23.76 / 14.62 / 14.21 ms**, Delta **67.96 / 43.78 / 46.09 ms**, and Gamma **79.10 / 51.78 / 53.90 ms**. Gamma tail is **51.18 ms**. All four viewer interaction reports pass and their source layers remain unchanged (`build/drag-perf/viewer-summary.json`). The corrected active-root standalone evaluator measures **99.52 → 66.87 ms** between the intermediate shared-context runtime and final partitioned runtime; this is distinct from the full controlled viewer comparison. Its authored-edit median changes **64.07 → 73.32 ms**, an explicit tradeoff rather than a drag measurement.

The rebuilt and signed runtime is installed on **BD1**. The instrumented ordinary controller benchmark passes, measuring Snow root/hand/foot native medians of **82.69 / 52.78 / 48.19 ms**, with input-through-submitted-redraw medians **92.31 / 61.39 / 57.58 ms** at 2716 × 1455 pixels. It excludes display scanout and is separate from real touch input. `testBlenderSnowRootControl` and `testBlenderGammaTailControl` also pass on the same build, including camera/rig-generation isolation and actual body-mesh deformation. Device evidence is in `build/drag-perf/device-Snow/`, `device-performance-test.log`, and `device-touch-tests.log`.

All **14 focused native tests** pass, including interactive and baked parity, frame caches, rest epochs, skin topology, imaging, non-authoring, and supplied snapshots. All **11 converter tests** also pass (`build/drag-perf/converter-tests.log`). All **41 production pose comparisons are identical** to the indexed baseline (`build/drag-perf/final-pose-equivalence.json`); this does not resolve the preexisting Blender parity errors.

The drag hot spots are reduced, but the **33 ms target remains open** for the production Blender rigs. Device release-to-redraw also remains **0.52–0.86 seconds** in this capture, versus approximately **87 ms** desktop release timings; release latency is not included in the per-move figures above. The remaining measured per-move cost is primarily connected-expression/seed evaluation in stock OpenExec, followed by frame assembly and publication. No package content, expressions, or hidden construction rigs were removed to obtain these improvements.

## 3. Motion parity: expanded audit, residual failures explicit

Re-ran the full Delta 12 / Gamma 15 / Snow 12 pose comparisons, then added focused Gamma eye-target and upper-eyelid translation probes in `tests/captureRigReference.py`, captured a fresh independent Blender reference, and compared all 17 Gamma poses. After the runtime optimization, all 41 pose results exactly match their pre-optimization measurements. Every sampled mesh with more than 0.01 units of source displacement still has native displacement; that motion gate is weaker than numerical parity.

The numerical threshold remains **maximum vertex error ≤ 0.00002 scene units**, with matching topology/UVs and valid required frames. No tolerance was loosened. This gate still fails. Representative residual maximum errors:

| Rig / probe | Error |
| --- | --- |
| Delta saved mesh | 0.0000926 |
| Delta left / right foot | 0.01430 / 0.01564 |
| Gamma saved main mesh | 0.00918 |
| Gamma left / right hand | 0.06007 / 0.01505 |
| Gamma left / right foot | 0.03706 / 0.04110 |
| Gamma torso / tail | 0.13805 / 0.02841 |
| Gamma head, pupil mesh | 0.06044 |
| Gamma eye-target translation, pupil mesh | 0.00344 |
| Snow saved head mesh | 0.00907 |
| Snow left / right hand | 0.01651 / 0.01484 |
| Snow left / right foot | 0.01684 / 0.01679 |

The eye-target probe produces native motion close in magnitude to Blender, but pupil placement differs; the eyelid translation probe has no source mesh motion and cannot establish support. Snow's all-mesh root/torso maxima (0.21449 / 0.24705) occur on `GEO-snow-eyes`, which the extracted source marks viewport-invisible but render-visible. Retain those errors rather than conflating them with the visible body or silently dropping that mesh from the audit.

Diagnostics identify additional unsupported or approximate behavior beyond the three-hook tail: Gamma's pupil Shrinkwrap and four-hook Spline IK; Delta's eyelid Locked Track constraints; B-Bone segment deformation (204 Delta, 80 Gamma, 113 Snow diagnostics, including construction rigs); shape keys and driven animation retained at the saved frame; Snow Corrective Smooth, geometry-node modifiers, Floor, some limit constraints, and lattice/armature constraint cases. Snow Surface Deform uses approximate cage-to-bone weights for four tooth/gum meshes. Native IK, Stretch To volume, and preserve-volume skinning are not exact Blender equivalents. These require targeted converter/native feature work; hiding warnings or pruning construction rigs is not a parity fix. The next highest-displacement supported-control investigations are Gamma torso/hand and head/pupil, then Snow corrective deformation and Delta foot/torso.

## 4. Snow texture cache: stale runtime fixed, physical test passes

The failure was independent of rig loading/deformation. Detailed diagnostics showed successful fixture creation, successful reads, and identical pixels, but **no cache file in any of the four cases**. The installed device `libusd_hio.dylib` lacked the decoded-cache code already present in `scripts/patches/hio-stb-ios-read-safety.patch`; the prepared patched Hio build did contain it. Installing that library and rebuilding/re-signing the app fixed the diagnostic.

`testSnowTexturesLoadProgressively` passed on physical BD1 in `/tmp/cache-fixed-test.log`, including resize/flip/crop cache equivalence, cache creation, deliberate corrupt-cache recovery, texture loading completion, and a camera gesture while cold textures were loading. The copied device report `texture-cache-parity.json` has `passed: true` with all four detailed cases passing. The debug diagnostic now reports individual failure stages instead of only one aggregate Boolean, and checks the fixture's source dimensions on every open. The final indexed-runtime run also passes `testSnowTexturesLoadProgressively` (`device-final-tests.log`). The texture UI test now checks that loading is active immediately before the gesture, instead of requiring it to remain active afterward: faster completion is valid, and taking a screenshot before the gesture consumed the remaining load interval. Future iOS rebuilds must apply the existing Hio patch through `scripts/build-usd-ios.sh` and embed the resulting library; rebuilding Swift alone cannot update it.

## Package and verification baseline

USDZ SHA-256, rechecked unchanged:

- Delta: `f40a05b7052c199212d7aa4f49aa80031f6419e4fd35d7bd5cac1e9bcdf93f4d`
- Gamma: `4851e4b424497a8d2b79d49b974b74d02595921c71ff06def4a6981b83c16713`
- Snow: `556caf0588a5917e77b878abdd30fd91eafefe99ffbc8786438c4013f0ddfed4`

The converter changes were not reverted or replaced, and the package inventories remain unchanged. This environment can run Blender, USD, and CTest; the prior sandbox-crash limitation no longer applies. Device lock state is a separate, intermittent blocker. Preserve the distinction between a passing mesh-motion test, unchanged numerical errors, improved performance, and full acceptance.
