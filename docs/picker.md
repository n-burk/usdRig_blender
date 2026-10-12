# Native pickers

Every picker the translator authors has the same shape, whatever rig it came from: the usdRig biped `Body` and `Face` panels (prims `Body` and `Facial`) filled by anatomical role, then overflow panels (`page_N`) for everything else. Nothing in extraction or translation is keyed to a rig, armature or character name. What decides the layout is the snapshot (which controls the source pages offer, whether they are visible, shaped and editable) and the naming conventions of the two rig generators, Rigify and CloudRig.

## Source pages

`blender/extract.py` turns what the rig stores into plain pages of buttons; it decides no layout.

| Source | Pages |
| --- | --- |
| Rigify (`rig_id`, collections with `rigify_ui_row`) | One page per UI collection in sidebar row order, named by its `rigify_ui_title` or else the collection, one button per bone, hidden collections included |
| CloudRig (`is_generated_cloudrig`, `ui_data`) | One page per stored sidebar panel, one button per bound property or collection, then one page per bone collection including nested and hidden ones |
| Generated rig that stores neither | One page per control collection (`blender/picker.py`), plus one page of numeric bone properties as unavailable settings |

Rigify and CloudRig register these fields with `bpy.props`. Blender 5 stores such values as system properties, apart from custom properties and whether or not the add-on is enabled; earlier files hold them as custom properties. Extraction runs with factory settings, without the add-ons, and reads both stores, so a rig generated in Blender 5 yields the same pages as an older file.

For the last case a bone is a control when it has a custom shape, or when it sits unshaped in a control collection, and in either case has an unlocked channel or a numeric property. A control collection is measured, not named: more than half of its bones carry a custom shape. Bones that fail are listed with a reason in the snapshot inventory, not in the stage.

A source button that names a bone collection selects every bone of that collection, nested children included. Such a button is a group; see below.

## Roles

`resources/biped_picker.usda` carries the artwork. Each selecting slot lists the native biped role(s) it stands for in `customData.blender.roles`, for example `L_UpArm`; group slots list several. The template holds role keys only, never a source bone name. `tools/make_picker_template.py` regenerates the file from the Body and Face panels of usdRig's `examples/biped` pickers: each native control selection becomes its role, labels, commands, attribute slots, sliders and mode-only artwork are dropped, and the FK/IK pairs are separated as described under Clickability.

A role resolves to one source control of the picker's armature through `PickerRoles` in `translator.cpp`:

1. The side is parsed from the name: a `.L`/`.R`, `_L`/`_R` token at the end or before a further `.`/`_` part, or a leading `Left `/`Right `. Everything else is the centre.
2. The rest of the name is compared, ignoring case, with the names listed for the role, Rigify names first, then CloudRig names, in the order written.
3. Among the matches a visible control beats a hidden one, then a shaped control beats an unshaped one, then table order decides. A bone that is neither visible nor shaped is a mechanism the source merely lists and takes no slot.
4. A control fills one role and a role takes one control.

A slot whose role does not resolve is not authored; there are no dimmed placeholders. A group slot needs at least two resolved members, a mirror link survives only between two authored buttons, and a panel without a live slot is omitted. Native IK/FK switches, space dials and commands are not part of the template.

### Where the names come from

A name is in the table because a generator emits it for the bones of its own metarigs, not because a particular rig uses it.

**Rigify**, from the add-on bundled with Blender (`scripts/addons_core/rigify`), rig types applied to the bone names of the bundled metarigs (`metarigs/human.py`, `Basic/`, `Animals/`):

| Rig type | Controls |
| --- | --- |
| `generate.py` | `root` |
| `spines.basic_spine` | `torso`, `hips`, `chest`, `<bone>_fk` (`spine_fk.001`), `tweak_<bone>` (`tweak_spine.001`), `torso_pivot` with the custom pivot option |
| `spines.super_head` | `neck`, `head` |
| `basic.super_copy` | the bone itself (`shoulder.L`, `teeth.T`) |
| `limbs.arm`, `limbs.leg`, `limbs.paw` | `<bone>_fk`, `<bone>_ik`, `<bone>_ik_target`, `<bone>_parent`, `<bone>_tweak`, `foot_heel_ik`; the toe is `toe` (`toe_fk` with an IK toe), `r_toe` on the cat and horse metarigs |
| `limbs.super_finger`, `limbs.super_palm` | `f_index.01`, `thumb.01`, `palm.01_fk` |
| `faces.super_face` | `eyes`, `eye.L`, `jaw_master`, `nose_master`, `ear.L`, `lips.L`, and the face bones as tweaks |
| `face.skin_eye`, `face.skin_jaw`, skin chains (the upgraded face) | `eye_common`, `eye_master.L`, `jaw_master_mouth`, one control per chain bone and a `<chain>_end` control at its tip (`lip_end.L.001`) |

Chain indices follow the bundled metarig: `brow.T.L.002`, `lid.T.L.002`, `lip.T.L.001`. A metarig with longer lip chains continues the numbering, so `lip.T.L.002`, `.003` and `lip_end.L.002`, `.003` are listed as well. A chain's first control has no index and sits where the chain starts: `lid.T` runs from the outer eye corner over the eye and `lid.B` from the inner corner under it (`metarigs/human.py`: `eye.L` at x 0.0516, `lid.B.L` 0.0262, `lid.T.L` 0.0768), so `lid.B` is the inner corner and `lid.T` the outer one. `brow.T.003` is the innermost brow control and `brow.T.001` the outermost. CTest's `rigifyPicker` generates the bundled human rig and checks every such chain against the positions of its controls.

**CloudRig**, from its repository (`CloudRig/rig_components`, release tags for Blender 2.83 to 4.5 and the main branch), a component prefix on a bone name of its bundled metarigs (`META-Cloud_Human`, `META-Sintel` and the component samples: `Spine`, `RibCage`, `Chest`, `Neck`, `Head`, `Shoulder`, `UpperArm`, `Forearm`, `Wrist`, `Thigh`, `Knee`, `Foot`, `Toes`, `Finger_Index1`, `Finger_Index_Carpal`, `Eye`, `Jaw`). The human metarig renamed two chains between versions, and all forms are listed: its spine is `Spine`, `RibCage`, `Chest` in the component sample, `Spine1`, `Spine2`, `Spine3` in the 4.5 metarig and `Hips`, `Spine`, `Chest` on main; its hand is `Wrist`, and `Hand` on main. The spine roles resolve from the lowest bone up, so `FK-Spine` is the middle bone only where the chain starts at `FK-Hips`:

| Component | Controls from 4.1 | Before 4.1 |
| --- | --- | --- |
| `cloud_spine` | `TORSO-<spine>`, `HIP-<spine>`, `CHST-<spine>` | `MSTR-<spine>_Torso`, `MSTR-<spine>_Hips`, `MSTR-<spine>_Chest` |
| `cloud_fk_chain` | `FK-<bone>`, `FK-OS-<bone>` (offset control, 4.1 to 4.5), `ROOT-<bone>` | `FK-<bone>`, `ROOT-<bone>` |
| `cloud_ik_chain`, `cloud_limb` | `IK-<bone>`, `POLE-<bone>`, `FK-W-<bone>` | `IK-MSTR-<bone>`, `IK-POLE-<bone>` (`IK-POLE-Arm`, `IK-POLE-Leg` up to 2.93), `FK-W-<bone>` |
| `cloud_leg` | `ROLL-M-<foot>` | `ROLL-<foot>` |
| `cloud_chain` | `STR-<bone>`, `STR-<bone>_<n>` per segment | `STR-<bone>`, `STR-<bone><n>` |
| `cloud_aim` | `CTR-<bone>`, `TGT-<bone>`, `TGT-<group>` (group `Eyes` by default) | `CTR-<bone>`, `TGT-<bone>`, `MSTR-TGT-<group>` |

The `ACT-Eyelid_Upper`, `ACT-Eyelid_Lower`, `ACT-Eyebrow_Main` and `ACT-MouthCorner` bones of the bundled face metarig are listed too.

### The table

| Role | Rigify | CloudRig |
| --- | --- | --- |
| `Main` | `root` | `root` |
| `M_Body` | `torso` | `TORSO-Spine`, `TORSO-Spine1`, `TORSO-Hips`, `MSTR-Spine_Torso` |
| `M_BodyGimbal` | `torso_pivot` | – |
| `M_HipSwivel` | `hips` | `HIP-Spine`, `HIP-Spine1`, `HIP-Hips`, `MSTR-Spine_Hips` |
| `M_Chest` | `chest` | `CHST-Spine`, `CHST-Spine1`, `CHST-Hips`, `MSTR-Spine_Chest` |
| `M_Torso` | `spine_fk.001`, `spine_fk` | `FK-Hips`, `FK-OS-Spine`, `FK-Spine`, `FK-OS-Spine1`, `FK-Spine1` |
| `M_SpineMid` | `spine_fk.002` | `FK-OS-RibCage`, `FK-RibCage`, `FK-OS-Spine2`, `FK-Spine2`, `FK-Spine` |
| `M_ChestTop` | `spine_fk.003` | `FK-OS-Chest`, `FK-Chest`, `FK-OS-Spine3`, `FK-Spine3` |
| `M_SpineBend1` | `tweak_spine.001` | `STR-Hips`, `STR-Spine`, `STR-Spine1` |
| `M_SpineBend2` | `tweak_spine.002` | `STR-RibCage`, `STR-Spine2`, `STR-Spine` |
| `M_SpineBend3` | `tweak_spine.003` | `STR-Chest`, `STR-Spine3` |
| `M_Neck` | `neck` | `FK-Neck` |
| `M_NeckShift` | – | `STR-Neck` |
| `M_Head` | `head` | `FK-Head` |
| `M_HeadGimbal` | – | `STR-Head` |
| `L_/R_Shldr` | `shoulder` | `FK-Shoulder` |
| `L_/R_UpArm` | `upper_arm_fk` | `FK-UpperArm` |
| `L_/R_LoArm` | `forearm_fk` | `FK-Forearm` |
| `L_/R_Hand` | `hand_fk` | `FK-Wrist`, `FK-Hand` |
| `L_/R_ArmIK` | `hand_ik` | `IK-Wrist`, `IK-Hand`, `IK-MSTR-Wrist` |
| `L_/R_ArmPV` | `upper_arm_ik_target` | `POLE-UpperArm`, `IK-POLE-UpperArm`, `IK-POLE-Arm` |
| `L_/R_Arm` | `upper_arm_parent` | – |
| `L_/R_UpArmSwing` | `upper_arm_ik` | `ROOT-UpperArm` |
| `L_/R_ArmBend0` | `upper_arm_tweak` | `STR-UpperArm_1`, `STR-UpperArm1` |
| `L_/R_ArmBend1` | `upper_arm_tweak.001` | `STR-UpperArm_2`, `STR-UpperArm2` |
| `L_/R_ArmBend2` | `forearm_tweak` | `STR-Forearm_1`, `STR-Forearm1` |
| `L_/R_ArmBend3` | `forearm_tweak.001` | `STR-Forearm_2`, `STR-Forearm2` |
| `L_/R_ArmBend4` | `hand_tweak` | `STR-Wrist`, `STR-Hand` |
| `L_/R_UpLeg` | `thigh_fk` | `FK-Thigh` |
| `L_/R_LoLeg` | `shin_fk` | `FK-Knee` |
| `L_/R_Foot` | `foot_fk` | `FK-W-Foot`, `FK-Foot` |
| `L_/R_Toe` | `toe_fk`, `toe`, `r_toe` | `FK-Toes` |
| `L_/R_LegIK` | `foot_ik` | `IK-Foot`, `IK-MSTR-Foot` |
| `L_/R_LegPV` | `thigh_ik_target` | `POLE-Thigh`, `IK-POLE-Thigh`, `IK-POLE-Leg` |
| `L_/R_Leg` | `thigh_parent` | – |
| `L_/R_UpLegSwing` | `thigh_ik` | `ROOT-Thigh` |
| `L_/R_Heel` | `foot_heel_ik` | – |
| `L_/R_BallRoll` | – | `ROLL-M-Foot`, `ROLL-Foot` |
| `L_/R_LegBend0` | `thigh_tweak` | `STR-Thigh_1`, `STR-Thigh1` |
| `L_/R_LegBend1` | `thigh_tweak.001` | `STR-Thigh_2`, `STR-Thigh2` |
| `L_/R_LegBend2` | `shin_tweak` | `STR-Knee_1`, `STR-Knee1` |
| `L_/R_LegBend3` | `shin_tweak.001` | `STR-Knee_2`, `STR-Knee2` |
| `L_/R_LegBend4` | `foot_tweak` | `STR-Foot` |
| `L_/R_ThumbBase` | `thumb.01` | `FK-Finger_Thumb1` |
| `L_/R_ThumbMid` | `thumb.02` | `FK-Finger_Thumb2` |
| `L_/R_ThumbTip` | `thumb.03` | `FK-Finger_Thumb3` |
| `L_/R_IndexBase` | `f_index.01` | `FK-Finger_Index1` |
| `L_/R_IndexMid` | `f_index.02` | `FK-Finger_Index2` |
| `L_/R_IndexTip` | `f_index.03` | `FK-Finger_Index3` |
| `L_/R_MiddleBase` | `f_middle.01` | `FK-Finger_Middle1` |
| `L_/R_MiddleMid` | `f_middle.02` | `FK-Finger_Middle2` |
| `L_/R_MiddleTip` | `f_middle.03` | `FK-Finger_Middle3` |
| `L_/R_RingBase` | `f_ring.01` | `FK-Finger_Ring1` |
| `L_/R_RingMid` | `f_ring.02` | `FK-Finger_Ring2` |
| `L_/R_RingTip` | `f_ring.03` | `FK-Finger_Ring3` |
| `L_/R_PinkyBase` | `f_pinky.01` | `FK-Finger_Pinky1` |
| `L_/R_PinkyMid` | `f_pinky.02` | `FK-Finger_Pinky2` |
| `L_/R_PinkyTip` | `f_pinky.03` | `FK-Finger_Pinky3` |
| `L_/R_IndexMeta` | `palm.01_fk` | `FK-Finger_Index_Carpal` |
| `L_/R_MiddleMeta` | `palm.02_fk` | `FK-Finger_Middle_Carpal` |
| `L_/R_RingMeta` | `palm.03_fk` | `FK-Finger_Ring_Carpal` |
| `L_/R_PinkyMeta` | `palm.04_fk` | `FK-Finger_Pinky_Carpal` |
| `M_Look` | `eye_common`, `eyes` | `TGT-Eyes`, `MSTR-TGT-Eyes` |
| `L_/R_Eye` | `eye`, `eye_master` | `CTR-Eye`, `TGT-Eye` |
| `M_Jaw` | `jaw_master`, `jaw` | `Jaw` |
| `M_Chin` | `chin` | – |
| `M_Mouth` | `jaw_master_mouth` | – |
| `teeth_upper_def` | `teeth.T` | – |
| `teeth_lower_def` | `teeth.B` | – |
| `M_Nose` | `nose_master`, `nose` | – |
| `M_NoseBridge` | `nose.001` | – |
| `M_Septum` | `nose.004` | – |
| `L_/R_Brow` | `brow.T.002` | `ACT-Eyebrow_Main` |
| `L_/R_InBrow` | `brow.T.003` | – |
| `L_/R_MidBrow` | `brow.T.001` | – |
| `L_/R_UpLid` | `lid.T.002` | `ACT-Eyelid_Upper` |
| `L_/R_LoLid` | `lid.B.002` | `ACT-Eyelid_Lower` |
| `L_/R_InLid` | `lid.B` | – |
| `L_/R_OutLid` | `lid.T` | – |
| `M_UpLipMid` | `lip.T` | – |
| `M_LoLipMid` | `lip.B` | – |
| `L_/R_UpLip1` | `lip.T.001` | – |
| `L_/R_LoLip1` | `lip.B.001` | – |
| `L_/R_UpLip2` | `lip.T.002` | – |
| `L_/R_LoLip2` | `lip.B.002` | – |
| `L_/R_UpLip3` | `lip.T.003` | – |
| `L_/R_LoLip3` | `lip.B.003` | – |
| `L_/R_OutLip` | `lips`, `lip_end.001`, `lip_end.002`, `lip_end.003` | – |
| `L_/R_Mouth` | – | `ACT-MouthCorner` |
| `L_/R_Cheek` | `cheek.B.001` | – |
| `L_/R_CheekInner` | `cheek.T.001` | – |
| `L_/R_Ear` | `ear` | – |

Sided roles are written once and resolve as `L_…` and `R_…`. The template has more native roles than the table (toes, eye sockets, individual lid and lip points of the native face); those slots never resolve and are never authored.

## Overflow panels

The set of selectable controls is exactly what the source pages offer. Every control without a single-target biped slot gets exactly one button on an overflow panel, labelled with the source bone name. It goes to the first source page that lists it alone, otherwise the first page that lists it in a group; a settings binding decides the page only for a control no selection button lists. No control is on two overflow panels, and none with a biped slot is repeated on overflow.

Panels are cut by count, after the controls with a biped slot are gone: three columns, at most 48 buttons, split as `Name 1/3` when a page needs more. Extraction therefore emits whole pages. The panel label is the page name, the collection name where a Rigify title is a bare suffix such as `(Tweak)`, and gets ` (more)` appended where it would repeat a biped panel label.

## Group buttons

A source button that selects several controls is kept as one group button with the source label and the whole source set, on the overflow panel of its page, ahead of that page's single buttons and in a different colour. Its members are each selectable on their own as well, through their biped slot or overflow entry.

A group is all or nothing. If any member is read-only in standard usdRig interaction, the button selects nothing: no group button is authored and the members are not made selectable through it.

The biped template has group slots of its own (a whole arm, all finger tips). They are native artwork, resolved by role, and need at least two resolved members.

## Unavailable entries

Read-only selections, settings switches and Python operators are diagnosed and recorded on the picker prim as `blender:unavailable` (`page: label`) with a parallel `blender:unsupportedReason`. They are not buttons, and listing them does not make the setting functional. The picker prim also carries `blender:sourceKind`, `blender:controlCount`, `blender:bipedCount`, `blender:overflowCount` and `blender:groupCount`. Source JSON, UI script text and control inventories are not copied onto picker prims.

## Clickability

The stock picker hit-tests bounding boxes and gives a click to the topmost button: widget controls first, then the larger `ui:depth`, then the later prim. The native layout stacks each IK slot on its FK slot behind a mode dial. Without the dial the template separates them: FK and IK hand, and FK and IK foot, are side-by-side halves of the native rectangle, and the knee pole vector draws above the FK thigh. With every slot live, each keeps at least a quarter of its rectangle.

## Limits

- Only names a generator emits for its bundled metarig bones take a slot. A rig built on a renamed metarig, and every hand-named control (most CloudRig face rigs, props, hair, clothing), is reached through overflow.
- Which of several listed names fills a role also depends on visibility: a visible control beats a hidden one before table order decides, so a rig saved with a control collection hidden can give that collection's slot to a visible alias. Extract with the collections shown as the animator uses them.
- Names are compared without case, so a hand-named bone that reuses a generator name (`Jaw`, `Root`) takes that slot.
- A quadruped maps by the same names: forelegs generated as `upper_arm`/`forearm`/`hand` fill the arm slots.
- Slot position along a numbered face chain is the bundled metarig's; a metarig with a different bone count shifts it.
- Snapshots written by older extractions (pages already cut to 48, `layout` and `inventory` fields) translate through the same path. Their pages are not merged again, so a page can end up nearly empty once its controls have a biped slot; extract again for whole pages.
- Source settings, switches and operators are not translated to native attributes.

## Checks

| Check | Covers |
| --- | --- |
| `ctest -R fileFormat` | Rigify and CloudRig role mapping on synthetic bones, names no generator emits, hidden unshaped bones, overflow de-duplication, cutting by count, group buttons, the unavailable rule, clickability of the template and of translated panels, mirror links, older snapshot fields |
| `ctest -R pickerInventory` | Control classification and collection pages without Blender, under renamed collections |
| `ctest -R pickerPackaging` (`tests/testPickerPackaging.py build/blender-proof build/picker-proof`) | Extraction of the generated fixture (Rigify rows stored as custom and as system properties, CloudRig panels with a collection group, the control-collection fallback on real Blender objects), its translation, and USDZ packaging |
| `ctest -R rigify` | The bundled Rigify human rig, generated and then extracted by a Blender without the add-on: one page per UI collection in row order, and every role chain (arm, leg, spine tweaks, brows, lids, lips, cheeks, nose, teeth) consistent with where the generator puts the controls |
| `ctest -R pickerTemplate` | `resources/biped_picker.usda` is what `tools/make_picker_template.py` regenerates from the usdRig biped example pickers |
| `tests/checkPickerCoverage.py stage.usdc [--inventory capture.json]` | The picker contract on any converted rig; with an inventory captured in Blender by `tests/capturePickerInventory.py`, that the selectable set equals its classified controls |
| `bin/usdview.sh --testScript tests/testUsdviewPickerCoverage.py stage.usdc` | Real mouse events in stock usdview on any converted rig: every live button reachable, a click per panel, both halves of each separated FK/IK pair, Alt-click mirror selection |

The last two take any stage; expectations a caller knows (settings count, mirror clicks, required bone names) are passed in, not built in.
