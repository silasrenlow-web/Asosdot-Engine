/**************************************************************************/
/*  voxel_terrain_3d.h                                                    */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "terrain_layer_textures.h"
#include "terrain_material.h"
#include "voxel_mesher.h"
#include "voxel_terrain_data.h"

#include "core/object/worker_thread_pool.h"
#include "scene/3d/node_3d.h"

class ArrayMesh;
class NavigationMesh;
class NavigationMeshSourceGeometryData3D;

// Terrain made of voxels, so it can have caves, overhangs, arches and
// tunnels, and be dug into while the game runs.
//
// The surface is extracted with marching cubes in blocks of 32 cells. Blocks
// near the viewer are meshed at full detail and each step further out halves
// it, following an octree, so the view can reach hundreds of metres while
// only the ground nearby costs full price. Meshing runs on worker threads,
// and a block keeps its old mesh until the new one is ready, so edits and
// camera moves never leave holes.
class VoxelTerrain3D : public Node3D {
	GDCLASS(VoxelTerrain3D, Node3D);

public:
	enum DebugView {
		DEBUG_VIEW_DISABLED,
		DEBUG_VIEW_NORMALS,
		DEBUG_VIEW_LAYERS,
		DEBUG_VIEW_LOD,
	};

	enum ShadowCasting {
		SHADOW_CASTING_OFF,
		SHADOW_CASTING_ON,
		SHADOW_CASTING_DOUBLE_SIDED,
		SHADOW_CASTING_SHADOWS_ONLY,
	};

	static constexpr int MAX_LOD = 9;

private:
	struct Chunk {
		RID mesh;
		RID instance;
		RID body;
		RID shape;
		// Part of the current level of detail selection. Chunks that are not
		// stay drawn until whatever replaces them is ready.
		bool wanted = true;
		// A mesh, perhaps an empty one, has been built at least once.
		bool ready = false;
		// Needs building, or building again.
		bool dirty = true;
		bool building = false;
		bool wants_collision = false;
		bool has_collision = false;
		// Tells a chunk from an earlier one with the same key, so a job
		// started for a chunk that has since gone is not applied to its
		// replacement.
		uint32_t generation = 0;
	};

	struct Job {
		Vector4i key;
		uint32_t generation = 0;
		Ref<VoxelTerrainData> data;
		Vector3i origin;
		int step = 1;
		float voxel_size = 1.0;
		bool collision = false;
		VoxelMesher::Output output;
		WorkerThreadPool::TaskID task = WorkerThreadPool::INVALID_TASK_ID;

		static void run(void *p_job);
	};

	Ref<VoxelTerrainData> data;
	Ref<TerrainMaterial> terrain_material;

	float view_distance = 512.0;
	float lod_distance = 64.0;
	ShadowCasting cast_shadow = SHADOW_CASTING_ON;
	uint32_t render_layers = 1;
	NodePath viewer_path;

	bool collision_enabled = true;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	real_t collision_priority = 1.0;
	float collision_distance = 64.0;

	DebugView debug_view = DEBUG_VIEW_DISABLED;

	RID material;
	TerrainLayerTextures layer_textures;
	// Keyed by (x, y, z, lod), x, y and z counting chunks of that level.
	HashMap<Vector4i, Chunk> chunks;
	LocalVector<Job *> jobs;

	Vector3 last_viewer;
	bool lod_dirty = true;
	bool layers_dirty = true;
	bool material_dirty = true;
	bool retire_pending = false;
	bool was_meshing = false;

	// Editor brush preview.
	bool brush_visible = false;
	Vector3 brush_position;
	float brush_radius = 1.0;
	float brush_hardness = 0.5;
	Color brush_color = Color(1, 1, 1);

	static Vector3 editor_viewer;
	static bool has_editor_viewer;

	void _data_changed_area(const AABB &p_voxel_box);
	void _data_generator_changed();
	void _data_layers_changed();
	void _material_changed();

	void _process_updates();
	void _update_material_params();
	void _update_lod(const Vector3 &p_viewer);
	void _select(const Vector4i &p_key, const Vector3 &p_viewer, HashMap<Vector4i, bool> &r_wanted) const;
	void _start_jobs(const Vector3 &p_viewer);
	void _finish_jobs();
	void _apply(const Vector4i &p_key, Chunk &r_chunk, Job &p_job);
	void _retire_chunks();
	void _free_chunk(Chunk &r_chunk);
	void _free_collision(Chunk &r_chunk);
	void _free_all();
	void _wait_for_jobs();
	void _update_instance_settings(const Vector4i &p_key, const Chunk &p_chunk) const;
	void _update_body_settings(const Chunk &p_chunk) const;
	void _update_transforms();
	void _update_visibility();
	void _mark_all_dirty();

	Vector3 _get_origin() const;
	Vector3 _get_viewer() const;
	float _get_voxel_size() const;
	static AABB _chunk_box(const Vector4i &p_key);
	static int _chunk_step(const Vector4i &p_key) { return 1 << p_key.w; }
	// Meshes the blocks of one level of detail that cover a box, into one
	// output, or just their triangles for physics and navigation.
	bool _bake(const AABB &p_global_box, int p_lod, bool p_faces_only, VoxelMesher::Output &r_output) const;

	static Callable _navmesh_source_geometry_parsing_callback;
	static RID _navmesh_source_geometry_parser;

protected:
	void _notification(int p_what);
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_data(const Ref<VoxelTerrainData> &p_data);
	Ref<VoxelTerrainData> get_data() const { return data; }
	void set_terrain_material(const Ref<TerrainMaterial> &p_material);
	Ref<TerrainMaterial> get_terrain_material() const { return terrain_material; }

	void set_view_distance(float p_distance);
	float get_view_distance() const { return view_distance; }
	void set_lod_distance(float p_distance);
	float get_lod_distance() const { return lod_distance; }
	void set_cast_shadow(ShadowCasting p_mode);
	ShadowCasting get_cast_shadow() const { return cast_shadow; }
	void set_render_layers(uint32_t p_layers);
	uint32_t get_render_layers() const { return render_layers; }
	void set_viewer_path(const NodePath &p_path);
	NodePath get_viewer_path() const { return viewer_path; }

	void set_collision_enabled(bool p_enabled);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_collision_priority(real_t p_priority);
	real_t get_collision_priority() const { return collision_priority; }
	void set_collision_distance(float p_distance);
	float get_collision_distance() const { return collision_distance; }

	void set_debug_view(DebugView p_view);
	DebugView get_debug_view() const { return debug_view; }

	// Positions in global space, distances in metres.
	Vector3 global_to_voxel(const Vector3 &p_global_position) const;
	Vector3 voxel_to_global(const Vector3 &p_voxel_position) const;
	float get_distance(const Vector3 &p_global_position) const;
	Vector3 get_normal(const Vector3 &p_global_position) const;
	int get_layer(const Vector3 &p_global_position) const;
	String get_surface_name(const Vector3 &p_global_position) const;
	Dictionary intersect_ray(const Vector3 &p_from, const Vector3 &p_direction, float p_max_distance = 1000.0) const;
	bool intersect_ray_fast(const Vector3 &p_from, const Vector3 &p_direction, float p_max_distance, Vector3 &r_position) const;

	// Editing while the game runs, in global space.
	// A layer of -1 keeps whatever the voxels were made of.
	void add_sphere(const Vector3 &p_global_position, float p_radius, int p_layer = -1);
	void dig_sphere(const Vector3 &p_global_position, float p_radius);
	void smooth_sphere(const Vector3 &p_global_position, float p_radius, float p_strength = 0.5);
	void flatten_sphere(const Vector3 &p_global_position, float p_radius, float p_strength = 0.5);
	void paint_sphere(const Vector3 &p_global_position, float p_radius, int p_layer);

	Ref<ArrayMesh> bake_mesh(const AABB &p_global_box, int p_lod = 0) const;
	void rebuild();
	bool is_meshing() const;
	int get_chunk_count() const;

	// For the editor.
	static void set_editor_viewer(const Vector3 &p_position);
	void set_brush_preview(bool p_visible, const Vector3 &p_position = Vector3(), float p_radius = 1.0, float p_hardness = 0.5, const Color &p_color = Color(1, 1, 1));

	PackedStringArray get_configuration_warnings() const override;

	static void navmesh_parse_init();
	static void navmesh_parse_source_geometry(const Ref<NavigationMesh> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData3D> p_source_geometry_data, Node *p_node);

	VoxelTerrain3D();
	~VoxelTerrain3D();
};

VARIANT_ENUM_CAST(VoxelTerrain3D::DebugView);
VARIANT_ENUM_CAST(VoxelTerrain3D::ShadowCasting);
