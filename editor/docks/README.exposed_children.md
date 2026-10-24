# Exposed children

**Editable Children** opens a whole instance: every node of its scene shows in the Scene dock, most of them nothing you meant to touch. **Expose in Instances** lets a scene choose what other scenes can reach instead.

## Exposing a node

In the scene the node belongs to, right-click it in the Scene dock and choose **Expose in Instances**. The node gets the exposed badge, a box with a node coming out of it. Click the badge, or choose the menu item again, to stop exposing it. Several nodes can be exposed at once; the scene's root can't be, since it is the instance itself. **Toggle Expose in Instances** in the editor's shortcut settings can be given a key.

An exposed instance of another scene brings what that scene exposes: if `player.tscn` exposes its `Gun` instance, and `gun.tscn` exposes `Muzzle`, a level with a player can reach both.

## In a scene that instances it

The Scene dock shows the instance's exposed nodes under it, tinted as nodes of another scene, and nothing else of the instance:

```text
Level
  Player
    Anim
    Hand        (Player/Body/Hand: Body isn't exposed, so it isn't shown)
    Gun
      Muzzle
```

The instance's tooltip lists what it exposes, and an exposed node's tooltip says where it is in its scene. An exposed node is edited as with Editable Children: change its properties, add nodes under it, add it to groups and connect its signals. It can't be renamed, moved or deleted, since it belongs to its scene. The scene saves only what changed.

In the 2D and 3D viewports, a click on an instance selects the instance, as always. Clicking the selected instance again, without dragging, selects the node its scene exposes at that spot; another click goes back to the instance. **Alt + right-click** lists both. Dragging a selected instance or exposed node moves it.

## Changes are never lost

- If a scene stops exposing a node another scene changed, that scene keeps showing the node with a warning badge and keeps its changes, until they are reverted.
- Turning off Editable Children keeps the changes to exposed nodes and the nodes added under them. Only the rest of the instance is reverted, and nodes added under hidden nodes move to the instance, as before.
- Renaming or moving an exposed node in its scene keeps the changes other scenes made to it, through the scene's node IDs.
- **Make Local** turns an instance into the scene's own nodes, which are then not exposed until the scene exposes them.

Exposing changes nothing at runtime: the node can always be reached with `get_node()`. A game that loads a scene and packs it again keeps the changes to exposed nodes, as it does for editable children. Scenes that expose nothing behave exactly as before.

In scripts, `Node.exposed_in_instances` is the flag, and `scene_root.is_node_editable(node)` says whether a scene can edit one of its nodes, which is what the Scene dock and saving go by.

## Verification

Build the editor, then run:

```text
python misc/scripts/test_exposed_children.py --engine bin/asosdot.windows.editor.x86_64.exe
```

It builds a level that instances a player, which instances a gun, and checks the Scene dock, then edits exposed nodes, adds a node under one, groups and connects them, saves, and checks the file and a runtime load and pack. It then stops exposing a node the level changed, turns Editable Children on and off, toggles exposure from the menu in the player's own scene, and clicks the 3D and 2D viewports to check selection. `--state-only` skips the clicks, which need a graphics run.
