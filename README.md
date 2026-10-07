# usdBlenderRig

A read-only OpenUSD `SdfFileFormat` plugin that opens Blender `.blend` files as
native usdRig rigs. It follows the standalone sidecar pattern in
[`../usdRig_maya`](../usdRig_maya/README.md) and links to
[`../usdRig`](../usdRig/README.md)'s installed `rigExecRigging`, `rigExec`, and
`rigExecMath` libraries.
CMake consumes the installed dependency SDK package.

Blender reads its own binary format in a separate headless process and emits
a versioned JSON snapshot. The C++ translator authors native RigExec schema
prims. Blender does not need the usdRig Python module, and its bundled USD
version does not need to match the host's USD. Exported USD evaluates using
usdRig and this sidecar's native computation plugin. `.blendrig` snapshots
also open directly without Blender. Exported rigs contain no sampled playback
substitute and never launch Blender while evaluating a pose.

This is a supported-subset rig translator. It does not reproduce Blender's
entire dependency graph. Inspect diagnostics or use strict mode before relying
on a production rig.

## Build

Build usdRig against OpenUSD 26.08 with OpenExec first, following its README,
then install its package:

```sh
cmake --install ../usdRig/build --prefix ../usdRig/build/install
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUSDBLENDERRIG_BLENDER=/path/to/Blender.app/Contents/MacOS/Blender
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix build/install
```

CMake automatically finds the sibling's `build/install` package. For a
different install, pass `-DrigExec_DIR=/path/to/lib/cmake/rigExec`.
`USDBLENDERRIG_BLENDER` is the configure-time executable default; the environment
variable of the same name overrides it at runtime. It must name the executable,
not the `.app` bundle. If omitted, the reader searches `PATH` for `blender`.
Set `-DUSDBLENDERRIG_TEST_BLENDER=OFF` to run only the snapshot/native tests on
a machine without Blender. macOS and Blender 5.2 have been exercised; Windows
and Linux process launch paths are implemented but have not been exercised here.

## Load and convert

Register both the sidecar and RigExec schemas, using the same USD environment
and Python interpreter as usdRig:

```sh
export PXR_PLUGINPATH_NAME="$PWD/build/usd/usdBlenderRig/resources:$PWD/../usdRig/build/install/lib/usd/rigExecSchema/resources${PXR_PLUGINPATH_NAME:+:$PXR_PLUGINPATH_NAME}"
export USDBLENDERRIG_BLENDER=/path/to/blender
python tools/convert.py character.blend character.usdc
python tools/convert.py character.blend character.usda --strict
```

Use the installed resource directory
`build/install/lib/usd/usdBlenderRig/resources` when deploying the sidecar.
USD and RigExec shared libraries must remain discoverable by the platform
loader. After installing this sidecar, launch `bin/usdview.sh character.usdz`.
This loads the SDK selected by CMake and the Blender control companion together.
The configured SDK prefix is recorded in `build/rigexec-prefix`; set
`USDBLENDERRIG_RIGEXEC_PREFIX` to override it when testing another installation.
The companion maps shape selections to their owning bones and uses native
evaluated bone frames when converting gizmo drags into Blender pose channels.
Static converted rigs skip background warming of identical frames; live drag
evaluation remains enabled, and authored avar animation restores normal warming.

The sibling `usdRig_ios` build includes this sidecar's `nativeFrames.cpp` and
`nativeSkin.cpp` in its native runtime and embeds the `UsdBlenderRig` schema
resources. Rebuild that runtime before opening exported USDC/USDZ rigs on iOS;
the mobile viewer does not need Blender to evaluate them. Its gizmo uses the
published `blender:editorFrame` contract for editable channels, and the picker
page row scrolls to preserve access to long Rigify/CloudRig page lists.

```python
from pxr import Sdf, Usd

stage = Usd.Stage.Open("character.blend")
print(stage.GetDefaultPrim().GetAttribute("blender:diagnostics").Get())
stage.GetRootLayer().Export("character.usdc")

# strict=1 rejects every diagnostic; strict=0 is the default.
layer = Sdf.Layer.FindOrOpen("character.blend", {"strict": "1"})
```

The source layer is read-only. Pose edits belong in a USD session layer or an
overlay, or in the exported USD. Controls/joints expose `avars:tx`, `avars:ry`,
`avars:sx`, etc. `blender:id` custom data identifies the original object/bone;
display names preserve Blender names, while prim identifiers use compact labels
with deterministic collision suffixes. Bone custom shapes are display-only child
controls named `Display`.

For a reusable Blender-free snapshot:

```sh
blender --background --factory-startup --disable-autoexec --python-exit-code 1 \
  --python blender/extract.py -- character.blend character.blendrig
```

Reads have a 120-second Blender-process timeout and a 512 MiB snapshot limit.
Hierarchy and constraint dependency depths are limited to 512 levels.
Each read uses a private temporary directory, removes it on completion, and
transfers the translated content only after the conversion succeeds. `.blend`
reads require a resolved local file; custom resolver assets that exist only
in memory or an archive need to be materialized first. Metadata-only `.blend`
reads still need Blender; their `complete` flag is false.

## Translation contract

| Blender data | Native USD result |
| --- | --- |
| Active scene's object hierarchy | Nested `RigExecControl` providers under `/Rig/Dag`; parent-inverse placement becomes `rest:space`, saved object basis becomes editable TRS avars |
| Armature bones, head frames and roll | Nested `RigExecJoint` providers; local bind matrices preserve bone orientation and length metadata |
| Saved bone pose | Source translation/scale and XYZ rotation avars; other source rotation modes are converted to XYZ |
| Bone inheritance | Full, none, average, aligned, fix-shear and legacy-none scale modes, optional rotation inheritance and nonlocal translation through native Exec expressions |
| Object bone parenting | Tail-relative or relative-rest parenting, following native bone pose |
| Copy Location | Native expression stacks with influence, WORLD/POSE/LOCAL/CUSTOM owner and target spaces, LOCAL_OWNER_ORIENT targets, axis masks, inversion, offset and simple bone head/tail targets; CUSTOM uses a live object or bone frame |
| Copy Rotation | Full-axis REPLACE/BEFORE/AFTER with influence in WORLD/POSE/LOCAL expression stacks and LOCAL_OWNER_ORIENT targets; all axes disabled is also supported; partial Euler masks and other mixing modes remain diagnosed |
| Copy Scale | WORLD/POSE/LOCAL expression stacks and LOCAL_OWNER_ORIENT targets with influence, masks, power, uniform scale, multiplicative or legacy additive offset |
| Copy Transforms | REPLACE, BEFORE/AFTER, FULL and SPLIT mixing in WORLD/POSE/LOCAL/CUSTOM expression stacks and LOCAL_OWNER_ORIENT targets, optional target shear removal and connected-bone origins; partial influence uses world-space polar blending |
| Stretch To | World stacks with influence, PLANE_X/PLANE_Z/SWING_Y orientation, volume modes and smooth bulge limits; almost zero-length stretches are diagnosed and retain the incoming frame |
| Damped Track | World stacks with influence, all six signed axes and half-turn handling |
| Limit Rotation | World stacks with influence and all angle limits disabled, reproducing Blender's shear removal; enabled angle limits remain unsupported |
| Armature constraint | World stacks with influence, weighted targets, linear or dual-quaternion blending and rest/current pivots, without envelopes; preserves owner avars and live armature placement. B-Bone targets use the whole-bone frame with an explicit segment-binding diagnostic |
| Constraint order and dependencies | Source stack order, parent-before-child dependencies, explicit cycle rejection |
| IK without stretch/rotation targeting | `RigExecSingleChainIkConstraint`, optional pole provider and a virtual bone-tail joint; solver equivalence diagnosed |
| Armature vertex-group skinning | `RigExecSkinMover`, used deform-bone palette, normalized weights, live mesh/armature spaces, zero-weight vertices following their owning object |
| Masked/chained armatures | Native `RigExecBlenderArmatureMover` revisions; fractional/inverted masks and original-input caching for contiguous multi-modifier chains |
| Preserve Volume | Native dual quaternion skinning, with a Blender-parity diagnostic |
| Meshes | `UsdGeomMesh` control cages with `subdivisionScheme = "catmullClark"` assumed for every mesh, asset-space bind points, extent, render-active face-varying `st` and all named UV sets; editing/render UV selection metadata is retained |
| Constant topology modifiers | Pre-deformation Mirror and nonsmooth vertex-group Mask materialized on undeformed bind data; no pose or shape-key geometry captured |
| Unskinned meshes | Native matrix mover following the owning object control |
| Bone custom shapes | Mesh-edge or POLY-curve guides, source shape transforms, rest-bone size, alternate shape bone, source colors/visibility and thin hairline drawing; helper geometry is omitted |
| Materials | `UsdPreviewSurface` color, opacity, roughness, metallic, emission and tangent normals; packed/file/UDIM images, named UVs and supported static texture arithmetic are converted to portable PNG assets |
| Material slots | Whole-mesh binding or material face subsets |
| Rigify UI collections | Native `RigExecPicker` pages in source UI row order, including hidden collections, with bone selection buttons |
| CloudRig UI panels and collections | Every stored panel, conditional binding and nested/hidden collection preserved; native selection buttons target the corresponding bones |
| Linked libraries and file textures | Flattened loaded scene data, anchored asset dependencies; files are referenced rather than copied |
| Scene units and frame rate | Z-up, meters-per-unit and time-code metadata |

Object channels are captured at the saved frame as editable TRS avars.
Bone pose channels remain relative to their original bind frames. Translation
does not export animation curves; all animation/driver/NLA data is diagnosed.
Prim names use compact Blender labels truncated to 32 characters. Numeric
starts receive an `n_` prefix; duplicate or sanitized labels receive
deterministic identity suffixes when they collide;
full source identifiers remain in `blender:id` and source metadata. Relationships,
connections and every picker target use the resulting paths. UV property names
retain their reversible encoding, preserving distinct named UV sets.
Mesh bind points include saved object placement; native expressions account
for live object/armature placement during deformation. Bind topology is fixed;
editing Mirror/Mask parameters or their driver inputs is not supported.
Supported constraint subsequences retain source order when other operations
are unsupported. Every omitted operation is diagnosed, and the conversion
remains incomplete. Missing upstream operations can change downstream results
substantially. CUSTOM vertex-group spaces remain unsupported.

The material adapter follows nested groups, reroutes, UV maps, identity mapping,
constant values, common Mix/Math operations and tangent Normal Map nodes.
Supported texture arithmetic is resolved into cached image assets, with source
color-space conversion, raw data channels and source content fingerprints.
This processes static textures only; rig evaluation has no sampled fallback.
Unsupported procedural nodes, nonidentity mapping, Bump height derivatives,
ray-dependent shader mixes, subsurface, coat, sheen and transmission are
diagnosed. Preview materials therefore do not certify Blender shader parity.
The content-addressed cache defaults to `~/Library/Caches/usdBlenderRig`;
`USDBLENDERRIG_ASSET_CACHE` overrides it. USDZ includes the derived textures.

## Picker pages and USDZ

Rigify/CloudRig sidebar data is adapted to a three-column native picker layout.
All source pages are retained, including hidden and empty collections. CloudRig
panels retain their order, labels, nested conditional branches and property
bindings as inert metadata; buttons select the associated controls. Source
settings switches and Python operators are not executable picker actions.
Embedded UI text is preserved without running it. Separate third-party picker
formats are not currently imported.

Package an exported native layer and its resolved asset dependencies with:

```sh
python tools/package.py character.usdc character.usdz
```

This uses OpenUSD's general USDZ packager and verifies ZIP alignment/CRC,
dependency resolution, byte-identical concrete UDIM tile sets, native prim types and computation connections, every
picker page and control target, and unchanged source bytes. A sibling
`character.package.json` records the result and SHA-256. Native evaluation and
the picker UI require usdRig and this sidecar plugin; packaging does not add
sampled playback or certify Blender parity.

## Diagnostics and limits

`/Rig.blender:diagnostics` and layer `blenderRig:diagnostics` report unsupported
features and known approximations. `blenderRig:complete` means a full read
produced no diagnostics; it is not a general Blender-equivalence claim.
`blender:sourceData` preserves the extracted snapshot (including constraint
settings, topology, smooth flags and modifiers' diagnostics) as inert JSON.
Each provider also retains its extracted source record. Dependencies are
available through `GetExternalAssetDependencies()` and layer custom data.

Unsupported features include shape keys, arbitrary modifiers, bone envelopes,
multi-armature caches following unsupported modifiers, B-Bone segments,
object vertex parenting, CUSTOM spaces, LOCAL_OWNER_ORIENT owner spaces,
non-WORLD spaces for non-copy constraints, partial rotation masks, rotation
offset/inversion/ADD mix modes,
enabled rotation limits, unsupported constraint types, custom shape
transform bones, non-POLY curve geometry, custom split normals, and procedural
or packed textures. Rest scale/shear/reflection is retained but diagnosed
because RigExec orthonormalizes bind frames. Smooth shading requires host normal
recomputation and is diagnosed. Principled materials are a preview subset;
unsupported connected inputs and the listed coat/sheen/transmission/subsurface/
emission values are diagnosed.

Stretch distances below `1e-6 * max(1, rest_length)` retain their incoming
frames. Collapsed transforms cannot be represented by invertible RigExec
providers; this guard prevents downstream evaluation failure and is a
conversion diagnostic, including in strict mode.

The custom armature mover has no `.rigexec` binary-export kernel or separate
RigExec scalar oracle. Blender comparisons exercise its native USD evaluator;
binary export must reject it rather than silently substitute another mover.

Embedded Python is disabled during file loading. Drivers/expressions are not
translated into code, and embedded text blocks are diagnosed. Process arguments
are passed directly to Blender, without a shell. Source `.blend` files are
never saved by the reader. These controls follow Blender's documented
[command-line arguments](https://docs.blender.org/manual/en/5.1/advanced/command_line/arguments.html).

## Validation

`fileFormat` tests actual USD plugin discovery without linking the reader,
strict mode, native types, duplicate IDs, malformed topology, hierarchy and
constraint cycles, missing/duplicate picker owners and control targets,
failure atomicity and USDA serialization.
`evaluation` tests native control → constraint → mixed-weight skin deformation
and reset. `blenderFixture` creates a compressed real Blender rig with rolled
bones, distinct mesh/armature placements, UVs, materials and a custom shape.
`blenderParity` compares saved pose, translated control, bone rotation, bone
scale and reset against independently evaluated Blender mesh points, then
exports and reopens native USD. The tolerance is 0.00002 scene units.
`snapshotParity` repeats those comparisons through the portable snapshot;
`incrementalPoseParity` resets and edits channels without clearing a layer or
recompiling, to cover ordinary interactive invalidation.
`nativeUsdParity` runs them in a fresh process against the exported USD, with
the Blender executable set to an unavailable path.
`blenderRead` verifies compressed-file probing, strict and metadata reads,
unchanged source bytes, atomic missing-file/executable failures, and disabled
embedded text execution.

Generated Blender sources, reference measurements and exported USD live in
`build/blender-proof/`; the supported fixture generator is checked in. A
passing generated fixture is evidence for these cases, not for arbitrary
Rigify or production rigs. Production acceptance requires passing the same
independent Blender comparison on the asset itself.

For a production asset, with NumPy installed in the host USD Python:

```sh
python tools/validate.py character.blend build/character-proof --blender /path/to/blender
```

This reads the source without saving it, captures the saved pose, available
root/hand IK/arm FK/foot/head/jaw/torso control edits, and reset, exports `native.usdc`,
and compares native point positions, topology, render UVs and bone frames at
0.00002 scene-unit tolerance. Source SHA-256, invalid source drivers, skipped
controls, unsupported features and per-pose errors are recorded. Displacement
measurements distinguish saved-shape errors from live control-response errors,
and expose control edits that do not move source geometry at the saved settings. Native
evaluation runs in a fresh process with Blender unavailable. A mismatch exits
with status 1; inspect `native-report.json` and `compare.log`. Reference/native
NPZ measurements are test evidence; no sampled USD playback is generated.
Material/shader rendering and computed viewport normals require separate
visual acceptance and are not proved by these numerical checks.

The generated fixtures additionally exercise fractional complementary skin
masks, cached input across multiple armatures, Mirror/Mask bind topology,
relative/tail bone parents, all six inheritance modes with rotation enabled
and disabled, nonlocal translations, connected bones, and owner/object edits
through an Armature constraint. `dualQuaternionParity` and
`nativeDualQuaternionParity` compare opposing bone rotations and a masked
LBS-to-DQ chain against Blender, including fresh-process native USD reopening.
Constraint probes cover all stretch plane/volume combinations, bulge bounds,
six signed tracking axes, scale masks/power/uniform/offset modes, shear
removal, fractional influence, rotation BEFORE/AFTER mixing, a connected
midpoint-to-stretch chain, and
WORLD/POSE/LOCAL/CUSTOM copies with LOCAL_OWNER_ORIENT targets across differently
placed armatures with posed parents. Collapsed
stretch guards and recovery have separate native validity checks; those
unsupported collapsed cases are not claimed to match Blender.

After the Blender fixture tests, run the picker/package regression with the
same USD/plugin environment:

```sh
python tests/testPickerPackaging.py build/blender-proof build/picker-proof
```

This also verifies two concrete UDIM tiles and rejects an empty tile pattern.
To exercise materials, picker loading, visible shape selection, world-channel
conversion, a Qt mouse drag and rendered pixels in the real viewer:

```sh
USDBLENDERRIG_USDVIEW=/path/to/testusdview \
USDBLENDERRIG_VIEW_PROOF=/tmp/blender-viewport \
bin/usdview.sh --testScript tests/testUsdviewBlender.py character.usdz
```

The test selects the visible root control, excluding hidden construction rigs.
It verifies that control and the largest visible mesh, not every control or
modifier in a production rig. Full native production comparisons remain
separate; `tests/compareRigReference.py --incremental` checks ordinary channel
edits against an existing independent Blender reference.

For a desktop speed comparison and parity check of the generation usdview
actually displays, build the test-only snapshot inspector and run:

```sh
cmake --build build --target usdBlenderRigViewSnapshot
source ../usdRig/bin/_env.sh
USDBLENDERRIG_USDVIEW="$TESTUSDVIEW" \
USDBLENDERRIG_VIEW_PROOF=/tmp/rig-desktop-comparison \
USDBLENDERRIG_VIEW_REFERENCE=/path/to/reference.json \
RIGEXEC_IMAGING_PROFILE=1 \
bin/usdview.sh --testScript tests/testUsdviewRigComparison.py character.usdz
```

Run viewers sequentially. This uses a fixed 1280 by 960 physical viewport,
Storm, scene materials and the default low refinement setting. It sends real
Qt gizmo mouse events for root/body, left hand IK and left foot IK, with two
warm-up gestures followed by 24 measured moves per control. The report separates
native preview/publish time, event handling and input through viewport redraw;
the redraw measurement includes queued UI work and can include multiple paints.
It excludes physical display scanout. Biped's hand IK switch is enabled for its
hand workload and restored afterward. Omit `USDBLENDERRIG_VIEW_REFERENCE` for a
native baseline such as `../usdRig/examples/biped/Biped_stack.usda`.

For physical iPad comparison, `tools/compare_ipad.py` uses the same desktop
workloads and independent Blender references. It requires the configured
OpenUSD Python environment and NumPy:

```sh
"$PY" tools/compare_ipad.py prepare build/compact-proof/Gamma.usdz \
  build/usdview-comparison/Gamma.json build/ipad-comparison-fixtures \
  --reference build/revision-proof/gamma-reference/reference.json
```

Copy the manifest and `Blender-Gamma.usdz` to the app's Documents directory.
The iOS Debug launch argument `-ipadRigComparison Gamma.json` records the
terminal Hydra scene index's float32 mesh points, published asset-space
frames, screenshots and control timings in `Documents/RigComparisonOutput/Gamma`.
The physical device must be unlocked. The driver refuses simulator capture.
`AppUITests/testPhysicalRigComparisonCapture` accepts
`RIGEXEC_IPAD_COMPARISON_MANIFEST=Gamma.json`; run the three Blender picker
drag tests separately for ordinary XCUITest touch acceptance.

After copying the output directory back, compare it without evaluating a rig:

```sh
"$PY" tools/compare_ipad.py compare \
  build/ipad-comparison-fixtures/Gamma-metadata.json \
  build/ipad-comparison/Gamma build/ipad-comparison/Gamma.json
```

Native preview timing includes buffer packing, evaluation and publication.
Redraw timing ends at the next submitted Metal render, with GPU readback
outside measured moves. Desktop Qt paint and iPad render submission have
different timing boundaries and viewport sizes; the report keeps those
separate. Blender parity compares every captured vertex and bone frame.
For direct desktop versus device arrays, run the desktop test with
`USDBLENDERRIG_VIEW_CAPTURE_POINTS=1` and optionally
`USDBLENDERRIG_VIEW_POSES_ONLY=1` to omit timing workloads. Pass its proof
directory to `compare --desktop-captures <directory>`. Captures are numerical
evidence; they do not author sampled playback or change the parity tolerance.
Desktop equivalence compares the error/displacement metrics, rather than
claiming direct vertex-array identity. Captures are measurement files, and
the native USDZ files have no sampled playback added.

Mesh points and bone frames come from the viewer's stage-scoped immutable
snapshot, with authored points used only for meshes without a published override.
There is no second evaluator in this viewer test. It replays independent Blender
control edits and compares cage points, topology, UVs and frames using the
reference's unchanged tolerance. Screenshots and profile TSVs accompany the
JSON. `interaction_passed` checks control-frame movement and publication;
`parity_passed` separately reports complete pose correctness. A completed
comparison can report failed parity, and does not certify Blender shader or
subdivision-limit surface parity. Every edit is confined to the session layer.

It checks both Rigify and CloudRig pages, hidden/nested control membership,
conditional bindings, native control links, and a background image packaged
byte-for-byte with no authored time samples.

Native numerical conventions and algorithm sources are documented in
[references](docs/references.md).
