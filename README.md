# usdBlenderRig

A read-only OpenUSD `SdfFileFormat` plugin that opens Blender `.blend` files as
native usdRig rigs. It follows the standalone sidecar pattern in
[`../usdRig_maya`](../usdRig_maya/README.md) and links to
[`../usdRig`](../usdRig/README.md)'s installed `rigExecRigging`, `rigExec` and
`rigExecScene` libraries. CMake consumes the installed dependency SDK package.

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

Build usdRig `main` against OpenUSD 26.08 with OpenExec first, following its
README, then install its package:

```sh
cmake --build ../usdRig/build
cmake --install ../usdRig/build --prefix ../usdRig/build/install
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DUSDBLENDERRIG_BLENDER=/path/to/Blender.app/Contents/MacOS/Blender
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix build/install
```

`cmake --build ../usdRig/build` also compiles usdRig's own tests. Where one of
them does not compile with your toolchain (at usdRig a64e332 with Apple clang
21, `testRigExecRuntimeInputs`), build with `cmake --build ../usdRig/build -- -k 0`
so Ninja finishes every library, then install as above; the SDK does not
depend on that executable.

CMake automatically finds the sibling's `build/install` package. For a
different install, pass `-DrigExec_DIR=/path/to/lib/cmake/rigExec`; a build
tree configured that way keeps the choice until `cmake -U rigExec_DIR -S . -B build`.
Configuration stops with an error when the package has no `rigExec::rigExecScene`
target, that is when it predates usdRig `main`.
Rebuild and reinstall usdRig, then rebuild this plugin, whenever usdRig changes:
the install is a copy, and the plugin must load the SDK it was compiled against.
usdRig lowers USD rigs in `rigExecScene`, which registers itself when loaded, so
the plugin and the evaluating tests link it and compile `sceneLink.cpp` to keep
that dependency under linkers that drop unreferenced libraries.
The Python tests that open a stage (`pickerPackaging`, `rigifyPicker`,
`pickerTemplate`, `migrateRuntime`) run with the interpreter the OpenUSD package
records, or `-DUSDBLENDERRIG_USD_PYTHON=/path/to/python`; they are not
registered when no interpreter imports `pxr`. `launcherFileFormatOnly` and
`pickerTemplate` need the sibling `../usdRig` checkout and are registered only
when it is there.
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
in `build/rigexec-prefix`; set `USDBLENDERRIG_RIGEXEC_PREFIX` to test another SDK
and `USDBLENDERRIG_BUILD` when the plugin's build tree is not `build`. It takes
Python, OpenUSD and usdview from the sibling checkout's `../usdRig/bin/_env.sh`.
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

# sourceData=1 embeds the extracted snapshot JSON; sourceData=0 is the default.
layer = Sdf.Layer.FindOrOpen("character.blend", {"sourceData": "1"})

# pruneUnobserved=1 omits armatures that nothing observes. Off by default: the
# result is no longer a 1:1 translation (see "Diagnostics and limits").
layer = Sdf.Layer.FindOrOpen("character.blend", {"pruneUnobserved": "1"})

# probeWholeRig=1 runs the lower-limb calibration probe on the whole rig instead
# of its dependency closure: the same layer plus a diagnostic, and much slower.
layer = Sdf.Layer.FindOrOpen("character.blend", {"probeWholeRig": "1"})
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
extent and Blender's source pixel width. The original pixel width stays in
the snapshot (`sourceData=1`). Legacy snapshots with zero wire width use the same extent rule.
Generated IK endpoint joints have their guides hidden, so their schema-default
radius cannot cover the character. Texture file attributes carry the authored
color space as metadata in addition to the UsdUVTexture input.
Generated rigs author no evaluation-mode opt-in on the root. usdRig compiles
every rig into one operation graph, and its schema has no `rigExec:baked`
attribute; authoring an undeclared property raises on open in Python hosts.
Earlier exports that carry the attribute still load and compile; it is ignored.

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
| Armature bones, head frames and roll | Nested `RigExecJoint` providers; local bind matrices preserve bone orientation; `blender:deform` records the deform flag |
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
| Armature vertex-group skinning | `RigExecSkinMover`, used deform-bone palette, normalized weights, live mesh/armature spaces, zero-weight vertices following their owning object. Skin layers and deformers of one mesh share an influence provider per bone (`influence_<mesh>_<bone>`) and one owner-follow provider (`follow_<mesh>`); the follow palette slot exists only when a vertex has no weight |
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
| Rigify UI collections | Native `RigExecPicker` with the biped Body/Face panels filled by anatomical role from Rigify's generated control names; every other control of a UI collection, including hidden ones, has one button on an overflow panel named after its collection. The sidebar rows are read where Blender stores them, as system properties from Blender 5 and as custom properties before, with or without the add-on |
| CloudRig UI panels and collections | The same biped panels by role from CloudRig's component prefixes; remaining controls of stored panels and nested/hidden collections each have one overflow button; a collection binding keeps its whole set as a group button; settings and operator bindings are diagnosed and kept as picker metadata, not buttons |
| Generated rigs without stored UI pages | Collections whose bones mostly carry custom shapes become the source pages; numeric source settings are listed as unavailable picker metadata |
| Linked libraries and file textures | Flattened loaded scene data, anchored asset dependencies; files are referenced rather than copied |
| Scene units and frame rate | Z-up, meters-per-unit and time-code metadata |

Object channels are captured at the saved frame as editable TRS avars.
Eligible bone controls compose native translation, XYZ rotation and scale in
their bone's own native space: a `RigExecSpaceSwitch`
(`/Rig/Mechanisms/controlSpace_<name>`) re-parents the control to the bone's
`/Rig/Dag` parent, and the control's `default:space` holds a literal matrix:
the bone's raw rest over the parent's default space. RigExec builds a default
space from rests that it
orthonormalizes level by level, so the joint's own `default:space` would lose
the last digits of a rest saved in single precision; the literal cancels the
parent default exactly and leaves `avars * rest * parent posed`, the neutral
Blender frame. The control frame therefore equals its bone's pre-constraint
frame to floating-point rounding at any scale, not just within an absolute
tolerance: the difference is a few 1e-15 of the frame's largest element, on
the measured rigs at most 3.1e-15. That is rounding of the parent frame's
magnitude, so it is the same absolute amount on every axis: an axis of a
control scaled down to 0.02 carries it as about 1e-13 of its own length, and a
frame a million units from the origin as about 1e-10 units. A viewer that composes frames from
authored USD without the evaluator reads the same literal, so an un-activated
gizmo sits on the bone's rest frame with the control's own channels applied
and does not follow posed parents; the evaluated frame is the authority. A
bone whose rest carries scale, shear or
reflection instead keeps a neutral Blender frame provider under
`/Rig/ControlSpaces`. The original joints retain the Blender bone
and constraint evaluation, so skins and downstream bones read the same bone
providers. The native control displays the pre-constraint edit frame when its
bone has an output constraint. Connected bones, nonlocal translation, and
non-FULL or disabled rotation inheritance use `rigExec:channelSpaces`: two
evaluated providers describe the rotation and translation channel bases,
`/Rig/ControlSpaces/<name>` and `/Rig/TranslationSpaces/<name>`. They are
siblings, not parent and child, so the translation basis is not revisited
when the rotation basis changes; resolve them through the relationship, in
that order, not by path.
Their native expression still computes the exact Blender pre-constraint pose
from the original channel values. Both viewers use those published bases for
manipulation. Connected bones disable translation while retaining rotation
and scale, matching their fixed origin in Blender. Extra native
configuration channels such as `avars:rspin`, rotation order, rotation sign
and unit scale do not map to Blender bone behavior; animate the nine exported
TRS channels with XYZ rotation. The converter authors each control's default
space, so stock Pivot editing is refused while Pose editing remains available:
a native control holds the literal above, or, where that literal is exactly
identity and would read like no opinion, a connection to the shared identity
source `/Rig/Mechanisms/identityParent.outputs:matrix`; every other control
connects to its neutral provider, and a `rigExec:channelSpaces` control has no
pivot by contract.

`/Rig/Controls`, `/Rig/ControlSpaces`, `/Rig/TranslationSpaces` and
`/Rig/Mechanisms` only collect providers. Each is authored as one hidden
`RigExecControl` instead of a `Scope` whenever a provider lives beneath it,
because on every evaluation, however little changed, usdRig samples the
namespace between each provider and its nearest ancestor provider, or the
asset root when it has none. A channel basis or skin provider directly beneath
its holder has nothing left to sample, and a Control one prim instead of the
path to the root. These holders
are not controls of the source rig: they have no `blender:id`, zero guide
opacity and untouched channels, and `/Rig/Controls` carries the `guide`
purpose of the Controls beneath it. Every provider beneath them is an
absolute or switched frame, so editing a holder's channels changes no mesh and
no joint, and moves Control frames only by floating-point rounding of the
holder's own magnitude: below 1e-14 for an ordinary transform, about 1e-10
units for a holder translated a million units, because the holder's channels
enter each Control frame and cancel again. A holder's
rest is not free in the same way, because skin providers bind through it, so
each holder connects its `default:space` to the shared identity source and
stock Pivot editing is refused on it as on a Control.
`/Rig/Dag` stays a `Scope`, since the scene's root objects would inherit from
such a provider. Translation
does not export animation curves; all animation/driver/NLA data is diagnosed.
Prim names use compact Blender labels truncated to 32 characters. Numeric
starts receive an `n_` prefix; duplicate or sanitized labels receive
deterministic identity suffixes when they collide;
full source identifiers remain in `blender:id` and `blender:sourceId`. Relationships,
connections and every picker target use the resulting paths. UV property names
retain their reversible encoding, preserving distinct named UV sets.
Mesh bind points include saved object placement; native expressions account
for live object/armature placement during deformation. Bind topology is fixed;
editing Mirror/Mask parameters or their driver inputs is not supported.
Relative shape keys run before this modifier stack. Each mesh exposes a float
attribute with `blender:shapeKey` custom data containing the source key name;
editing it updates both signed native sample branches without Blender. The
negative branch blends by a float-mover inversion of that attribute and reads
it with `rigExecReadPhase = "final"`; the default phase would see the authored
value instead of the mover's result.
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

Every translated picker is mapped onto the native biped `Body` and `Face`
panels (prims `Body` and `Facial`), followed by overflow panels. The translator
does this from the snapshot alone, so existing `.blendrig` files get the layout
without re-extraction, and nothing is keyed to a rig or character name.
[docs/picker.md](docs/picker.md) is the full description; in short:

- **Roles.** `resources/biped_picker.usda` carries the artwork. Each selecting
  slot names the native biped role(s) it stands for (`customData.blender.roles`,
  for example `L_UpArm`) and nothing else: no source bone names. A role
  resolves to one control of the picker's armature through the `PickerRoles`
  table in `translator.cpp`. The side is parsed from `.L`/`_R`/`Left ` style
  tokens and the rest of the name is compared, ignoring case, with the names
  the rig generators emit for their own metarig bones:

  | Generator | Convention | Examples |
  | --- | --- | --- |
  | Rigify (rig types and metarigs bundled with Blender) | generated control names | `root`, `torso`, `hips`, `chest`, `spine_fk.001`, `tweak_spine.001`, `neck`, `head`, `shoulder`, `upper_arm_fk`, `hand_ik`, `upper_arm_ik_target`, `upper_arm_parent`, `upper_arm_tweak`, `thigh_fk`, `foot_ik`, `foot_heel_ik`, `toe`, `f_index.01`, `thumb.01`, `palm.01_fk`, `eye_common`, `jaw_master`, `lid.T.002`, `lip.T.001` |
  | CloudRig (component prefix on a bundled-metarig bone) | `FK-`, `FK-OS-`, `IK-`, `POLE-`, `ROOT-`, `STR-`, `FK-W-`, `ROLL-M-`, `TORSO-`/`HIP-`/`CHST-`, `CTR-`, `TGT-`, `ACT-`, and their pre-4.1 forms `IK-MSTR-`, `IK-POLE-`, `MSTR-<spine>_Torso`, `MSTR-TGT-`; the spine is `Spine`/`RibCage`/`Chest`, `Spine1`..`Spine3` or `Hips`/`Spine`/`Chest` and the hand `Wrist` or `Hand`, by metarig version | `FK-UpperArm`, `IK-Wrist`, `IK-Hand`, `POLE-Thigh`, `STR-Forearm_1`, `TORSO-Spine`, `TORSO-Hips`, `FK-Spine2`, `FK-Finger_Index1`, `CTR-Eye`, `TGT-Eyes`, `ACT-Eyelid_Upper` |

  Rigify names rank before CloudRig names; among matches a visible control
  beats a hidden one, then a shaped one an unshaped one. A bone that is neither
  visible nor shaped takes no slot. A control fills one role and a role takes
  one control. A name no generator emits, such as a character's own face,
  prop, hair or clothing control, takes no slot and is reached through
  overflow.
- **Unresolved slots** are not authored (no dimmed placeholders), mirror pairs
  whose peer is gone are dropped, a template group slot needs at least two
  resolved members, and a panel without a live slot is omitted. Native IK/FK
  switches, space dials and commands are not part of the template.
- **Clickability.** The native layout stacks each IK slot on its FK slot and
  lets the mode dial hide one of them; the stock picker gives a click to the
  topmost bounding box, so the template separates those pairs: FK and IK hand,
  and FK and IK foot, are side-by-side halves of the native rectangle, and the
  knee pole vector draws above the FK thigh. With every slot live, each keeps
  at least a quarter of its rectangle clickable.
- **Overflow.** The set of selectable controls is exactly what the source pages
  offer. Every control without a single-target biped slot gets exactly one
  button on an overflow panel (`page_N`, three columns) for the first source
  page that lists it alone, otherwise the first page that lists it in a group.
  Panels are cut by count once the placed controls are gone, at most 48
  buttons, as `Name 1/3`; extraction emits whole pages. The label is the page
  name, the collection name where a Rigify title is a bare suffix such as
  `(Tweak)`, with ` (more)` appended where it would repeat a biped panel label.
- **Group buttons.** A source button that selects several controls (for
  example a CloudRig `Bone Collections` entry) is kept as one group button
  with the source label and the whole source set, first on the overflow panel
  of its page. If any member is read-only in standard usdRig interaction the
  button selects nothing: a group never selects part of its source set.
- **Unavailable entries.** Read-only selections, settings switches and Python
  operators are diagnosed and recorded on the picker prim as
  `blender:unavailable` (`page: label`) with a parallel
  `blender:unsupportedReason`; they are not buttons. `blender:controlCount`,
  `blender:bipedCount`, `blender:overflowCount` and `blender:groupCount` give
  the totals. Source JSON, embedded UI text and control inventories are not
  copied onto picker prims, and button attributes equal to the schema fallback
  are not authored. Separate third-party picker formats are not imported.

A generated rig that stores no sidebar pages gets source pages from its
control collections. A collection counts as one when more than half of its
bones carry a custom shape; no collection is recognised by name.

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
The translated layer does not embed the extracted snapshot. Providers keep
`blender:id`/`blender:sourceId`, and each bone joint a `blender:deform` flag;
schema attributes are authored only where they differ from the usdRig fallback.
An armature bone's avars are connected to its Control and also keep the saved
pose as their own value. The file-format argument `sourceData=1`
adds the whole snapshot (including constraint settings, topology, smooth flags
and modifiers' diagnostics) as inert JSON in `/Rig` custom data
`blender:sourceData`, which multiplies the layer size. Dependencies are
available through `GetExternalAssetDependencies()` and layer custom data.

Every armature is translated by default, including hidden construction rigs
such as a Rigify metarig. The file-format argument `pruneUnobserved=1` is an
explicit opt-out of that 1:1 contract: an armature is omitted with all of its
bones, their Controls, mechanisms and constraints when none of its bones is
visible and none is named anywhere else in the snapshot, that is as a picker
target, skin influence, constraint source/pole/target, guide source, hook or
parent of a node that is kept. A reference from another armature counts only
when that armature is itself kept. The armature object stays, every surviving
prim keeps its path, name and values, and evaluated meshes and remaining bones
are unchanged. Each omitted armature is reported once as
`unobserved armature omitted (pruneUnobserved): <id> (<n> bones)`, so such a
layer is never `complete` and `strict=1` rejects it. Its bones cannot be posed
or compared against Blender.

The lower-limb calibration probe compiles a temporary copy of only the prims
the probed leg joints can depend on (their namespace ancestors, every
connection source and relationship target, and every prim that writes one of
them). It falls back to the whole rig when that copy cannot be built or does
not pose a probed joint; the authored result is the same either way, but the
whole-rig probe compiles the entire rig during translation (seconds to tens of
seconds on a production character), so it is always reported as
`connected lower-limb calibration probed the whole rig: <reason>`.
The file-format argument `probeWholeRig=1` requests that path to verify the
closure against it: apart from this diagnostic the two layers must be
identical, which the `fileFormat` test asserts on `tests/fixtures/leg.blendrig`,
a Rigify-convention pair of FK, IK, ORG, tweak and DEF leg chains written by
`tests/makeLegFixture.py`. The probe follows Rigify's `limbs.leg` and
`limbs.paw` naming: the `ORG-`, `DEF-` and `MCH-` bones of a side's `thigh`,
`shin`, `foot` and `toe` (`r_toe`), read through `ORG-shin` against that
side's `foot_ik`. Its nudge and thresholds are relative to the shin's length,
not to scene units.

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
`build/blender-proof/`. Every generator is checked in: `tests/makeFixture.py`
for the Blender fixture, `tests/makeRigifyFixture.py` for a rig the bundled
Rigify generates, `tests/makeLegFixture.py` for `tests/fixtures/leg.blendrig`
and `tools/make_picker_template.py` for `resources/biped_picker.usda`; CTest's
`legFixture` and `pickerTemplate` fail when a checked-in file no longer
matches its generator. A
passing generated fixture is evidence for these cases, not for arbitrary
Rigify or production rigs. Production acceptance requires passing the same
independent Blender comparison on the asset itself.

For a production asset, with NumPy installed in the host USD Python:

```sh
python tools/validate.py character.blend build/character-proof --blender /path/to/blender
```

This reads the source without saving it, captures the saved pose, available
root/hand IK/arm FK/foot/head/jaw/torso control edits under their Rigify or
CloudRig names, and reset, exports `native.usdc`,
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

CTest's `pickerPackaging` runs the picker/package regression on the Blender
fixture (`tests/testPickerPackaging.py build/blender-proof build/picker-proof`
by hand): extraction of Rigify rows stored both ways, CloudRig panels with a
collection group, the collection fallback on real Blender objects, the biped
mapping, and USDZ packaging with two concrete UDIM tiles and a rejected empty
tile pattern. `rigifyFixture` generates the bundled Rigify human rig and
extracts it with a Blender that has not enabled the add-on; `rigifyPicker`
then requires one page per UI collection in sidebar row order and ties the
role table to the generated geometry: a slot drawn further out or lower on
the biped panel selects a control that is further out or lower on the rig.
`fileFormat` covers the Rigify and CloudRig role tables, overflow, group
buttons, the unavailable rule, clickability and mirror links on synthetic
bones; `pickerInventory` covers control classification. Two further checks
take any converted rig and have no rig-specific expectations built in:

```sh
python tests/checkPickerCoverage.py character.usdc
USDBLENDERRIG_USDVIEW=/path/to/testusdview \
bin/usdview.sh --testScript tests/testUsdviewPickerCoverage.py character.usdc
```

The first checks the picker contract on the stage; with
`--inventory capture.json` it also requires the selectable set to equal the
classified controls of an independent Blender capture of an armature's pose
bones, which `tests/capturePickerInventory.py` writes. The second drives stock
usdview with real mouse events. What a caller knows about its rig is passed
in: `USDBLENDERRIG_PICKER_CONTROLS` (a JSON list of required source bone
names), `USDBLENDERRIG_PICKER_SETTINGS` (the number of unavailable source
settings) and `USDBLENDERRIG_PICKER_MIRROR_CLICKS` (the number of Alt-click
mirror selections; without it a rig with no mirrored slot passes).

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
Qt gizmo mouse events for root/body, left hand IK and left foot IK, under the
name Rigify or CloudRig generates for each, with two
warm-up gestures followed by 24 measured moves per control.
`USDBLENDERRIG_VIEW_CONTROLS` adds workloads as `label=name` pairs,
`USDBLENDERRIG_VIEW_RIGID_MESHES` lists mesh prim names that must follow a
root drag rigidly (within `USDBLENDERRIG_VIEW_RIGID_TOLERANCE`, default 2e-5),
and `USDBLENDERRIG_VIEW_MOVED_MESHES` lists `control=mesh` pairs whose mesh
that control must move; nothing about a particular asset is built in. The report separates
native preview/publish time, event handling and input through viewport redraw;
the redraw measurement includes queued UI work and can include multiple paints.
It excludes physical display scanout. Biped's hand IK switch is enabled for its
hand workload and restored afterward. Omit `USDBLENDERRIG_VIEW_REFERENCE` for a
native baseline such as `../usdRig/examples/biped/Biped_stack.usda`.

For physical iPad comparison, `tools/compare_ipad.py` uses the same desktop
workloads and independent Blender references. It requires the configured
OpenUSD Python environment and NumPy:

```sh
"$PY" tools/compare_ipad.py prepare build/character-proof/Character.usdz \
  build/usdview-comparison/Character.json build/ipad-comparison-fixtures \
  --reference build/character-proof/reference.json
```

Copy the manifest and `Blender-Character.usdz` to the app's Documents directory.
The iOS Debug launch argument `-ipadRigComparison Character.json` records the
terminal Hydra scene index's float32 mesh points, published asset-space
frames, screenshots and control timings in `Documents/RigComparisonOutput/Character`.
The physical device must be unlocked. The driver refuses simulator capture.
`AppUITests/testPhysicalRigComparisonCapture` accepts
`RIGEXEC_IPAD_COMPARISON_MANIFEST=Character.json`; run the three Blender picker
drag tests separately for ordinary XCUITest touch acceptance.

After copying the output directory back, compare it without evaluating a rig:

```sh
"$PY" tools/compare_ipad.py compare \
  build/ipad-comparison-fixtures/Character-metadata.json \
  build/ipad-comparison/Character build/ipad-comparison/Character.json
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

Native numerical conventions and algorithm sources are documented in
[references](docs/references.md).

`tests/testUsdviewBlenderControls.py` checks hand/foot picker clicks, visible
curve picking, exact world translation and real gizmo mouse drags, on the left
hand and foot IK control under its Rigify or CloudRig name or on the controls
named in `USDBLENDERRIG_VIEW_CONTROLS`. Build
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
