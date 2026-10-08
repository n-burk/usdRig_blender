# usdBlenderRig

A read-only OpenUSD `SdfFileFormat` plugin that opens Blender `.blend` files as
native usdRig rigs. It follows the standalone sidecar pattern in
[`../usdRig_maya`](../usdRig_maya/README.md) and links to
[`../usdRig`](../usdRig/README.md)'s installed `rigExecRigging` and `rigExec` libraries.
CMake consumes the installed dependency SDK package.

Blender reads its own binary format in a separate headless process and emits
a versioned JSON snapshot. The C++ translator authors native RigExec schema
prims. Blender does not need the usdRig Python module, and its bundled USD
version does not need to match the host's USD. Exported USD evaluates using
usdRig alone. This repository supplies file-format translation only; runtime
computations, movers, and their schemas are shared core features. `.blendrig` snapshots
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
loader. The converter plugin is needed only when opening `.blend` or
`.blendrig` source files. Exported USDC/USDZ rigs, their pickers, and their
computations run in the ordinary usdRig desktop or iOS runtime without this
repository's plugin or Blender. The helper `bin/usdview.sh` registers the
file-format plugin only for source files and otherwise selects the SDK recorded
in `build/rigexec-prefix`; set `USDBLENDERRIG_RIGEXEC_PREFIX` to test another SDK.
Reconvert older exports to obtain current native picker controls. Existing exports using the former runtime type names must be upgraded with
`tools/migrate_runtime.py`; core contains no converter-specific aliases. New exports
use the shared `RigExecBoneFrame`, `RigExecConstraintFrame`, `RigExecCopyFrame`,
`RigExecMappedFrame`, `RigExecSkinInfluence`, `RigExecArmatureParent`, and
`RigExecLayeredSkinMover` types.

```python
from pxr import Sdf, Usd

stage = Usd.Stage.Open("character.blend")
print(stage.GetDefaultPrim().GetAttribute("blender:diagnostics").Get())
stage.GetRootLayer().Export("character.usdc")

# strict=1 rejects every diagnostic; strict=0 is the default.
layer = Sdf.Layer.FindOrOpen("character.blend", {"strict": "1"})
```

The source layer is read-only. Pose edits belong in a USD session layer or an
overlay, or in the exported USD. An editable Blender bone has a native
`/Rig/Controls/<name>/Control` prim with nine TRS avars. Its original
`RigExecJoint` remains the evaluated bone and exposes `blender:channelControl`
to locate the editable prim. `blender:id` identifies the original object/bone;
`blender:sourceId` identifies its channel control. Display names preserve Blender
names, while prim identifiers use compact labels with deterministic collision
suffixes. Custom shapes live on the editable control. An alternate Blender
shape-transform bone is retained as `guide:source`, so drawing follows that
bone while viewport picking and the picker select the animation control.

Controller wires have a positive diameter derived from the deforming armature
extent and Blender's source pixel width. The original pixel width is retained
as metadata. Legacy snapshots with zero wire width use the same extent rule.
Generated IK endpoint joints have their guides hidden, so their schema-default
radius cannot cover the character. Texture file attributes carry the authored
color space as metadata in addition to the UsdUVTexture input.
Generated rigs enable `rigExec:baked` by default to request compiled
evaluation. This keeps controls editable; it does not bake animation into
time samples. Unsupported computations may still require dynamic evaluation,
so the flag alone does not guarantee reduced memory or Blender parity.
Previously exported USD files must be reconverted to pick up this default.
Generated rigs also opt into usdRig connected-pose seed reuse. Their declared
provider and attribute inputs allow refreshes to pin current dependency frames
while avoiding repeated invalidation of the whole execution network. The
runtime preserves complete override reads for rigs that do not opt in.

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
| Saved bone pose | Native `RigExecControl` TRS avars drive the original Blender bone computation through standard USD connections; a native `RigExecSpaceSwitch` or explicit `rigExec:channelSpaces` preserves the Blender channel bases for manipulation |
| Bone inheritance | Full, none, average, aligned, fix-shear and legacy-none scale modes, optional rotation inheritance and nonlocal translation through native Exec expressions |
| Object bone parenting | Tail-relative or relative-rest parenting, following native bone pose |
| Copy Location | Native expression stacks with influence, WORLD/POSE/LOCAL/CUSTOM owner and target spaces, LOCAL_OWNER_ORIENT targets, axis masks, inversion, offset and simple bone head/tail targets; CUSTOM uses a live object or bone frame |
| Copy Rotation | Full-axis REPLACE/BEFORE/AFTER with influence in WORLD/POSE/LOCAL expression stacks and LOCAL_OWNER_ORIENT targets; all axes disabled is also supported; partial Euler masks and other mixing modes remain diagnosed |
| Copy Scale | WORLD/POSE/LOCAL expression stacks and LOCAL_OWNER_ORIENT targets with influence, masks, power, uniform scale, multiplicative or legacy additive offset |
| Copy Transforms | REPLACE, BEFORE/AFTER, FULL and SPLIT mixing in WORLD/POSE/LOCAL/CUSTOM expression stacks and LOCAL_OWNER_ORIENT targets, optional target shear removal and connected-bone origins; partial influence uses world-space polar blending |
| Stretch To | World stacks with influence, PLANE_X/PLANE_Z/SWING_Y orientation, volume modes and smooth bulge limits; almost zero-length stretches are diagnosed and retain the incoming frame |
| Damped Track | World stacks with influence, all six signed axes and half-turn handling |
| Limit Rotation | World stacks with influence and all angle limits disabled, reproducing Blender's shear removal; enabled angle limits remain unsupported |
| Armature constraint | Bone and object world stacks with influence, weighted targets, linear or dual-quaternion blending and rest/current pivots, without envelopes; preserves owner avars and live armature placement. Object owners bind at their incoming origin, including lattice cages that follow a skinned mesh. B-Bone targets use the whole-bone frame with an explicit segment-binding diagnostic |
| Constraint order and dependencies | Source stack order, parent-before-child dependencies, explicit cycle rejection |
| IK without rotation targeting | `RigExecSingleChainIkConstraint`, object-pole mode, bone-basis pole angle and a hidden virtual bone-tail joint; the constraint stretch flag and per-bone stretch values are retained; nonuniform stretch and solver equivalence are diagnosed |
| Three-hook Spline IK | A native `RigExecSplineIk` follows the three hooked bone controls and maps its virtual chain back to the Blender bones. Hooked Bezier shape, twist and scale are approximate; other Spline IK configurations remain diagnosed |
| Connected lower-limb IK chains with inert native targets | The translator probes a foot IK control, then maps its affected connected deformation bones through calibrated native source frames. This preserves the saved pose while restoring foot-driven mesh motion; Stretch To length and volume remain approximate |
| Armature vertex-group skinning | `RigExecSkinMover`, used deform-bone palette, normalized weights, live mesh/armature spaces, zero-weight vertices following their owning object |
| Transform constraints mapping to location | Existing native constraint frame maps evaluated location, XYZ Euler rotation or scale through per-axis ranges, clamping or extrapolation, axis remapping and ADD/REPLACE location mixing. Reads constrained source frames, preserves stack order and world-space influence; other destination or rotation policies remain diagnosed |
| Masked/chained armatures | Shared `RigExecLayeredSkinMover` revisions; fractional/inverted masks and original-input caching for contiguous multi-modifier chains |
| Relative mesh shape keys | Existing `RigExecBlendShapeMover` with sparse `UsdSkelBlendShape` offsets before skinning, non-Basis relative keys, group masks and editable signed weights. Saved mute remains static; absolute keys, keys with prepared topology, key animation and key drivers remain diagnosed |
| Bound Surface Deform | Saved polygon/centroid/corner bindings become native `RigExecSurfaceBindingMover` attributes: indices, generalized barycentric weights, vector offsets and masks. Reads the cage's final points in modifier order, with live object-space conversion; no nearest-triangle weight transfer. Unsupported modifiers on the cage or attachment remain diagnosed |
| Corrective Smooth | Existing `RigExecDeltaMushMover` with directed corner frames, simple or current edge-length weighting, iterative masks, border pinning, detail scale and smoothing-only output. Saved BIND coordinates come from SDNA; ORCO requires compatible original topology. Smoothing runs in live owner local space |
| Nearest-surface Shrinkwrap | Existing `RigExecSurfaceMover` with on-surface, inside, outside and outside-surface policies, signed offset and fractional/inverted masks. Source and target spaces follow native frames. Saved target tessellation is explicit; projection, above-surface mode and changing tessellation remain diagnosed |
| Lattice | Existing `RigExecLatticeMover` evaluates current incoming coordinates against the canonical regular grid, with per-axis linear, B-spline, cardinal or Catmull-Rom interpolation, end-index clamping, masks and strength. Native point cages retain authored deformations and follow object/bone parenting and weighted radius-zero Hooks |
| Consumed vertex-weight masks | Texture-free VertexWeightMix masks are evaluated by Blender into the snapshot when a supported downstream modifier consumes them. Weight edits and source drivers remain diagnosed; unrelated weight modifiers retain their diagnostics |
| Preserve Volume | Native dual quaternion skinning, with a Blender-parity diagnostic |
| Meshes | `UsdGeomMesh` control cages with `subdivisionScheme = "catmullClark"` assumed for every mesh, asset-space bind points, extent, render-active face-varying `st` and all named UV sets; editing/render UV selection metadata is retained |
| Constant topology modifiers | Pre-deformation Mirror and nonsmooth vertex-group Mask materialized on undeformed bind data; including hidden dependency cages; no pose or shape-key geometry captured |
| Unskinned meshes | Native matrix mover following the owning object control |
| Bone custom shapes | Mesh-edge or POLY-curve guides live on the editable control; `guide:source` preserves alternate shape-transform bones without creating read-only pick targets |
| Materials | `UsdPreviewSurface` color, opacity, roughness, metallic, emission and tangent normals; packed/file/UDIM images, named UVs and supported static texture arithmetic are converted to portable PNG assets |
| Material slots | Whole-mesh binding or material face subsets |
| Rigify UI collections | Native `RigExecPicker` pages in source UI row order, including hidden collections; supported buttons select native controls |
| CloudRig UI panels and collections | Stored panel order, conditional bindings and nested/hidden collections are preserved as metadata; supported buttons explicitly say `Select` and target native controls |
| Linked libraries and file textures | Flattened loaded scene data, anchored asset dependencies; files are referenced rather than copied |
| Scene units and frame rate | Z-up, meters-per-unit and time-code metadata |

Object channels are captured at the saved frame as editable TRS avars.
Eligible bone controls compose native translation, XYZ rotation and scale over
an exact neutral Blender frame. The original joints retain the Blender bone
and constraint evaluation, so skins and downstream bones read the same bone
providers. The native control displays the pre-constraint edit frame when its
bone has an output constraint. Connected bones, nonlocal translation, and
non-FULL or disabled rotation inheritance use `rigExec:channelSpaces`: two
evaluated providers describe the rotation and translation channel bases.
Their native expression still computes the exact Blender pre-constraint pose
from the original channel values. Both viewers use those published bases for
manipulation. Connected bones disable translation while retaining rotation
and scale, matching their fixed origin in Blender. Extra native
configuration channels such as `avars:rspin`, rotation order, rotation sign
and unit scale do not map to Blender bone behavior; animate the nine exported
TRS channels with XYZ rotation. The converter fixes each control's default
space through a standard connection, so stock Pivot editing is refused while
Pose editing remains available. Translation
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
Relative shape keys run before this modifier stack. Each mesh exposes a float
attribute with `blender:shapeKey` custom data containing the source key name;
editing it updates both signed native sample branches without Blender.
Offsets use each key's own relative key and saved vertex-group mask. Saved
muted keys retain zero offsets; changing their mute metadata does not re-enable
them. Source shape-key animation and drivers remain diagnosed.
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
bindings as inert metadata. A supported binding is labelled `Select <name>` and
selects the associated native controls; it does not change the source setting.
If any member of a button is unsupported, the whole button is dimmed and
labelled `(unavailable)`, with no target, so a group never selects only part
of its source set. Source settings switches and Python operators are diagnosed
and are not executable picker actions.
Embedded UI text is preserved without running it. Separate third-party picker
formats are not currently imported.

Ellie reuses the native biped Body and Face artwork, with all 766 source
transform controls represented on collection pages, including hidden FK and
detail controls. The 115 source behavioral settings are retained on explicit
unavailable scene metadata because their property drivers are not translated;
the stock picker hides panels with no live actions. See
[the Ellie picker contract](docs/ellie-picker.md) for inventory and validation.

Package an exported native layer and its resolved asset dependencies with:

```sh
python tools/package.py character.usdc character.usdz
```

This uses OpenUSD's general USDZ packager and verifies ZIP alignment/CRC,
dependency resolution, byte-identical concrete UDIM tile sets, native prim types and computation connections, every
picker page and control target, and unchanged source bytes. A sibling
`character.package.json` records the result and SHA-256. Native evaluation and
the picker UI require only usdRig; this file-format plugin is not a playback
dependency. Packaging does not add
sampled playback or certify Blender parity.

## Diagnostics and limits

`/Rig.blender:diagnostics` and layer `blenderRig:diagnostics` report unsupported
features and known approximations. `blenderRig:complete` means a full read
produced no diagnostics; it is not a general Blender-equivalence claim.
`blender:sourceData` preserves the extracted snapshot (including constraint
settings, topology, smooth flags and modifiers' diagnostics) as inert JSON.
Each provider also retains its extracted source record. Dependencies are
available through `GetExternalAssetDependencies()` and layer custom data.

Unsupported features include absolute shape keys and keys with prepared topology,
shape-key animation/drivers, arbitrary modifiers, bone envelopes,
multi-armature caches following unsupported modifiers, B-Bone segments,
object vertex parenting, CUSTOM spaces, LOCAL_OWNER_ORIENT owner spaces,
non-WORLD spaces for non-copy/non-Transform constraints, partial rotation masks, rotation
offset/inversion/ADD mix modes,
enabled rotation limits, unsupported constraint types, non-POLY curve geometry, custom split normals, and procedural
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
`evaluation` tests native control → constraint → mixed-weight skin deformation,
reset, and live native T/R/S control-frame agreement across parent edits and a
constrained parent. `blenderFixture` creates a compressed real Blender rig with rolled
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

`tests/testUsdviewBlenderControls.py` checks hand/foot picker clicks, visible
curve picking, exact world translation and real gizmo mouse drags. Build
`usdBlenderRigViewSnapshot`, then run it with the same viewer environment as
the comparison test. Curves hidden in the Blender source are checked through
the picker. Channel-space controls require the updated usdRig viewer/runtime
and regenerated exports on desktop and iOS.

### Surface bindings

The extractor reads saved Surface Deform bindings from an uncompressed temporary
copy written by Blender, using that file's SDNA rather than process-memory
offsets. The original `.blend` is never overwritten. The temporary copy is
removed after extraction. No source UI scripts are executed.

`RigExecSurfaceBindingMover` is a shared native surface mover. Binding attributes
hold sparse affected vertices, polygon indices, barycentric weights, binding
weights, and `vector3f[] rigExec:offsets`. Offset components are tangent,
bitangent and normal distances in binding space. Blender's normal distance
becomes `(0, 0, distance)`; runtime supports all three components.

The native mover also accepts limit-surface position and derivative stencils
(`rigExec:surfaceMode = "limit"`) and area-weighted interpolated vertex normals
(`rigExec:normalMode = "smooth"`). Limit mode requires authored limit stencils;
it does not reinterpret polygon weights as subdivision coordinates. Blender
Surface Deform exports use polygon positions and geometric polygon normals
to preserve Blender's binding behavior. Topology changes require rebinding.

The converter preserves modifier order across armatures, Delta Mush, surface
bindings, supported shrinkwrap and lattices. Lattice cages are native
`UsdGeomPoints` dependencies; their weighted Hooks use ordinary matrix movers
and the existing frame expression. The original lattice grid remains the
reference even when its saved control points were edited.

Absolute or topology-prepared shape keys, radial Hook falloff, lattice data weight groups, unsupported
shrinkwrap policies and source drivers remain diagnosed. Nontriangular
shrinkwrap targets retain a captured tessellation; Blender can change its
diagonals as a cage deforms. Native operator coverage does not establish full
production asset parity when upstream deformation or constraints differ.
Strict conversion still rejects diagnostics.

Exported native fixtures are also evaluated in fresh core-only processes by
`coreOnlyNativeUsdParity` and `coreOnlyDualQuaternionParity`. These tests exclude
the converter from plugin discovery, assert that its library is absent, reject
legacy runtime type names in new exports, and compare edited poses to Blender.
`tests/checkStockControlRuntime.py --require-core-runtime` checks real asset
control frames, root-driven geometry, edit restoration, schema ownership, and
the loaded runtime image without writing the source asset.

Upgrade an existing exported asset without evaluation or reconversion:

```sh
source ../usdRig/bin/_env.sh
"$PY" tools/migrate_runtime.py saved-backup.usdz character.usdz
```

The migration tool belongs to this source translator, not the shared runtime.
It renames the seven legacy type names in every packaged USD layer and variant,
including nested packages, while preserving composition, entry names, and asset
bytes. It writes a distinct output atomically and verifies that the source stays
unchanged. USDC/USD roots are also supported; external legacy layers must be
migrated first or supplied as a complete USDZ. No core legacy aliases are needed.
Packages with ambiguous entry paths, symbolic links, or explicit directory
entries are rejected before replacing the output.
