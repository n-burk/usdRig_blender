# Native computation references

Runtime implementation belongs to the shared usdRig core:
[`affineFrameKernels.cpp`](../../usdRig/libs/rigExecMath/affineFrameKernels.cpp)
contains the stage-free affine numerics,
[`affineFrameComputations.cpp`](../../usdRig/libs/rigExec/affineFrameComputations.cpp)
provides the shared frame computation adapters, and
[`armatureMover.cpp`](../../usdRig/libs/rigExec/movers/armatureMover.cpp)
implements layered skinning. This repository contains source extraction and
translation, including SDNA binding readers and source property interpretation;
it contains no runtime computation or mover implementation.

Blender's source identified coordinate, decomposition, and rounding contracts.
The Blender source remains under its upstream license and is not vendored into
this converter. These references describe the supported operations, not a claim
that the whole Blender dependency graph has been reproduced.

| Contract | Public primary reference | Native implementation |
| --- | --- | --- |
| Bone inheritance, local/pose conversion and connected origins | [Blender armature.cc](https://github.com/blender/blender/blob/a4e9e4869d6abea38277c1d6a08dde63cfb72d0d/source/blender/blenkernel/intern/armature.cc), `BoneParentTransform` and `BKE_armature_mat_pose_to_bone` | `BoneParentSpaces`, `BoneFrame`, `ConstraintParentSpaces` |
| Copy constraints, local owner orientation, stretch, tracking and world-space influence | [Blender constraint.cc](https://github.com/blender/blender/blob/6bf86856285015ad468923862cacd3ba33164e16/source/blender/blenkernel/intern/constraint.cc), `BKE_constraint_mat_convertspace`, the corresponding evaluators and `BKE_constraints_solve` | `ConstraintValue`, `Track`, `ConstraintFrame` |
| Custom constraint object and bone spaces | [Blender constraint spaces](https://docs.blender.org/manual/en/4.0/animation/constraints/interface/common.html), and `BKE_constraint_custom_object_space_init` in the source above | Live `rigExec:customSpace` provider; world/custom matrix conversion |
| Y-axis shear removal and polar interpolation | [Blender math_matrix_c.cc](https://github.com/blender/blender/blob/c90e8bae0beaea55248dc948e16edf64c8d86c32/source/blender/blenlib/intern/math_matrix_c.cc), `orthogonalize_m4_stable`, `mat3_polar_decompose`, `interp_m4_m4m4` | `Orthogonalize`, `PolarFrame`, `BlendFrame` |
| Polar factors via Newton iteration | Higham, [What Is the Polar Decomposition?](https://nhigham.com/2020/07/28/what-is-the-polar-decomposition/) (2020) | `PolarFrame`; nonsingular matrices, at most 64 inverse-transpose iterations |
| Quaternion influence interpolation | [Blender math_rotation_c.cc](https://github.com/blender/blender/blob/77b14f2dcbe1393b661e23a77199091a1f52df8d/source/blender/blenlib/intern/math_rotation_c.cc), `interp_qt_qtqt` | `BlendFrame`; shortest arc, Blender's near-alignment threshold |
| Weighted Armature constraints and rest-oriented dual-quaternion scale pivots | [Blender constraint.cc](https://github.com/blender/blender/blob/6bf86856285015ad468923862cacd3ba33164e16/source/blender/blenkernel/intern/constraint.cc), `armdef_accumulate_matrix`, `armdef_evaluate`; [Blender math_rotation_c.cc](https://github.com/blender/blender/blob/77b14f2dcbe1393b661e23a77199091a1f52df8d/source/blender/blenlib/intern/math_rotation_c.cc), `mat4_to_dquat`, `add_weighted_dq_dq_pivot` | `ArmatureBlend`, using usdRig dual-quaternion conversion/normalization |
| Linear and dual-quaternion skinning | [usdRig numerical references](../../usdRig/docs/references.md), including Kavan et al. (2008) | Shared `RigExecLayeredSkinMover` calls usdRig skinning kernels |
| Relative mesh shape keys and saved mute | [Blender key.cc](https://github.com/blender/blender/blob/d9b6fe34ddce527d93b97c0bf42ad92cebac4e4e/source/blender/blenkernel/intern/key.cc), `key_evaluate_relative`; [rna_key.cc](https://github.com/blender/blender/blob/d9b6fe34ddce527d93b97c0bf42ad92cebac4e4e/source/blender/makesrna/intern/rna_key.cc), shape-key value and slider setters | Extracted relative masked offsets become existing native BlendShape samples; FloatMath maps signed channel weights |

USD uses row-vector matrices. The shared core implementation keeps symmetric polar
stretch on the left of rotation, the transpose of Blender's mathematical
column-vector convention. Bone local-space conversions retain separate
rotation/scale, location, and post-scale factors. Constraint influence is
applied after returning the solution to world space, including for LOCAL and
POSE owners. Damped tracking retains Blender's float direction/angle boundary
because almost opposite directions can otherwise choose different axes.

The reference source versions above correspond to the unchanged numerical
files read from the local Blender checkout. Runtime parity is independently
checked against Blender 5.2.2 LTS, and exported USD is reopened with Blender
and the converter plugin unavailable. See the README for limitations and the production acceptance
report for asset-specific failures.

Saved surface bindings use the public Blender
[Surface Deform implementation](https://github.com/blender/blender/blob/main/source/blender/modifiers/intern/MOD_surfacedeform.cc)
and [DNA modifier layouts](https://github.com/blender/blender/blob/main/source/blender/makesdna/DNA_modifier_types.h)
as numerical and serialization references. `blender/surface_bind.py` interprets
the serialized file's SDNA and expands corner/centroid bindings into generalized
barycentric coefficients; it does not copy Blender code or inspect live pointers.

Saved Corrective Smooth coordinates use the same SDNA reader and
[Corrective Smooth implementation](https://github.com/blender/blender/blob/main/source/blender/modifiers/intern/MOD_correctivesmooth.cc).
The shared native Delta Mush mover supplies corner frame transport and current
edge-length smoothing; the converter preserves the saved local reference space.
Lattice grids, displacement interpolation and weighted Hooks follow
[lattice_deform.cc](https://github.com/blender/blender/blob/main/source/blender/blenkernel/intern/lattice_deform.cc),
[key.cc](https://github.com/blender/blender/blob/main/source/blender/blenkernel/intern/key.cc)
and [MOD_hook.cc](https://github.com/blender/blender/blob/main/source/blender/modifiers/intern/MOD_hook.cc).
Hook transforms are expressed using the existing live frame expression and
native matrix mover; no source scripts are required at playback.
