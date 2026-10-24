# The Animation workspace

The **Animation** button, beside **3D** at the top of the editor, opens a screen laid out for animating cutscenes and rigs, the way a video editor's edit page is:

```text
+--------------+--------------------------------+-------------+
| Library      |  Viewer: the 3D editor         | Inspector   |
|  Animations  |  (gizmos, bone posing, Shot)   | (nodes and  |
|  Shots       |                                |  keys)      |
|  Sounds      |                                |             |
|  Poses       |                                |             |
+--------------+--------------------------------+             |
| Timeline: the Animation panel, made tall      |             |
+-----------------------------------------------+-------------+
```

- **Viewer**: the 3D editor itself, with its gizmos, transform tools and the skeleton editor for posing bones. Selecting nodes keeps you in the workspace.
- **Library**, in the viewer's left panel: the animations, cameras and cuts, sounds, and poses described below.
- **Inspector**: the editor's own Inspector, for the selected node, or the selected key's time, value and easing.
- **Timeline**: the Animation panel, opened and made taller while you're here. Everything it does works as usual.

The Scene, FileSystem and other side docks make way while the workspace is open, and come back when you leave it. **Docks** in the toolbar brings them back in the workspace too.

## Toolbar

The workspace's buttons sit in the 3D editor's toolbar:

- **Record**: while it's on, edits are keyed at the playhead. Moving, rotating or scaling something in the viewer keys its position, rotation or scale, only what changed. Posing a bone keys that bone. Changing a property in the Inspector keys that property. Tracks are made as needed without asking. Undo takes back the keys first, then the edit.
- **Shot**: the viewer looks through the camera that's live at the playhead. Camera cuts switch it as you scrub or play.
- **Cut**: cuts to the selected camera at the playhead.
- **Docks**: shows the other side docks.
- The playhead's time, and its frame at the animation's step.

## Library

**Animations** lists the edited AnimationPlayer's animations by library, with their lengths; looping ones have the loop icon. Click one to edit it; double-click to play it from the start. **New**, **Duplicate**, **Rename** and **Delete** do what the Animation panel's menu does.

**Shots** lists the scene's cameras, each in the colour its shots have on the timeline, and the current animation's cuts.

- To cut to a camera, put the playhead where its shot starts and press the camera's key button (or select the camera and press **Cut**).
- Click a cut to move the playhead to it; its minus button removes it.
- Double-click a camera to select it.

**Sounds** lists the project's audio files. Double-click one to listen; double-click again to stop. Drag one onto an audio track like a file from the FileSystem dock. Its **+** button adds it at the playhead, on the selected AudioStreamPlayer's audio track, making the track if there isn't one.

**Poses** keeps rig poses in a pose library. A pose library is an AnimationLibrary file you choose with **Open...** or make with **New...**; the workspace remembers it for the project.

- **Save Pose** stores the selected Skeleton3D's pose under a name.
- **Apply** poses the selected skeleton; double-clicking a pose does the same.
- **Apply to Bone** poses only the bone selected in the skeleton editor.
- **Mix** goes part of the way, from the current pose (0%) to the saved one (100%).
- With **Record** on, applying a pose keys the bones it moved.

Poses are matched by bone name, so a pose fits any rig with the same bones.

## Camera cuts in Godot

A cut is an ordinary key on each camera's `current` property: `true` where its shot starts and `false` where the next camera's starts. Every camera with cuts also gets a `false` key at the start, so no shot shows before it begins. The timeline draws these tracks as shot bars, like clips in a video editor, in each camera's colour.

Because they are ordinary keys, the cuts play the same in a game, and in Godot 4.7. Asosdot also plays them correctly when seeking or scrubbing: the latest key at or before the playhead decides which camera is live, where Godot would use the nearest key.

## Saved with the editor layout

The workspace remembers its library tab, Shot, Docks and the pose mix in the editor layout. Godot 4.7 opens a project left in the workspace in the 3D screen.
