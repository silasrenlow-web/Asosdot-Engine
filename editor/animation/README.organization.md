# Organizing 3D animation

## Clip browser

The search button beside the animation list opens every clip of the AnimationPlayer in one list.

- Type to filter clips by name. **Enter** opens the first match, and **Down** moves into the list.
- Clips whose names start with the same word, such as `rifle_fire` and `rifle_reload`, sit in a folder. A word every clip starts with, such as Blender's `Armature|`, is skipped, so the folders come from the words after it. Clips in other libraries appear under their library.
- Each clip shows its length, and its icon shows its loop mode. Imported clips and the autoplay clip are marked.
- Double-click a clip, or select it and press **Enter**, to open it.
- Select several clips with **Ctrl** or **Shift** and choose **None**, **Loop** or **Ping-Pong** to set their loop mode together. One **Undo** reverses it. Clips imported from a scene file are read-only here; set their loop mode in the file's Advanced Import Settings.

Folded folders stay folded while the editor runs.

## Body parts in the track list

With tracks grouped by node (the default), a skeleton's bone tracks are split by body part, and each part has its own fold arrow:

- A chain of bones that runs without branching, such as the spine, an arm or a leg, is a part. It's named after its first and last bone, for example `shoulder.L … hand.L`.
- A lone leaf bone, such as an eye or a jaw, joins its parent's part.
- Three or more short chains under one bone, such as fingers or toes, share one part, for example `hand.L: 5 chains`.

The bone icon marks a body part, and its tooltip names the skeleton. Folds are kept with the animation, as node groups' folds are, and **Filter Tracks** still finds bones by name.

## Verification

Build the editor, then run:

```text
python misc/scripts/test_animation_organization.py --engine bin/asosdot.windows.editor.x86_64.exe
```

It checks the body-part groups of a humanoid skeleton and folds them by clicking, then checks the clip browser's grouping, search, opening a clip and setting the loop mode of several clips with one undo. A graphics run captures `organize.png`; use `--keep` to see it, or `--state-only` to skip it.
