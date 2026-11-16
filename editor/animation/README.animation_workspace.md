# The Animation workspace

The **Animation** button, beside **3D** at the top of the editor, opens a screen laid out for animating cutscenes and rigs, the way a video editor's edit page is:

```text
+------------+---------------------+--------------------+-----------+
| Scene Tree | Viewer: the 3D      | Monitor: the live  | Inspector |
|  the nodes | editor (gizmos,     | shot or any camera |           |
|------------| bone posing)        |--------------------|           |
| Library    |                     | Multicam: 1  2  3  |           |
|  Animations|                     |                    |           |
|  Shots ... |                     |                    |           |
+------------+---------------------+--------------------+           |
| Timeline: tracks, or the Graph Editor (Ctrl+Tab)      |           |
+-------------------------------------------------------+-----------+
```

- **Viewer**: the 3D editor itself, with its gizmos, transform tools and the skeleton editor for posing bones. Selecting nodes keeps you in the workspace.
- **Scene Tree**, left of the viewer: the scene's nodes, to pick what to animate. Described below.
- **Library**, under it: the animations, cameras and cuts, sounds, and poses described below.
- **Monitor**, right of the viewer: the shot as the game shows it, described below. Drag the line between them to share the width differently.
- **Inspector**: the editor's own Inspector, for the selected node, or the selected key's time, value and easing.
- **Timeline**: the Animation panel, opened and made taller while you're here. Everything it does works as usual.

The Scene, FileSystem and other side docks make way while the workspace is open, and come back when you leave it.

## Toolbar

The workspace's buttons sit in the 3D editor's toolbar, in three groups and one menu:

- **Record**: while it's on, edits are keyed at the playhead. Moving, rotating or scaling something in the viewer keys its position, rotation or scale, only what changed. Posing a bone keys that bone. Changing a property in the Inspector keys that property. Tracks are made as needed without asking. Undo takes back the keys first, then the edit.
- **Shot**: the viewer looks through the camera that's live at the playhead. Camera cuts switch it as you scrub or play.
- **Cut**: cuts to the selected camera at the playhead.
- **Graph**: switches the timeline to the Graph Editor and back, and gives the timeline more of the height while it shows.
- **Panels**: what the workspace shows, and what floats. See below.
- The playhead's time, and its frame at the animation's step.

## Panels and floating windows

The **Panels** menu has two halves.

**Show** switches each part on or off: the **Scene Tree**, the **Library**, the **Camera Monitor**, and the editor's **Other Editor Docks** (Scene, FileSystem and the rest, which the workspace hides otherwise). A part that is switched off gives its room to the others; with the Scene Tree and the Library both off, the viewer takes the whole width.

**Float in a Window** moves a part into a window of its own, to put on another screen:

- The **Scene Tree**, the **Library** and the **Camera Monitor** also have a float button at the right of their header. Click it to float the part where it is; right-click it to pick the screen.
- While a part floats, the parts that stay share the room it left.
- Close the window to dock the part back where it was, or choose it in the menu again. **Dock All Back** returns all three at once.
- The **Timeline** and the **Inspector** are editor docks. The menu floats them the way their own dock menu does, and they dock back the same way: close the window.

The workspace's floating windows belong to it. They close when you switch to another screen, such as Script, and reopen where they were when you come back.

What shows, what floats, and where each window is, are saved with the editor layout, so they are the same after a restart. This follows the editor setting **Interface > Multi Window > Restore Windows on Load**; with it off, everything starts docked. Floating needs the editor's multi-window mode (on by default); in single-window mode the float buttons are disabled.

## Scene Tree

The Scene Tree lists the edited scene's nodes, like the Scene dock, without its editing tools:

- **Click** a node to select it. It is the editor's own selection, so the Inspector shows the node, the viewer's gizmo moves to it, and the timeline highlights its tracks. Ctrl and Shift select several.
- A **key** icon marks every node the animation being edited has tracks for. The line under the tree counts them.
- **Filter Nodes** narrows the list by name. As in the Scene dock, `type:Camera3D` filters by type and `group:enemies` by group.
- The two buttons beside the filter **open** every branch, or **close** every branch under the scene's root, to keep a big scene readable.
- Selecting a 2D node or a Control here does not switch to the 2D screen while the workspace is open.

## Graph Editor

**Graph** in the toolbar, the curve button at the bottom of the timeline, or **Ctrl+Tab** with the timeline focused switches the timeline between its tracks (a dope sheet) and the Graph Editor, as Blender does. The Graph Editor draws every animated number of the animation as a curve over the same ruler, so the playhead, zoom and scroll stay as they were:

- **Position**, **rotation** and **scale** tracks, as X, Y and Z curves. Rotations are shown and edited as Euler angles in degrees, in the node's rotation order, and kept as quaternions.
- **Blend shape** tracks, and **value** tracks of numbers, vectors and colours, a curve per number.
- **Bezier** tracks, with their handles.

Curves are coloured as in Blender (X red, Y green, Z blue) and listed by node on the left. A curve's eye hides it and its lock keeps it from being edited. Click a curve's name to select it, double-click to select its keys, and click a node to select all its curves.

What you see is what plays: each curve is drawn from Godot's own interpolation of the track, with its easing.

### Keys

| Key | Does |
| --- | --- |
| Click, Shift+click | Select a key, add to the selection |
| Drag on empty space, B | Box select (Shift adds, Ctrl takes away) |
| A, Alt+A, Ctrl+I | Select all, none, invert |
| K | Select the keys at the same times on every curve |
| [ and ] | Select the keys before or after the playhead |
| G, or drag a key | Move; X keeps it to time, Y to value; click or Enter confirms, right-click or Esc cancels |
| S | Scale about the selection's middle, with X and Y as for G |
| Shift+D | Duplicate and move |
| X, Delete | Delete |
| Ctrl+C, Ctrl+V | Copy, and paste at the playhead |
| I | Key every selected curve (or every curve shown) at the playhead |
| Ctrl+right-click | Key the curve under the mouse where you click |
| T | Interpolation: Constant, Linear, Bezier |
| Ctrl+E | Easing: Linear, Ease In, Ease Out, Ease In and Out, Ease Out and In |
| V | Handle type: Auto Clamped, Vector, Aligned, Free, Mirrored |
| Shift+S | Snap keys to the playhead or to frames, or the playhead to the keys |
| Up, Down | Jump the playhead to the next or previous key |
| Home, Numpad . | Frame all, frame the selection |
| Ctrl+H, H, Shift+H, Alt+H | Show all handles; hide selected, hide others, reveal curves |
| N | The sidebar: the active key's time, value, interpolation, easing and handles |
| Wheel, Ctrl+wheel, Shift+wheel, middle-drag | Zoom time, zoom values, scroll values, pan |

Right-click opens the Key menu. Every key is an editor shortcut under Graph Editor, so it can be changed.

**Normalize** draws each curve from -1 to 1 over its own range, to compare curves of different sizes. **Only Show Selected** shows only the curves of the nodes selected in the scene.

### How Godot keys differ

A Godot key holds all of a track's numbers at once, so a position key is the X, Y and Z of one moment. Moving a key in time moves it on all three curves. Deleting it deletes it from all three. Moving it up or down changes only the curve you moved it on.

Position, rotation, scale and value tracks interpolate the same way all along, so **T** sets the whole track's interpolation, and **Ctrl+E** eases each key into the next. Bezier tracks have handles on every key instead. **Channel > Convert to Bezier Curves** splits the selected tracks into a Bezier track per number, keyed where they were, for Blender-style control of every key's handles. A rotation becomes `rotation:x`, `rotation:y` and `rotation:z` in radians, as `Node3D.rotation` is. Bone rotations and quaternion values stay as they are, as separate curves can't keep a quaternion whole. Undo puts a converted track back exactly.

## Camera monitor

The monitor shows a camera's picture as the game would: no gizmos, grid or selection, with the project's antialiasing and 3D scaling, in the game window's shape. The menu at its top picks what it shows:

- **Live Shot**: the camera that's live at the playhead. It switches as the cuts do, while you scrub or play.
- **Any camera** in the scene, to frame it while another one is live.

Its border is a tally light: red while it shows the live shot, green while it previews another camera. The burn-in over the picture shows the camera's name, its lens as a full-frame focal length and field of view, the timecode, the frame and the aspect ratio. Double-click the picture to select the camera and change its lens or settings in the Inspector.

The eye menu has:

- **Rule of Thirds**, a **Center Mark** and **Safe Areas** (action safe 93%, title safe 90%).
- **Burn-in**, the text over the picture.
- **Letterbox** bars for 1.85:1 or 2.39:1; the guides keep to the frame inside them.
- **Aspect Ratio**: the game window's (from the project's viewport size), or 16:9, 4:3, 21:9, 9:16 or 1:1, to check a shot on other screens.
- **Half Resolution**, for big scenes on slower machines.

### Multicam

Under the picture, the multicam strip shows every camera in the scene, numbered in scene order and marked in its shot colour. The live camera's tally is red, and the camera the monitor previews is green.

- **Click** a camera to cut to it at the playhead. While the animation plays, clicking cuts live, on the frame being shown, so you can switch shots in time with the action as on a vision mixer.
- **1** to **9** cut to the first nine cameras the same way. They're editor shortcuts (Animation Workspace > Cut to Camera), so they can be changed.
- **Right-click** a camera to show it in the monitor or select it.
- The grid button at the top of the monitor hides the strip.

The strip only renders again when something changes, a couple of cameras a frame while playing, so a quiet editor stays idle. Its pictures are drawn without what costs each view the most: SDFGI, screen-space effects (SSAO, SSIL, SSR), volumetric fog, Asos Lighting, and omni and spot light shadows. Their colour and framing are right, but the lighting is simpler than in the monitor, which draws everything as the game does. A scene's every view renders its own shadow passes, one for each face of each shadowed omni light, so a level with many lights would cost the strip a great deal otherwise.

Cameras with the same name, such as several "Camera3D"s, are shown with their parent's name too. A camera inside a SubViewport with its own world shows that world.

## Library

**Animations** lists the edited AnimationPlayer's animations by library, with their lengths; looping ones have the loop icon. Click one to edit it; double-click to play it from the start. **New**, **Duplicate**, **Rename** and **Delete** do what the Animation panel's menu does.

**Shots** lists the scene's cameras, each in the colour its shots have on the timeline, and the current animation's cuts.

- To cut to a camera, put the playhead where its shot starts and press the camera's key button (or select the camera and press **Cut**).
- Click a cut to move the playhead to it; its minus button removes it.
- Double-click a camera to select it.

**Sounds** lists the project's audio files. Double-click one to listen; double-click again to stop. Drag one onto an audio player's header or lanes in the timeline, like a file from the FileSystem dock. Its **+** button adds it at the playhead. The line under the list says which player it goes to. See "Many sounds from one player" below.

**Poses** keeps rig poses in a pose library. A pose library is an AnimationLibrary file you choose with **Open...** or make with **New...**; the workspace remembers it for the project.

- **Save Pose** stores the selected Skeleton3D's pose under a name.
- **Apply** poses the selected skeleton; double-clicking a pose does the same.
- **Apply to Bone** poses only the bone selected in the skeleton editor.
- **Mix** goes part of the way, from the current pose (0%) to the saved one (100%).
- With **Record** on, applying a pose keys the bones it moved.

Poses are matched by bone name, so a pose fits any rig with the same bones.

## Many sounds from one player

One audio player plays any number of sounds at once, so an animation needs one `AudioStreamPlayer` for its non-positional sounds and one `AudioStreamPlayer3D` (or `2D`) for each place sounds come from, not a node per sound.

In the timeline, an audio player is one group. Its audio tracks are its **lanes**, named **Lane 1**, **Lane 2** and so on:

- Sounds on different lanes overlap and play together through that one node, with its bus, volume and, for a 3D player, its position.
- **Fold** the group with its arrow to get one row: every lane's clips drawn as thin bars, one row of bars per lane.
- The **+** at the end of the group's name opens a file picker. The sounds you choose are added at the playhead.
- **Drag** sounds from the Sounds list or the FileSystem dock onto the group's header or onto a lane. Dropped on the timeline they start where you drop them; dropped on the name, at the playhead. Several files can be dropped at once.
- A sound goes on the first lane with room at that time. If every lane is taken there, a new lane is made. Dropping onto a lane tries that lane first. So sounds never cut each other short.
- One **Undo** takes back everything one drop or one pick added, new lanes included.

Each clip is still an ordinary audio key. Select it to set its **Start Offset**, **End Offset**, **Volume dB** and **Pitch Scale** in the Inspector, or drag its edges in the lane to trim it.

The **+** in the Sounds list uses the selected audio player. With none selected it uses the first player the animation already has sounds on, and with none of those either it adds an `AudioStreamPlayer` named `Sounds` beside the AnimationPlayer's other nodes. For 3D sound, select your `AudioStreamPlayer3D` first (the Scene Tree is right above the list).

Lanes are ordinary audio tracks that share a node path, so the animation plays the same in a running game. Asosdot plays audio keys as layered sounds (`play_layered`), so they don't replace the player's own `stream`. A player layers up to 128 sounds, and an AnimationPlayer stops the oldest past its `audio_max_polyphony` (32 by default).

## Less in the way

The bar under the tracks keeps what is used all the time: the Graph Editor switch, the selected-nodes filter, insert at current time, snapping, the step, zoom and fit. The button at its end opens the rest: the Bezier curve editor and the handle mode of new Bezier keys, method names, sorting, grouping, and snapping to whole FPS.

## Camera cuts in Godot

A cut is an ordinary key on each camera's `current` property: `true` where its shot starts and `false` where the next camera's starts. Every camera with cuts also gets a `false` key at the start, so no shot shows before it begins. The timeline draws these tracks as shot bars, like clips in a video editor, in each camera's colour.

Each camera has the same colour everywhere: on the timeline, in the Shots list and in the multicam strip. Colours go round the colour wheel by the camera's place in the scene, so cameras next to each other never look alike, however alike their names are.

Because they are ordinary keys, the cuts play the same in a game, and in Godot 4.7. Asosdot also plays them correctly when seeking or scrubbing: the latest key at or before the playhead decides which camera is live, where Godot would use the nearest key.

## Saved with the editor layout

The workspace remembers its library tab, Shot, the pose mix, which parts show, which float and where their windows are in the editor layout, and the monitor remembers its camera, guides, letterbox, aspect ratio, resolution, strip and width. Godot 4.7 opens a project left in the workspace in the 3D screen.
