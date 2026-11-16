# Organizing 3D animation

## Clip browser

The search button beside the animation list opens every clip of the AnimationPlayer in one list.

- Type to filter clips by name. **Enter** opens the first match, and **Down** moves into the list.
- Clips whose names start with the same word, such as `rifle_fire` and `rifle_reload`, sit in a folder. A word every clip starts with, such as Blender's `Armature|`, is skipped, so the folders come from the words after it. Clips in other libraries appear under their library.
- Each clip shows its length, and its icon shows its loop mode. Imported clips and the autoplay clip are marked.
- Double-click a clip, or select it and press **Enter**, to open it.
- Select several clips with **Ctrl** or **Shift** and choose **None**, **Loop** or **Ping-Pong** to set their loop mode together. One **Undo** reverses it. Clips imported from a scene file are read-only here; set their loop mode in the file's Advanced Import Settings.

Folded folders stay folded while the editor runs.

## Skeletons in the track list

With tracks grouped by node (the default), a skeleton's tracks nest under its header, so a fully keyed rig stays readable:

- **Skeleton** header, then any tracks of the skeleton's own properties.
- **Body part** headers. A chain of bones that runs without branching, such as the spine, an arm or a leg, is a part, named after its first and last bone, for example `shoulder.L … hand.L`. A lone leaf bone, such as an eye, joins its parent's part, and three or more short chains under one bone, such as fingers, share one part (`hand.L: 5 chains`). A part with only one keyed bone gets no header of its own.
- **Bone** rows, one per bone. They start folded and show all of that bone's keys on one line. Open one with its arrow (or double-click the name) to see its **Position**, **Rotation** and **Scale** tracks.

Rows are sorted by the skeleton, not by when the tracks were made: parts in hierarchy order, bones in hierarchy order inside each part, and Position, Rotation, Scale inside each bone. A newly added track appears with its bone, not at the bottom of the list.

The keys on a bone row, a body part row or a folded header act on every key below them at that time:

- Click to select them, **Shift** or **Ctrl**+click to add or remove them, drag to move them (snapping as usual; **Right-click** while dragging cancels), and box select across rows.
- They are drawn with the selected key icon when all keys at that time are selected.

Folds are kept with the animation, as node groups' folds are, and **Filter Tracks** still finds bones by name.

## Selecting bones

Bone selection is shared by the timeline, the 3D viewport and the Skeleton3D inspector:

- In the timeline, click a bone row's name to select that bone (the skeleton becomes the edited node). **Shift**/**Ctrl**+click adds or removes it. Clicking a body part's name selects all of its bones.
- In the viewport, with the skeleton's **Edit Mode** on: click a joint to select it, **Shift**+click to add or remove one, and drag over empty space to **box select** joints (**Shift** adds to the selection). Clicking empty space clears the bones but keeps the skeleton selected.
- In the inspector's bone tree, **Ctrl**/**Shift**+click selects several bones.

The last bone picked is the *active* bone: the inspector shows its properties and the single-bone tools use it. Selected bones are drawn in the selected bone color in the viewport (the active joint yellow, the others orange), and their rows are highlighted in the timeline.

What the selection drives:

- **Insert Key** and **Insert Key (All Bones)** key only the selected bones when any are selected.
- **Reset Selected Poses** and **Apply Selected Poses to Rests** act on every selected bone.
- The track editor's **Only show tracks from nodes selected in tree** button shows only the selected bones' tracks.
- Moving several bones moves them together. A bone whose parent (or any ancestor) is also selected follows that parent and is not moved twice. Rotating or scaling several separate bones turns or scales each one in place instead of swinging them around the middle of the selection. One **Undo** restores every bone to its own starting pose.

The selection belongs to the skeleton, so it survives the inspector being rebuilt and comes back when the skeleton is selected again.

## Verification

Build the editor, then run:

```text
python misc/scripts/test_animation_organization.py --engine bin/asosdot.windows.editor.x86_64.exe
```

It checks a humanoid skeleton's track list (8 body part headers, 54 bone rows that start folded, opening a bone, folding the skeleton hides everything below it), then checks the clip browser's grouping, search, opening a clip and setting the loop mode of several clips with one undo. A graphics run captures `organize.png`; use `--keep` to see it, or `--state-only` to skip it.
