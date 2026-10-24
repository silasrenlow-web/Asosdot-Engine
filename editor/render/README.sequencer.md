# Render camera sequencer

Open a 3D scene and select **Render**. Existing Render settings become the first shot; their resolution, frame rate and supersampling become the sequence's output settings.

The camera preview occupies the upper left, with **Shot** and **Output / Music** properties on the right. The bottom dock holds the shot list, the timeline and the playback controls. Drag the separators to resize these panels; their sizes are remembered with each scene. **Reset Layout** restores the default sizes.

**Focus Workspace** hides the editor's surrounding docks while Render is active and restores their previous state when you leave Render. **Maximize Preview** temporarily hides the Render properties and sequence panels. The preview's **Guides** button adds a rule-of-thirds grid and safe margins without including them in exported images.

**Pose** opens a column of tool icons beside the preview: Rotate, Move and Reach, the parent and reset buttons, and the key buttons. Each icon names its action in its tooltip. The bone or part you selected, and the clip its keys go to, appear in the status line under the workspace. The picture keeps its height while posing.

## Shots

- **Add** creates a shot with default settings and the current studio camera view. **Duplicate** copies the selected shot's camera, lighting, animation and dialogue. **Up**, **Down** and **Remove** change the shot order. These operations support editor Undo/Redo. A sequence always has at least one shot.
- Each shot in the list shows its number, name and duration; its camera is in the tooltip and on the Shots lane. Choose a shot to edit its name, duration, camera, studio lighting and animation. Orbit, pan, zoom and Frame Subject are remembered separately for each studio camera. Scene cameras can also be animated by the selected AnimationPlayer.
- **Camera Blend** is the incoming shot's transition time. Zero means a hard cut. A positive value blends from the previous shot's final camera to this shot's camera with smooth easing during the beginning of the shot. It does not shorten or overlap shots. Transforms, FOV and orthographic size interpolate; differing projection types or aspect modes cut. Lighting and character animation switch at the shot boundary.
- **Animation Start** chooses where to begin in the source animation, in seconds. Animation Speed applies after that offset. Posing and keying use the corresponding source time.
- Select the **Output / Music** tab above the inspector to set one resolution, frame rate and supersampling level for every shot. Music has a sequence start time, a source offset and a volume. Each shot can have its own dialogue file, source offset and volume. WAV, Ogg Vorbis and MP3 files in the project are supported. Music stops at the file or sequence end; dialogue stops at the file or shot end. Green and amber bars indicate music and dialogue.

## Timeline

The timeline has one row of lanes, each named at its left:

- **Shots** shows each shot's name and camera; striped areas are camera blends.
- **Keys** shows the selected shot's animation keys as diamonds. Keys of the selected bone or part are bright. A click on a key lands exactly on that key's time.
- **Dialogue** and **Music** show the audio, with a playhead across all lanes.

Click or drag anywhere on the timeline to scrub. **Play** loops the complete sequence. The time readout is `hours:minutes:seconds:frames`, using the sequence's output frame rate. **Previous Key** and **Next Key** sit beside the readout.

## Keyboard

These work when no text or number field has keyboard focus:

| Key | Action |
| --- | --- |
| Space | Play or pause the sequence |
| Left, Right | Step one frame back or forward |
| Shift+Left, Shift+Right | Step one second back or forward |
| Ctrl+Left, Ctrl+Right | Previous or next key of the selected shot |
| Home, End | Go to the start or last frame of the sequence |
| `[`, `]` | Select the previous or next shot |

While posing, R, G and E choose Rotate, Move and Reach, I keys the pose, Shift+I keys the whole skeleton, Alt+I deletes keys here, and Up selects the parent bone. Escape cancels a pose drag.

## Playback and output

- **Render Image** captures the current preview. **Render Sequence** writes the complete sequence as OGV, AVI or numbered PNG frames. Video includes the music/dialogue mix; PNG output contains images. Audio is mixed by sample time, so rendering speed does not affect playback speed. Surround outputs receive the stereo mix in their front channels. The live scene's audio buses and audio tracks inside AnimationPlayer clips are not part of this soundtrack.
- The output size and frame rate appear at the right of the toolbar, beside the render actions.

Shot settings and views are remembered with each scene's editor state. Previewing, rendering, cancelling and switching shots restore any scene properties changed by the preview animation. Camera cuts get fresh render warmup frames before capture to allow temporal effects to settle.

## Verification

Build the editor, then run:

```text
python misc/scripts/test_render_sequence.py --engine bin/asosdot.windows.editor.x86_64.exe --temp-dir C:/dev
```

The integration check uses an isolated editor and project. It tests old state migration, cuts and direct scrubbing into blends, animation source offsets, shot ordering/undo, scene restoration, serialization, frame stepping, the keyboard transport and key navigation, and layout restoration. It then launches a graphics editor to check panel bounds at full size and 1280×800, capture workspace previews, and render a short sequence. It checks PNG/video frames, soundtrack timing and cancellation. Use `--state-only` for the headless checks, or `--keep` to retain screenshots and output for inspection.
