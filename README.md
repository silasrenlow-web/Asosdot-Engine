# Asosdot Engine

<p align="center">
  <img src="misc/asosdot_banner.png" width="500" alt="Asosdot Engine">
</p>

**Created by Aosjes or formerly Multi-Games Studio. Base model of Godot.**

Asosdot Engine is a fork of [Godot Engine](https://godotengine.org) 4.7, built
to make the games Aosjes actually ships rather than to be a general purpose
engine. Everything Godot does, Asosdot does; the difference is what has been
added on top.

## What this fork adds

**Lens flare.** A `LensFlare3D` node, a `LensFlare` resource and a screen space
pass that draws into the HDR colour buffer before glow and tonemapping, so a
flare blooms and is exposed with the rest of the frame instead of being painted
over a finished image. Six analytic element shapes plus nine bundled textures,
per camera filtering, depth buffer occlusion, and a **Generate flare** button in
the 3D toolbar that sets a flare up from whatever light is selected.

**Ray traced lighting.** Ambient occlusion, sun shadows, one bounce global
illumination and reflections, traced against a top level acceleration structure
built from the scene's meshes, denoised and accumulated over time. Behind the
`Asos Lighting` toggle in the environment.

**Rope3D.** Verlet simulated rope that can pin either end to any `Node3D`, drawn
either as a generated tube or as a mesh instanced per segment.

## Godot 4.7 compatibility

A Godot 4.7 project opens and runs in Asosdot as it does in Godot 4.7, and goes
back the same way:

- **Your class names come first.** When a project's own extension or script
  class has the name of an Asosdot class, such as the Terrain3D extension or a
  lens flare add-on's `LensFlare`, the project's class takes the name, as it
  does in Godot 4.7. Asosdot then leaves that feature out of the project,
  together with its editor tools, and says so in the Output panel. Asosdot's
  internal editor classes all start with `Asosdot`, so they can't clash at all.
- **Editor state means the same in both.** The 3D viewport's display mode and
  the selected main screen are saved with the numbers Godot 4.7 uses.
- **Imports go back cleanly.** A model Asosdot imported with its own nodes, such
  as a rig's `BlenderConstraint3D`s, is imported again by Godot 4.7 instead of
  loaded with nodes it doesn't have. A `.blend` that Godot 4.7 imported is
  imported again by Asosdot, where Blender is set up, to add what Asosdot adds.

Scenes you save with Asosdot's own nodes keep them, and Godot 4.7 keeps them
too, as placeholders it can't run. Everything Asosdot adds to Godot's own
classes is new, so nothing a Godot 4.7 project or GDExtension uses has changed.

Run the check with `python misc/scripts/test_godot_compat.py --engine
bin/asosdot.windows.editor.x86_64.exe --godot <Godot 4.7 editor>`.

## Licence

Asosdot Engine is released under the [MIT licence](LICENSE.txt), the same terms
as the Godot Engine it is built on. Godot's copyright notices are retained in
`LICENSE.txt` and `COPYRIGHT.txt`, as that licence requires, and the About
dialog lists every third-party component the engine ships.

Godot Engine is © 2014-present Godot Engine contributors and © 2007-2014 Juan
Linietsky, Ariel Manzur. "Godot" is a trademark of the Godot Foundation; this
fork is not affiliated with or endorsed by them.

The bundled lens flare textures are CC0, by hc (hackcraft.de), from
[OpenGameArt](https://opengameart.org/content/lens-flares-and-particles). See
[thirdparty/lens_flare/README.md](thirdparty/lens_flare/README.md) for the
changes made to them.

## Exporting games

Games export with Asosdot's own export templates, built from the same source as
the editor. Godot's templates for the same base version would make games without
Asosdot's nodes (such as `BlenderConstraint3D` and `Rope3D`) and fixes (such as
layered audio and camera cuts), so Asosdot keeps its templates in a folder of
their own, `export_templates/asosdot.1.0.stable`. It never downloads Godot's, and
**Install from File** refuses a Godot `.tpz` with a message saying why.

Build and package the Windows templates with:

```
scons platform=windows target=template_release arch=x86_64 d3d12=yes
scons platform=windows target=template_debug arch=x86_64 d3d12=yes
python misc/scripts/make_export_templates.py
```

That makes `bin/Asosdot-Export-Templates-1.0.stable.tpz`. Install it from
**Editor > Manage Export Templates > Install from File**. The script packages
whatever templates are in `bin/`: Linux and Web templates build the same way
with `platform=linuxbsd` or `platform=web` (Web needs Emscripten).

The macOS template needs a Mac, so GitHub builds it. Push a tag to start the
`Asosdot macOS export templates` workflow:

```
git tag asosdot-macos-templates-<n>
git push origin asosdot-macos-templates-<n>
```

Its `asosdot-macos-templates` artifact holds a universal `macos.zip`; put that
in `bin/` before running the packaging script. A tag with `-debug` in its name
builds the debug template too. A Windows editor exports macOS games from it,
signed ad hoc with the built-in codesign. iOS templates still need a Mac with
Xcode.

GDExtensions a project uses, such as GodotSteam or Terrain3D, export with it as
they do with Godot's templates.

## Building

```
scons platform=windows target=editor d3d12=yes
```

`d3d12=yes` needs the Direct3D 12 SDK files, which
`python misc/scripts/install_d3d12_sdk_windows.py` downloads; `d3d12=no`
builds without the Direct3D 12 renderer. Either way it goes on the command
line: `platform/windows/detect.py` forces it on and only the command line can
override a platform flag. The binary lands in `bin/` named after `short_name`
in `version.py`.

For everything else, Godot's own
[build documentation](https://docs.godotengine.org/en/latest/contributing/development/compiling/)
applies unchanged.
