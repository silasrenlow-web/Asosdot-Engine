/**************************************************************************/
/*  terrain_data.h                                                        */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#pragma once

#include "terrain_layer.h"

#include "core/io/image.h"
#include "core/io/resource.h"
#include "core/templates/hash_map.h"

// Everything that makes one terrain different from another: its heights, what
// is painted where, its holes, and the list of layers the paint refers to.
//
// The terrain is cut into square regions of region_size vertices a side, held
// sparsely, so a world with land in a few places costs nothing in between.
// Vertex (x, z) belongs to region (floor(x / region_size), floor(z / region_size)).
class TerrainData : public Resource {
	GDCLASS(TerrainData, Resource);

public:
	static constexpr int MAX_LAYERS = 16;
	static constexpr int SPLAT_MAP_COUNT = MAX_LAYERS / 4;

	enum MapType {
		MAP_HEIGHT = 1 << 0,
		MAP_SPLAT = 1 << 1,
		MAP_COLOR = 1 << 2,
		MAP_HOLES = 1 << 3,
		MAP_ALL = MAP_HEIGHT | MAP_SPLAT | MAP_COLOR | MAP_HOLES,
	};

	// The maps other than height are allocated the first time something is
	// painted into them. A null map reads as its default: layer 0 everywhere,
	// untinted and dry, no holes.
	struct Region {
		Ref<Image> height; // FORMAT_RF, metres above the terrain's origin.
		Ref<Image> splat[SPLAT_MAP_COUNT]; // FORMAT_RGBA8, one byte of weight per layer.
		Ref<Image> color; // FORMAT_RGBA8, RGB tint, A wetness.
		Ref<Image> holes; // FORMAT_R8, non-zero is a hole.
	};

private:
	int region_size = 256;
	HashMap<Vector2i, Region> regions;
	Vector<Ref<TerrainLayer>> layers;

	void _layer_changed();
	void _mark_edited();
	void _emit_region_changed(const Vector2i &p_region, uint32_t p_maps);
	void _emit_regions_changed();

	Region *_ensure_region(const Vector2i &p_region);
	static Ref<Image> _make_map(int p_size, Image::Format p_format, const uint8_t *p_fill, int p_fill_size);
	Ref<Image> _ensure_splat(Region &r_region, int p_map);
	Ref<Image> _ensure_color(Region &r_region);
	Ref<Image> _ensure_holes(Region &r_region);

	// Storage.
	void _set_regions(const Array &p_regions);
	Array _get_regions() const;
	void _set_layers(const Array &p_layers);
	Array _get_layers() const;

	// Undo support for the editor. A snapshot is a copy of some of a region's
	// maps, restorable as a unit; restoring one for a region that no longer
	// exists recreates it.
	Dictionary _get_snapshot(const Vector2i &p_region, int p_maps) const;
	void _apply_snapshots(const Array &p_snapshots);
	Array _get_layers_snapshot() const;
	void _apply_layers_snapshot(const Array &p_layers, const Array &p_splat_snapshots);

	void _remap_layers(const Vector<int> &p_old_to_new);

protected:
	static void _bind_methods();
	// Each layer shows in the Inspector as its own slot, so a layer can be
	// opened and edited, or swapped for a saved one, from the data itself.
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;

public:
	_FORCE_INLINE_ static int floor_div(int p_a, int p_b) {
		return p_a >= 0 ? p_a / p_b : -((-p_a + p_b - 1) / p_b);
	}
	_FORCE_INLINE_ Vector2i vertex_to_region(const Vector2i &p_vertex) const {
		return Vector2i(floor_div(p_vertex.x, region_size), floor_div(p_vertex.y, region_size));
	}

	void set_region_size(int p_size);
	int get_region_size() const { return region_size; }

	// Regions.
	int get_region_count() const { return regions.size(); }
	TypedArray<Vector2i> get_region_coords() const;
	Vector<Vector2i> get_region_coord_list() const;
	bool has_region(const Vector2i &p_region) const { return regions.has(p_region); }
	bool add_region(const Vector2i &p_region, float p_height = 0.0);
	void remove_region(const Vector2i &p_region);
	void clear();
	Rect2i get_vertex_bounds() const;
	Vector2 get_height_range() const;

	// Direct access for the renderer and the editor's brushes. Pointers stay
	// valid until the region is removed or its maps are replaced.
	const Region *get_region(const Vector2i &p_region) const { return regions.getptr(p_region); }
	Region *get_region_mut(const Vector2i &p_region) { return regions.getptr(p_region); }

	Ref<Image> get_height_map(const Vector2i &p_region) const;
	void set_height_map(const Vector2i &p_region, const Ref<Image> &p_image);
	Ref<Image> get_splat_map(const Vector2i &p_region, int p_index) const;
	void set_splat_map(const Vector2i &p_region, int p_index, const Ref<Image> &p_image);
	Ref<Image> get_color_map(const Vector2i &p_region) const;
	void set_color_map(const Vector2i &p_region, const Ref<Image> &p_image);
	Ref<Image> get_hole_map(const Vector2i &p_region) const;
	void set_hole_map(const Vector2i &p_region, const Ref<Image> &p_image);

	// Single vertices, for scripts. Vertices are in the terrain's own grid:
	// multiply by Terrain3D.vertex_spacing for metres.
	float get_height(const Vector2i &p_vertex) const;
	void set_height(const Vector2i &p_vertex, float p_height);
	bool is_hole(const Vector2i &p_vertex) const;
	void set_hole(const Vector2i &p_vertex, bool p_hole);
	Color get_color(const Vector2i &p_vertex) const;
	void set_color(const Vector2i &p_vertex, const Color &p_color);
	PackedFloat32Array get_layer_weights(const Vector2i &p_vertex) const;
	void set_layer_weights(const Vector2i &p_vertex, const PackedFloat32Array &p_weights);

	// The height a vertex renders at. A vertex on the far edge of the last
	// region, with no region beyond it, borrows the edge of the region it
	// closes off, which is also what the shader does. False for a hole or for
	// no terrain at all.
	bool get_render_height(const Vector2i &p_vertex, float &r_height) const;
	// Height anywhere, interpolated across the same triangle the mesh and the
	// collision shape draw, so all three agree. Position is in vertices.
	bool get_height_interpolated(const Vector2 &p_vertex_pos, float &r_height) const;
	// Unit normal from central differences, in terrain space with unit
	// horizontal spacing scaled by p_spacing.
	Vector3 get_normal_interpolated(const Vector2 &p_vertex_pos, float p_spacing) const;
	void get_layer_weights_interpolated(const Vector2 &p_vertex_pos, float *r_weights) const;
	Color get_color_interpolated(const Vector2 &p_vertex_pos) const;

	// Windows: a rectangle of vertices copied in or out in one go, row major.
	// Vertices with no region are reported invalid on read and skipped on write.
	void read_heights(const Rect2i &p_rect, float *r_heights, uint8_t *r_valid = nullptr) const;
	void write_heights(const Rect2i &p_rect, const float *p_heights);
	void read_splat(const Rect2i &p_rect, uint8_t *r_weights) const; // MAX_LAYERS bytes per vertex.
	void write_splat(const Rect2i &p_rect, const uint8_t *p_weights);
	void read_colors(const Rect2i &p_rect, Color *r_colors) const;
	void write_colors(const Rect2i &p_rect, const Color *p_colors);
	void read_holes(const Rect2i &p_rect, uint8_t *r_holes) const;
	void write_holes(const Rect2i &p_rect, const uint8_t *p_holes);

	// Layers.
	int get_layer_count() const { return layers.size(); }
	Ref<TerrainLayer> get_layer(int p_index) const;
	void set_layer(int p_index, const Ref<TerrainLayer> &p_layer);
	int add_layer(const Ref<TerrainLayer> &p_layer);
	void remove_layer(int p_index);
	void move_layer(int p_from, int p_to);
	const Vector<Ref<TerrainLayer>> &get_layer_list() const { return layers; }

	// Heightmaps.
	Error import_height_image(const Ref<Image> &p_image, const Vector2i &p_vertex_offset, float p_min_height, float p_max_height, bool p_normalized = true);
	Ref<Image> export_height_image(const Rect2i &p_vertex_rect = Rect2i()) const;

	void notify_region_changed(const Vector2i &p_region, int p_maps);

	// A copy with images of its own. Resource::duplicate() would share the
	// Image objects, so writing to the copy would change the original.
	Ref<TerrainData> duplicate_data() const;
};

VARIANT_ENUM_CAST(TerrainData::MapType);
