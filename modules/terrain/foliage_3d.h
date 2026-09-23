/**************************************************************************/
/*  foliage_3d.h                                                          */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "foliage_data.h"

#include "core/templates/hash_set.h"
#include "scene/3d/node_3d.h"

class NavigationMesh;
class NavigationMeshSourceGeometryData3D;
class Terrain3D;

// Draws a FoliageData on the Terrain3D it points at, normally its parent.
//
// Grass is grown in square cells around the camera within each type's
// visibility range, a few cells per frame, and dropped again behind it. Objects
// are built once into cells of their own, as a MultiMesh with collision or as
// instanced scenes, and rebuilt only where something changed.
class Foliage3D : public Node3D {
	GDCLASS(Foliage3D, Node3D);

	struct Cell {
		RID multimesh;
		RID instance;
		RID body;
		LocalVector<ObjectID> scenes;
		Vector3 origin;
		int count = 0;
	};

	NodePath terrain_path = NodePath("..");
	Ref<FoliageData> data;
	float grass_cell_size = 16.0;
	float object_cell_size = 64.0;
	float generation_budget_msec = 2.0;
	bool collision_enabled = true;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;

	ObjectID terrain_id;
	// Keyed by (cell x, cell z, type index).
	HashMap<Vector3i, Cell> grass_cells;
	HashMap<Vector3i, Cell> object_cells;
	HashSet<Vector3i> dirty_grass;
	HashSet<Vector3i> dirty_objects;
	HashSet<int> dirty_object_types;
	bool layout_dirty = true;

	static Vector3 editor_focus;
	static bool editor_focus_valid;

	static Callable _navmesh_source_geometry_parsing_callback;
	static RID _navmesh_source_geometry_parser;

	Terrain3D *_get_terrain();
	Terrain3D *_peek_terrain() const;
	bool _get_focus(Vector3 &r_local) const;

	void _data_types_changed();
	void _data_type_changed(int p_type);
	void _data_density_changed(int p_type, const Rect2 &p_area);
	void _data_instances_changed(int p_type, const Rect2 &p_area);
	void _terrain_changed(const Rect2 &p_area);

	void _free_cell(Cell &r_cell);
	void _free_all();
	void _process_updates();
	void _update_grass(Terrain3D *p_terrain);
	void _generate_grass_cell(Terrain3D *p_terrain, const Vector3i &p_key);
	void _rebuild_object_type(Terrain3D *p_terrain, int p_type);
	void _rebuild_object_cell(Terrain3D *p_terrain, const Vector3i &p_key);
	void _build_object_cell(Terrain3D *p_terrain, const Vector3i &p_key, const LocalVector<int> &p_instances);
	void _update_transforms();

	Vector2i _cell_of(const Vector2 &p_local, float p_size) const;
	void _mark_cells(HashSet<Vector3i> &r_set, const HashMap<Vector3i, Cell> &p_cells, int p_type, const Rect2 &p_area, float p_size, bool p_create);

protected:
	void _notification(int p_what);
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_terrain_path(const NodePath &p_path);
	NodePath get_terrain_path() const { return terrain_path; }
	void set_data(const Ref<FoliageData> &p_data);
	Ref<FoliageData> get_data() const { return data; }
	void set_grass_cell_size(float p_size);
	float get_grass_cell_size() const { return grass_cell_size; }
	void set_object_cell_size(float p_size);
	float get_object_cell_size() const { return object_cell_size; }
	void set_generation_budget_msec(float p_msec);
	float get_generation_budget_msec() const { return generation_budget_msec; }
	void set_collision_enabled(bool p_enabled);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }

	Terrain3D *get_terrain() const { return _peek_terrain(); }
	void regenerate();

	// Transforms of every object of a type, in global space, as drawn.
	Vector<Transform3D> compute_object_transforms(int p_type) const;
	// Transform for one object at a spot, applying the type's rules for
	// rotation, tilt, alignment and scale. False if the rules reject the spot.
	bool compute_placement(int p_type, const Vector2 &p_local, float p_yaw, float p_scale, float p_variation, float p_height_offset, Transform3D &r_local_transform, bool p_check_rules = true) const;

	// The editor's viewport camera, since there is no scene camera to follow
	// while editing.
	static void set_editor_focus(const Vector3 &p_global_position);

	PackedStringArray get_configuration_warnings() const override;

	static void navmesh_parse_init();
	static void navmesh_parse_source_geometry(const Ref<NavigationMesh> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData3D> p_source_geometry_data, Node *p_node);

	Foliage3D();
	~Foliage3D();
};
