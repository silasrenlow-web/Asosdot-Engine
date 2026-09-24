/**************************************************************************/
/*  terrain_3d.h                                                          */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "terrain_data.h"
#include "terrain_layer_textures.h"
#include "terrain_material.h"

#include "core/templates/hash_set.h"
#include "scene/3d/node_3d.h"

class NavigationMesh;
class NavigationMeshSourceGeometryData3D;

// A heightmap terrain drawn as a grid of culled chunks.
//
// Every chunk shares one flat grid mesh with its levels of detail baked in, and
// the vertex shader lifts it to the heights held in texture arrays, one slice
// per region. So each chunk is culled, shadowed and LOD selected by the
// renderer like any other instance, while an edit only re-uploads the regions
// it touched instead of rebuilding geometry.
class Terrain3D : public Node3D {
	GDCLASS(Terrain3D, Node3D);

public:
	enum DebugView {
		DEBUG_VIEW_DISABLED,
		DEBUG_VIEW_HEIGHT,
		DEBUG_VIEW_NORMALS,
		DEBUG_VIEW_LAYERS,
		DEBUG_VIEW_COLOR,
		DEBUG_VIEW_WETNESS,
		DEBUG_VIEW_GRID,
	};

	enum ShadowCasting {
		SHADOW_CASTING_OFF,
		SHADOW_CASTING_ON,
		SHADOW_CASTING_DOUBLE_SIDED,
		SHADOW_CASTING_SHADOWS_ONLY,
	};

	static constexpr int LOD_COUNT = 5;

	// Everything foliage needs to know about one spot, in one lookup.
	struct SurfaceSample {
		float height = 0.0; // Relative to the terrain's origin.
		Vector3 normal;
		float weights[TerrainData::MAX_LAYERS] = {};
		Color color;
	};

private:
	struct Chunk {
		RID instance;
		float min_height = 0.0;
		float max_height = 0.0;
		// How rough the chunk is compared with what the shared mesh's LOD
		// errors assume; scales lod_bias for this chunk alone.
		float lod_scale = 1.0;
	};

	Ref<TerrainData> data;
	Ref<TerrainMaterial> terrain_material;

	float vertex_spacing = 1.0;
	int chunk_size = 64;
	float lod_bias = 1.0;
	ShadowCasting cast_shadow = SHADOW_CASTING_ON;
	uint32_t render_layers = 1;
	bool occlusion_culling = true;

	bool collision_enabled = true;
	uint32_t collision_layer = 1;
	uint32_t collision_mask = 1;
	real_t collision_priority = 1.0;

	DebugView debug_view = DEBUG_VIEW_DISABLED;

	// Rendering.
	RID mesh;
	RID material;
	HashMap<Vector2i, Chunk> chunks;
	bool mesh_dirty = true;

	// One slice per region, in the order of region_slices.
	RID height_array;
	RID normal_array;
	RID splat_arrays[TerrainData::SPLAT_MAP_COUNT];
	int splat_array_count = 0;
	RID color_array;
	RID region_map_texture;
	HashMap<Vector2i, int> region_slices;
	Vector<Vector2i> slice_regions;
	Rect2i region_map_rect;
	// Normals are derived, so they live here rather than in the data.
	HashMap<Vector2i, Ref<Image>> normal_maps;

	TerrainLayerTextures layer_textures;

	// Collision and occlusion, one per region.
	RID static_body;
	HashMap<Vector2i, RID> collision_shapes;
	HashMap<Vector2i, RID> occluders;
	HashMap<Vector2i, RID> occluder_instances;

	// Work waiting for the next frame, so a burst of edits costs one upload.
	bool layout_dirty = true;
	bool layers_dirty = true;
	bool material_dirty = true;
	HashMap<Vector2i, uint32_t> dirty_regions;
	HashSet<Vector2i> dirty_collision;
	Rect2 pending_changed_area;
	bool has_pending_changed_area = false;
	uint64_t last_collision_update_usec = 0;
	bool collision_immediate = true;

	// Editor brush preview.
	bool brush_visible = false;
	Vector3 brush_position;
	float brush_radius = 1.0;
	float brush_hardness = 0.5;
	Color brush_color = Color(1, 1, 1);
	bool brush_segment = false;
	Vector3 brush_segment_start;

	void _data_region_changed(const Vector2i &p_region, int p_maps);
	void _data_regions_changed();
	void _data_layers_changed();
	void _material_changed();

	void _queue_update();
	void _process_updates();

	void _free_rendering();
	void _free_collision();
	void _rebuild_mesh();
	void _rebuild_layout();
	void _rebuild_layer_textures(bool p_force);
	void _update_material_params();
	void _update_region_textures(const Vector2i &p_region, uint32_t p_maps);
	void _update_normals(const Rect2i &p_vertex_rect);
	void _update_chunks_in(const Rect2i &p_vertex_rect);
	void _update_chunk(const Vector2i &p_chunk);
	void _update_instance_settings(const Chunk &p_chunk) const;
	void _update_transforms();
	void _update_visibility();
	void _update_region_collision(const Vector2i &p_region);
	void _update_region_occluder(const Vector2i &p_region);
	void _update_collision_settings();

	Ref<Image> _make_upload_image(const Vector2i &p_region, int p_kind) const;
	Vector3 _get_origin() const;
	void _mark_changed(const Rect2i &p_vertex_rect);
	void _apply_auto_slope(const Vector3 &p_normal, float *r_weights) const;

	static Callable _navmesh_source_geometry_parsing_callback;
	static RID _navmesh_source_geometry_parser;

protected:
	void _notification(int p_what);
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_data(const Ref<TerrainData> &p_data);
	Ref<TerrainData> get_data() const { return data; }
	void set_terrain_material(const Ref<TerrainMaterial> &p_material);
	Ref<TerrainMaterial> get_terrain_material() const { return terrain_material; }

	void set_vertex_spacing(float p_spacing);
	float get_vertex_spacing() const { return vertex_spacing; }
	void set_chunk_size(int p_size);
	int get_chunk_size() const { return chunk_size; }
	void set_lod_bias(float p_bias);
	float get_lod_bias() const { return lod_bias; }
	void set_cast_shadow(ShadowCasting p_mode);
	ShadowCasting get_cast_shadow() const { return cast_shadow; }
	void set_render_layers(uint32_t p_layers);
	uint32_t get_render_layers() const { return render_layers; }
	void set_occlusion_culling(bool p_enabled);
	bool is_occlusion_culling() const { return occlusion_culling; }

	void set_collision_enabled(bool p_enabled);
	bool is_collision_enabled() const { return collision_enabled; }
	void set_collision_layer(uint32_t p_layer);
	uint32_t get_collision_layer() const { return collision_layer; }
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const { return collision_mask; }
	void set_collision_priority(real_t p_priority);
	real_t get_collision_priority() const { return collision_priority; }

	void set_debug_view(DebugView p_view);
	DebugView get_debug_view() const { return debug_view; }

	// Queries, in global space.
	float get_height(const Vector3 &p_global_position) const;
	Vector3 get_normal(const Vector3 &p_global_position) const;
	PackedFloat32Array get_layer_weights(const Vector3 &p_global_position) const;
	int get_dominant_layer(const Vector3 &p_global_position) const;
	String get_surface_name(const Vector3 &p_global_position) const;
	float get_wetness(const Vector3 &p_global_position) const;
	Dictionary intersect_ray(const Vector3 &p_from, const Vector3 &p_direction, float p_max_distance = 10000.0) const;
	bool intersect_ray_fast(const Vector3 &p_from, const Vector3 &p_direction, float p_max_distance, Vector3 &r_position) const;
	AABB get_terrain_aabb() const;
	// In metres relative to the terrain's origin. False over a hole or where
	// there is no terrain.
	bool sample_surface(const Vector2 &p_local, SurfaceSample &r_sample) const;

	Vector2 global_to_vertex(const Vector3 &p_global_position) const;
	Vector3 vertex_to_global(const Vector2i &p_vertex) const;

	void update_collision();
	RID get_physics_body() const { return static_body; }

	// For the editor. Collision rebuilds are throttled while a brush stroke is
	// under way and forced through when it ends.
	void set_collision_update_immediate(bool p_immediate) { collision_immediate = p_immediate; }
	void set_brush_preview(bool p_visible, const Vector3 &p_position = Vector3(), float p_radius = 1.0, float p_hardness = 0.5, const Color &p_color = Color(1, 1, 1));
	void set_brush_segment(bool p_enabled, const Vector3 &p_start = Vector3());
	// A coarse triangle soup of the surface, for editor picking.
	PackedVector3Array get_picking_faces(int p_max_triangles = 65536) const;

	PackedStringArray get_configuration_warnings() const override;

	static void navmesh_parse_init();
	static void navmesh_parse_source_geometry(const Ref<NavigationMesh> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData3D> p_source_geometry_data, Node *p_node);

	Terrain3D();
	~Terrain3D();
};

VARIANT_ENUM_CAST(Terrain3D::DebugView);
VARIANT_ENUM_CAST(Terrain3D::ShadowCasting);
