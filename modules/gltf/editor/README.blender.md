# Blender round trips in Asosdot

Set **Editor Settings > Filesystem > Import > Blender > Blender Path** to the Blender executable (Blender 3.0 or later).

Use **Scene > Export As > Blender Scene...** to export the open scene to a `.blend` file. To export one character or object, select its parent branch and use **Selected Branch to Blender...**. Include the character's skeleton and AnimationPlayer inside that branch. Meshes referencing a skeleton outside the branch require selecting a higher parent or exporting the whole scene.

The export captures the current, unsaved scene's visible geometry, transforms, standard PBR materials, image textures, UVs, skin bindings, blend shapes, cameras and punctual lights. Images are embedded in the intermediate GLB and packed into the final `.blend`, so the file does not depend on the project's texture paths. Animation export and its sampling FPS are configurable. The Blender conversion runs in the background; the editor reports completion or errors and can open the resulting file in Blender.

To bring Blender edits back into Asosdot:

- Save the `.blend` inside the project. With Blender import enabled, the editor converts and reimports it when the file changes.
- Alternatively, use Blender's **File > Export > glTF 2.0**, choose **glTF Binary (.glb)**, and save it inside the project. GLB includes the image textures in one portable file.
- Build materials around **Principled BSDF** with UV-mapped image textures for base color, metallic, roughness, normals, and emission. Bake procedural textures and unsupported node graphs into images first. The `.blend` Import dock must have **Materials > Export Materials > Export** enabled (the default).
- Use an inherited scene or a wrapper scene for gameplay scripts and engine-specific additions so source reimports preserve your game setup.

The `.blend` importer enables **Meshes > Preserve Shape Keys** by default. When the Blender scene contains shape keys, this skips modifier application during conversion, since applying modifiers can remove the shape keys in some Blender versions. Disable it to prioritize evaluated modifier geometry, or bake compatible modifiers in Blender before exporting. Armature skinning still transfers with shape key preservation enabled.

The exchange uses glTF 2.0. Custom shaders, lens flares, the Render workspace's temporary studio, world environments, post-processing, physics and gameplay behavior do not have equivalent Blender data in this transfer. Renderer-specific lighting and color management can change the final appearance. Exported rigs transfer as deforming bones and skin weights; Asosdot's IK nodes don't go back to Blender. Imported `.blend` rigs bring their IK and other constraints with them, as described below. Only glTF-supported animation tracks transfer. Hidden objects are excluded. Branch exports retain their local transforms.

## Rigs and constraints from Blender

When a `.blend` is imported, Asosdot rebuilds the armature's constraints as skeleton modifiers, so the rig moves here the way it moves in Blender: animate the controls and the bones and props follow, in the game as in Blender. **Rigs > Rebuild Constraints** in the `.blend` Import dock turns this off for a file.

| Blender constraint | Asosdot node |
| --- | --- |
| Copy Location, Copy Rotation, Copy Scale, Copy Transforms | BlenderConstraint3D |
| Limit Location, Limit Rotation, Limit Scale | BlenderConstraint3D |
| Child Of, Damped Track, Track To, Locked Track, Stretch To | BlenderConstraint3D |
| Transformation, Maintain Volume | BlenderConstraint3D |
| IK with a chain of 2, with a pole target | TwoBoneIK3D, which turns both bones toward the pole as Blender does |
| IK with a longer chain | FABRIK3D (close, but Blender's solver differs) |

BlenderConstraint3D evaluates a constraint with Blender's own math, with every option the constraint has in Blender: World, Pose, Local With Parent and Local spaces, Head/Tail along a target bone, axes, inversion, offsets, every mix mode, Euler orders, power, uniform scale, limits, bulge and volume, the Child Of inverse matrix, and influence. Its settings keep their Blender names in the Inspector, such as `use_offset` and `mix_mode`. The nodes sit under the Skeleton3D in the order Blender evaluates the constraints, so a constraint that reads another constrained bone runs after it, and a bone's stack keeps its order. The nodes are named after the constraint and its owner, for example `Copy Location on hand_L` and `IK on forearm_L`.

Objects that follow the armature come along too: a prop or an empty with a Copy Location, Child Of or Track constraint aimed at a bone gets its own BlenderConstraint3D, whose **Owner Node** is that object.

To act once and live, a rebuilt constraint is muted while Blender bakes the animations, so the clips carry the poses before it and Asosdot applies it as the game runs. A constraint stays baked into the clips instead, and its node comes in switched off, when Asosdot can't reproduce it exactly: when its influence or settings are animated or driven, when it is on a bone in an IK chain, uses a custom or B-Bone space, targets another armature or an object that isn't exported, or when muting it would change something else that stays baked. The Output panel names each of these with the reason. Constraints with no Asosdot equivalent, such as Armature, Action and Shrinkwrap, stay baked and are listed there too.

The constraint zoo check matches Blender to well under a tenth of a millimetre and a thousandth of an axis for every type above, on Blender 4.2 and 5.2. Blender can shear a bone, for example with Child Of an unevenly scaled target; bones here, as in glTF, can't, so such a bone keeps Blender's Y axis and scale as Blender does when it removes shear.

Each control bone an IK constraint uses, such as its target or pole, gets a ModifierBoneTarget3D under the Skeleton3D, so posing or animating that bone moves the rig. Two-bone IK with a pole matches Blender to within about a millimetre and a quarter of a degree for joints and twist alike, which is the precision of Blender's own iterative solver. Other IK is rebuilt with differences, and the Output panel names them: IK without a pole (a pole is placed at the chain's bend) and IK with Rotation.

Constraints travel with `.blend` imports, where Blender reads them; a `.glb` exported by hand carries the baked animation only. Keep bone names as Blender has them: retargeting the skeleton with a BoneMap renames bones, and the rebuilt nodes refer to them by name. The import's animation optimizer drops keys within a small angle; for a clip that must play exactly as baked, turn off **Optimizer** for the AnimationPlayer in Advanced Import Settings.

Run the real application checks with:

```text
python misc/scripts/test_blender_constraints.py --engine bin/asosdot.windows.editor.x86_64.exe --blender "C:/Program Files/Blender Foundation/Blender 5.2/blender.exe"
python misc/scripts/test_blender_ik_rig.py --engine bin/asosdot.windows.editor.x86_64.exe --blender "C:/Program Files/Blender Foundation/Blender 5.2/blender.exe"
python misc/scripts/test_blender_roundtrip.py --engine bin/asosdot.windows.editor.x86_64.exe --blender "C:/Program Files/Blender Foundation/Blender 5.2/blender.exe"
```

The constraint check builds a Blender armature with one case for each constraint type and option above, a stack, a constraint that depends on another, an animated influence and two objects that follow bones. It compares the imported clip, played in Asosdot, with Blender's evaluated poses, then moves a target to check the constraints act live.

This uses an isolated editor and temporary project. It tests both export menu actions, packed albedo and normal images, a skin with two bones, current blend shape weights, a 12-second animation, animation omission, visibility, cameras/lights, Blender-to-engine GLB and direct `.blend` imports, paths containing spaces/apostrophes/Unicode, and preservation of an existing destination when Blender fails.
