#!/usr/bin/env python3
"""Check NavigationMesh.filter_isolated_regions, headless, with the engine's own bake.

Each scene is baked twice from code, with the filter off and on (it is on by default).
Walls stand on a floor that runs through them, so the voxel bake finds a walkable patch
on the floor inside the wall, which is where phase-through enemies path and get stuck. The
filter has to remove those patches and nothing else: floors, roofs, balconies, the tops of
walls, rooms (also sealed ones, also ones whose mesh faces inward) and the rest of the level
stay. One scene is a building with nothing outside it, the layout the old heightfield test
missed: the patch between a wall's inner face and the outer skin, with void beyond."""

from __future__ import annotations

import argparse
import shutil
import tempfile
from pathlib import Path

from test_blender_roundtrip import run

TEST = r"""
extends Node

var fails := 0


func check(p_name: String, p_ok: bool, p_detail: Variant = "") -> void:
	print(("PASS " if p_ok else "FAIL ") + p_name + "  " + str(p_detail))
	if not p_ok:
		fails += 1


func floor_mesh(p_size: float) -> PlaneMesh:
	var mesh := PlaneMesh.new()
	mesh.size = Vector2(p_size, p_size)
	return mesh


func box_mesh(p_size: Vector3) -> BoxMesh:
	var mesh := BoxMesh.new()
	mesh.size = p_size
	return mesh


# A box standing on the floor: p_center is the middle of its footprint, p_size its extent.
func add_box(p_geometry: NavigationMeshSourceGeometryData3D, p_center: Vector3, p_size: Vector3) -> void:
	p_geometry.add_mesh(box_mesh(p_size), Transform3D(Basis(), p_center + Vector3(0, p_size.y * 0.5, 0)))


func bake(p_filter: bool, p_build: Callable, p_floor := 40.0) -> Dictionary:
	var geometry := NavigationMeshSourceGeometryData3D.new()
	if p_floor > 0.0:
		geometry.add_mesh(floor_mesh(p_floor), Transform3D())
	p_build.call(geometry)
	var navigation_mesh := NavigationMesh.new()
	navigation_mesh.filter_isolated_regions = p_filter
	NavigationServer3D.bake_from_source_geometry_data(navigation_mesh, geometry)
	var vertices := navigation_mesh.get_vertices()
	var result := {"polygons": navigation_mesh.get_polygon_count(), "area": 0.0, "points": []}
	for i in navigation_mesh.get_polygon_count():
		var indices := navigation_mesh.get_polygon(i)
		var corners := []
		for index in indices:
			corners.append(vertices[index])
		var area := 0.0
		var centroid := Vector3()
		for j in range(1, corners.size() - 1):
			area += absf((corners[j] - corners[0]).cross(corners[j + 1] - corners[0]).y) * 0.5
		for corner in corners:
			centroid += corner
		result.area += area
		result.points.append([centroid / corners.size(), area])
	return result


# The area of the polygons whose middle is in p_box (an AABB).
func area_in(p_result: Dictionary, p_box: AABB) -> float:
	var total := 0.0
	for entry in p_result.points:
		if p_box.has_point(entry[0]):
			total += entry[1]
	return total


func report(p_scene: String, p_off: Dictionary, p_on: Dictionary) -> void:
	print("INFO %s: filter off %.1f m2 in %d polygons, on %.1f m2 in %d" % [p_scene, p_off.area, p_off.polygons, p_on.area, p_on.polygons])
	# Where the filter took polygons away, as boxes of what was removed.
	var removed := []
	for entry in p_off.points:
		var kept := false
		for other in p_on.points:
			if (other[0] - entry[0]).length() < 0.01:
				kept = true
				break
		if not kept:
			removed.append(entry)
	var box := AABB()
	var area := 0.0
	for i in removed.size():
		box = AABB(removed[i][0], Vector3()) if i == 0 else box.expand(removed[i][0])
		area += removed[i][1]
	if removed:
		print("INFO   removed %.1f m2 in %d polygons, around %s" % [area, removed.size(), box])


func _ready() -> void:
	var floor_level := AABB(Vector3(-20, -0.2, -20), Vector3(40, 1.0, 40)) # Everything standing on the ground.
	var raised := AABB(Vector3(-20, 1.5, -20), Vector3(40, 3.0, 40)) # Tops of platforms and walls.

	# 1. A thick wall, 14 x 3 x 3 m, closed. The floor runs through it.
	var thick_wall := func(p_geometry): add_box(p_geometry, Vector3(0, 0, 0), Vector3(14, 3, 3))
	var inside_wall := AABB(Vector3(-6.5, -0.2, -1.6), Vector3(13, 1.0, 3.2))
	var off: Dictionary = bake(false, thick_wall)
	var on: Dictionary = bake(true, thick_wall)
	report("thick wall", off, on)
	var patch_off := area_in(off, inside_wall)
	var patch_on := area_in(on, inside_wall)
	check("thick wall: without the filter a patch is baked inside the wall", patch_off > 1.0, patch_off)
	check("thick wall: with the filter the patch inside the wall is gone", patch_on < 0.01, patch_on)
	check("thick wall: the rest of the floor is untouched", absf((area_in(on, floor_level) - area_in(off, floor_level)) + patch_off) < 0.5, [area_in(off, floor_level), area_in(on, floor_level), patch_off])
	check("thick wall: its top, which an agent can stand on, is kept", absf(area_in(on, raised) - area_in(off, raised)) < 0.01 and area_in(on, raised) > 1.0, area_in(on, raised))

	# 2. Two crossing thick walls.
	var cross := func(p_geometry):
		add_box(p_geometry, Vector3(0, 0, 0), Vector3(14, 3, 3))
		add_box(p_geometry, Vector3(0, 0, 0), Vector3(3, 3, 14))
	off = bake(false, cross)
	on = bake(true, cross)
	report("crossing walls", off, on)
	check("crossing walls: the floor stays", on.area > 0.9 * off.area, [off.area, on.area])
	check("crossing walls: nothing is left inside them", area_in(on, inside_wall) < 0.01 and area_in(on, AABB(Vector3(-1.6, -0.2, -6.5), Vector3(3.2, 1, 13))) < 0.01)

	# 3. A raised platform and a balcony: open areas, which are not inside anything. The floor
	# under them is inside them, so it goes; what stands on them stays.
	var platforms := func(p_geometry):
		add_box(p_geometry, Vector3(-10, 0, -10), Vector3(8, 2, 8))
		add_box(p_geometry, Vector3(10, 0, 10), Vector3(2, 2, 2))
	off = bake(false, platforms)
	on = bake(true, platforms)
	report("platforms", off, on)
	check("platforms: a raised platform stays", area_in(on, AABB(Vector3(-14, 1.5, -14), Vector3(8, 1, 8))) > 5.0)
	check("platforms: a small balcony stays", area_in(on, AABB(Vector3(8.5, 1.5, 8.5), Vector3(3, 1, 3))) > 0.3, area_in(on, AABB(Vector3(8.5, 1.5, 8.5), Vector3(3, 1, 3))))
	check("platforms: nothing raised is lost", absf(area_in(on, raised) - area_in(off, raised)) < 0.01, [area_in(off, raised), area_in(on, raised)])

	# 4. A very thick wall, 8 m: there is no limit on how thick a wall may be.
	var huge_wall := func(p_geometry): add_box(p_geometry, Vector3(0, 0, 0), Vector3(16, 3, 8))
	var inside_huge := AABB(Vector3(-8, -0.2, -4), Vector3(16, 1, 8))
	off = bake(false, huge_wall)
	on = bake(true, huge_wall)
	report("8 m thick wall", off, on)
	check("8 m wall: a patch is baked inside it", area_in(off, inside_huge) > 10.0, area_in(off, inside_huge))
	check("8 m wall: and the filter removes it", area_in(on, inside_huge) < 0.01, area_in(on, inside_huge))

	# 5. A room with a doorway: walls 0.3 m thick, 8 x 8 m, a 2.2 m gap, a ceiling.
	var room_with_door := func(p_geometry): add_room(p_geometry, true)
	off = bake(false, room_with_door)
	on = bake(true, room_with_door)
	var room := AABB(Vector3(-3, -0.2, -3), Vector3(6, 1, 6))
	report("room with a doorway", off, on)
	check("room with a doorway: it is walkable and connected, so it stays", area_in(on, room) > 20.0 and absf(area_in(on, floor_level) - area_in(off, floor_level)) < 0.5, [area_in(off, floor_level), area_in(on, floor_level)])

	# 6. The same room with its door shut as part of the bake geometry. It is cut off from
	# the rest like the inside of a wall, but its walls face into it, so it stays.
	var room_shut := func(p_geometry): add_room(p_geometry, false)
	off = bake(false, room_shut)
	on = bake(true, room_shut)
	report("room with a shut door", off, on)
	check("room with a shut door: it is cut off from the rest", area_in(off, room) > 20.0 and area_in(off, floor_level) - area_in(off, room) > 1000.0)
	check("room with a shut door: the room is kept", absf(area_in(on, room) - area_in(off, room)) < 0.01, [area_in(off, room), area_in(on, room)])
	check("room with a shut door: the roof on top of it stays", absf(area_in(on, raised) - area_in(off, raised)) < 0.01 and area_in(on, raised) > 20.0, area_in(on, raised))

	# 7. A building with nothing outside it: a 20 x 20 m floor slab only, walls 3 m thick
	# around its edge. The patch inside the walls has void beyond them, not floor.
	var building := func(p_geometry):
		p_geometry.add_mesh(floor_mesh(20.0), Transform3D())
		add_box(p_geometry, Vector3(0, 0, -8.5), Vector3(20, 4, 3))
		add_box(p_geometry, Vector3(0, 0, 8.5), Vector3(20, 4, 3))
		add_box(p_geometry, Vector3(-8.5, 0, 0), Vector3(3, 4, 14))
		add_box(p_geometry, Vector3(8.5, 0, 0), Vector3(3, 4, 14))
	off = bake(false, building, 0.0)
	on = bake(true, building, 0.0)
	report("building over void", off, on)
	var hall := AABB(Vector3(-6.9, -0.2, -6.9), Vector3(13.8, 1, 13.8))
	var in_walls := area_in(off, floor_level) - area_in(off, hall)
	check("building over void: patches are baked inside its walls", in_walls > 1.0, in_walls)
	check("building over void: the filter removes them", absf(area_in(on, floor_level) - area_in(on, hall)) < 0.01, area_in(on, floor_level) - area_in(on, hall))
	check("building over void: the hall stays", absf(area_in(on, hall) - area_in(off, hall)) < 0.01 and area_in(on, hall) > 80.0, area_in(on, hall))
	check("building over void: the wall tops stay", absf(area_in(on, raised) - area_in(off, raised)) < 0.01, [area_in(off, raised), area_in(on, raised)])

	# 8. A sealed room modeled as one box with its faces turned inward, the way a room is often
	# built: it is cut off from the floor around it, and it stays, because it faces into itself.
	var inward_room := func(p_geometry):
		var faces := box_mesh(Vector3(6, 3, 6)).get_faces()
		var flipped := PackedVector3Array()
		for i in range(0, faces.size(), 3):
			flipped.append_array([faces[i], faces[i + 2], faces[i + 1]])
		p_geometry.add_faces(flipped, Transform3D(Basis(), Vector3(10, 1.5, 10)))
	off = bake(false, inward_room)
	on = bake(true, inward_room)
	report("inward-facing room", off, on)
	var inward := AABB(Vector3(7.2, -0.2, 7.2), Vector3(5.6, 1, 5.6))
	check("inward-facing room: it is baked", area_in(off, inward) > 10.0, area_in(off, inward))
	check("inward-facing room: and kept", absf(area_in(on, inward) - area_in(off, inward)) < 0.01, [area_in(off, inward), area_in(on, inward)])

	print("NAVMESH_INSIDE_WALLS fails=%d" % fails)
	get_tree().quit()


func add_room(p_geometry: NavigationMeshSourceGeometryData3D, p_open_door: bool) -> void:
	add_box(p_geometry, Vector3(0, 0, -4), Vector3(8.3, 3, 0.3))
	add_box(p_geometry, Vector3(-4, 0, 0), Vector3(0.3, 3, 8.3))
	add_box(p_geometry, Vector3(4, 0, 0), Vector3(0.3, 3, 8.3))
	add_box(p_geometry, Vector3(-2.625, 0, 4), Vector3(3.05, 3, 0.3))
	add_box(p_geometry, Vector3(2.625, 0, 4), Vector3(3.05, 3, 0.3))
	if not p_open_door:
		add_box(p_geometry, Vector3(0, 0, 4), Vector3(2.2, 2.4, 0.2))
	p_geometry.add_mesh(box_mesh(Vector3(8.3, 0.2, 8.3)), Transform3D(Basis(), Vector3(0, 3.1, 0)))
"""


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", required=True, type=Path)
    parser.add_argument("--temp-dir", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="asosdot-navwalls-", dir=args.temp_dir) as directory:
        work = Path(directory)
        runtime = work / "runtime"
        runtime.mkdir()
        isolated = runtime / args.engine.name
        shutil.copy2(args.engine.resolve(), isolated)
        if args.engine.name.endswith(".console.exe"):
            main_exe = args.engine.resolve().with_name(args.engine.name.replace(".console.exe", ".exe"))
            shutil.copy2(main_exe, runtime / main_exe.name)
        (runtime / "_sc_").touch()
        project = work / "project"
        project.mkdir()
        (project / "test.gd").write_text(TEST, encoding="utf-8")
        (project / "main.tscn").write_text(
            '[gd_scene load_steps=2 format=3]\n\n[ext_resource type="Script" path="res://test.gd" id="1"]\n\n'
            '[node name="Main" type="Node"]\nscript = ExtResource("1")\n',
            encoding="utf-8",
        )
        (project / "project.godot").write_text(
            f'config_version=5\n[application]\nconfig/name="Navmesh walls {work.name}"\nrun/main_scene="res://main.tscn"\n',
            encoding="utf-8",
        )
        command = [str(isolated), "--headless", "--path", str(project)]
        run(command, "NAVMESH_INSIDE_WALLS fails=0", work)
        log = (work / "NAVMESH_INSIDE_WALLS fails=0.log").read_text(encoding="utf-8", errors="replace")
        print("\n".join(line for line in log.splitlines() if line.startswith(("PASS", "FAIL", "INFO"))), flush=True)
        print("NAVMESH_INSIDE_WALLS_TEST_OK", flush=True)


if __name__ == "__main__":
    main()
