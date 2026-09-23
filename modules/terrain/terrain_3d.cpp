/**************************************************************************/
/*  terrain_3d.cpp                                                        */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "terrain_3d.h"

#include "terrain_shaders.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"

#ifndef PHYSICS_3D_DISABLED
#include "servers/physics_3d/physics_server_3d.h"
#endif

#ifndef NAVIGATION_3D_DISABLED
#include "scene/resources/3d/navigation_mesh_source_geometry_data_3d.h"
#include "scene/resources/navigation_mesh.h"
#include "servers/navigation_3d/navigation_server_3d.h"

Callable Terrain3D::_navmesh_source_geometry_parsing_callback;
RID Terrain3D::_navmesh_source_geometry_parser;
#endif

namespace {

// Every terrain draws with the same shader; it is compiled once and dropped
// when the last terrain goes.
RID shared_shader;
int shared_shader_users = 0;

enum UploadKind {
	UPLOAD_HEIGHT,
	UPLOAD_NORMAL,
	UPLOAD_SPLAT0,
	UPLOAD_COLOR = UPLOAD_SPLAT0 + TerrainData::SPLAT_MAP_COUNT,
};

// Geometric error, in metres per unit of vertex spacing, assigned to the first
// reduced level of detail; each level after doubles it. Godot swaps to a level
// once its error projects below a pixel, which with this puts LOD 1 roughly one
// chunk out at 1080p, and lod_bias scales from there.
constexpr float LOD_BASE_ERROR = 0.09;

// Collision rebuilds are throttled to this while a stroke is being painted.
constexpr uint64_t COLLISION_THROTTLE_USEC = 250000;

// One occluder vertex per this many terrain vertices.
constexpr int OCCLUDER_STEP = 8;

} // namespace

/* -------------------------------------------------------------------------- */
/*                                  Lifecycle                                 */
/* -------------------------------------------------------------------------- */

Terrain3D::Terrain3D() {
	set_notify_transform(true);
}

Terrain3D::~Terrain3D() {
	RenderingServer *rs = RenderingServer::get_singleton();
	_free_rendering();
	if (mesh.is_valid()) {
		rs->free_rid(mesh);
	}
	if (material.is_valid()) {
		rs->free_rid(material);
		shared_shader_users--;
		if (shared_shader_users == 0 && shared_shader.is_valid()) {
			rs->free_rid(shared_shader);
			shared_shader = RID();
		}
	}
	_free_collision();
}

Vector3 Terrain3D::_get_origin() const {
	return is_inside_tree() ? get_global_transform().origin : get_transform().origin;
}

void Terrain3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_process_internal(true);
		} break;

		case NOTIFICATION_ENTER_WORLD: {
			layout_dirty = true;
			_process_updates();
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			_free_rendering();
			_free_collision();
			layout_dirty = true;
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			_update_transforms();
			update_gizmos();
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			_update_visibility();
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			_process_updates();
		} break;
	}
}

/* -------------------------------------------------------------------------- */
/*                               Data and changes                             */
/* -------------------------------------------------------------------------- */

void Terrain3D::set_data(const Ref<TerrainData> &p_data) {
	if (data == p_data) {
		return;
	}
	if (data.is_valid()) {
		data->disconnect(SNAME("region_changed"), callable_mp(this, &Terrain3D::_data_region_changed));
		data->disconnect(SNAME("regions_changed"), callable_mp(this, &Terrain3D::_data_regions_changed));
		data->disconnect(SNAME("layers_changed"), callable_mp(this, &Terrain3D::_data_layers_changed));
	}
	data = p_data;
	if (data.is_valid()) {
		data->connect(SNAME("region_changed"), callable_mp(this, &Terrain3D::_data_region_changed));
		data->connect(SNAME("regions_changed"), callable_mp(this, &Terrain3D::_data_regions_changed));
		data->connect(SNAME("layers_changed"), callable_mp(this, &Terrain3D::_data_layers_changed));
	}
	layout_dirty = true;
	layers_dirty = true;
	_queue_update();
	update_configuration_warnings();
}

void Terrain3D::set_terrain_material(const Ref<TerrainMaterial> &p_material) {
	if (terrain_material == p_material) {
		return;
	}
	if (terrain_material.is_valid()) {
		terrain_material->disconnect_changed(callable_mp(this, &Terrain3D::_material_changed));
	}
	terrain_material = p_material;
	if (terrain_material.is_valid()) {
		terrain_material->connect_changed(callable_mp(this, &Terrain3D::_material_changed));
	}
	_material_changed();
}

void Terrain3D::_data_region_changed(const Vector2i &p_region, int p_maps) {
	uint32_t *existing = dirty_regions.getptr(p_region);
	if (existing) {
		*existing |= p_maps;
	} else {
		dirty_regions.insert(p_region, p_maps);
	}
	_queue_update();
}

void Terrain3D::_data_regions_changed() {
	layout_dirty = true;
	_queue_update();
	update_gizmos();
}

void Terrain3D::_data_layers_changed() {
	layers_dirty = true;
	_queue_update();
}

void Terrain3D::_material_changed() {
	material_dirty = true;
	_queue_update();
}

void Terrain3D::_queue_update() {
	// Nothing to do: the internal process picks the flags up on the next frame.
	// The function exists so every change goes through one place.
}

void Terrain3D::_mark_changed(const Rect2i &p_vertex_rect) {
	const Rect2 area(Vector2(p_vertex_rect.position) * vertex_spacing, Vector2(p_vertex_rect.size) * vertex_spacing);
	pending_changed_area = has_pending_changed_area ? pending_changed_area.merge(area) : area;
	has_pending_changed_area = true;
}

void Terrain3D::_process_updates() {
	if (!is_inside_world()) {
		return;
	}

	if (mesh_dirty) {
		_rebuild_mesh();
		layout_dirty = true;
	}

	if (layout_dirty) {
		_rebuild_layout();
		layout_dirty = false;
		dirty_regions.clear();
		dirty_collision.clear();
		layers_dirty = false;
		material_dirty = false;
	}

	if (layers_dirty) {
		const int wanted = data.is_valid() ? MAX(1, (data->get_layer_count() + 3) / 4) : 1;
		if (wanted != splat_array_count) {
			// More layers than the splat arrays hold: rebuild them all.
			_rebuild_layout();
			dirty_regions.clear();
			dirty_collision.clear();
		} else {
			_rebuild_layer_textures();
		}
		layers_dirty = false;
		material_dirty = true;
	}

	if (!dirty_regions.is_empty() && data.is_valid()) {
		const int rs = data->get_region_size();
		HashSet<Vector2i> geometry_regions;
		for (const KeyValue<Vector2i, uint32_t> &E : dirty_regions) {
			if (!region_slices.has(E.key)) {
				continue;
			}
			if (E.value & (TerrainData::MAP_HEIGHT | TerrainData::MAP_HOLES)) {
				geometry_regions.insert(E.key);
			}
			_update_region_textures(E.key, E.value);
			_mark_changed(Rect2i(E.key * rs, Size2i(rs, rs)));
		}
		for (const Vector2i &region : geometry_regions) {
			// A region's edge normals, chunk bounds and collision all reach one
			// vertex into the regions before it.
			const Rect2i rect(region * rs, Size2i(rs, rs));
			_update_normals(rect.grow(1));
			_update_chunks_in(rect.grow(1));
			for (int dz = -1; dz <= 0; dz++) {
				for (int dx = -1; dx <= 0; dx++) {
					dirty_collision.insert(region + Vector2i(dx, dz));
				}
			}
		}
		dirty_regions.clear();
	}

	if (!dirty_collision.is_empty()) {
		const uint64_t now = OS::get_singleton()->get_ticks_usec();
		if (collision_immediate || now - last_collision_update_usec >= COLLISION_THROTTLE_USEC) {
			for (const Vector2i &region : dirty_collision) {
				if (region_slices.has(region)) {
					_update_region_collision(region);
					_update_region_occluder(region);
				}
			}
			dirty_collision.clear();
			last_collision_update_usec = now;
			// The editor's picking surface follows the collision, on the same
			// throttle, rather than being rebuilt for every dab.
			update_gizmos();
		}
	}

	if (material_dirty) {
		_update_material_params();
		material_dirty = false;
	}

	if (has_pending_changed_area) {
		has_pending_changed_area = false;
		emit_signal(SNAME("terrain_changed"), pending_changed_area);
	}
}

/* -------------------------------------------------------------------------- */
/*                                  Rendering                                 */
/* -------------------------------------------------------------------------- */

void Terrain3D::_free_rendering() {
	RenderingServer *rs = RenderingServer::get_singleton();
	for (KeyValue<Vector2i, Chunk> &E : chunks) {
		if (E.value.instance.is_valid()) {
			rs->free_rid(E.value.instance);
		}
	}
	chunks.clear();
	for (KeyValue<Vector2i, RID> &E : occluder_instances) {
		rs->free_rid(E.value);
	}
	occluder_instances.clear();
	for (KeyValue<Vector2i, RID> &E : occluders) {
		rs->free_rid(E.value);
	}
	occluders.clear();

	RID *textures[] = { &height_array, &normal_array, &color_array, &region_map_texture, &layer_albedo_array, &layer_normal_array };
	for (RID *t : textures) {
		if (t->is_valid()) {
			rs->free_rid(*t);
			*t = RID();
		}
	}
	for (int i = 0; i < TerrainData::SPLAT_MAP_COUNT; i++) {
		if (splat_arrays[i].is_valid()) {
			rs->free_rid(splat_arrays[i]);
			splat_arrays[i] = RID();
		}
	}
	splat_array_count = 0;
	region_slices.clear();
	slice_regions.clear();
	normal_maps.clear();
}

void Terrain3D::_rebuild_mesh() {
	RenderingServer *rs = RenderingServer::get_singleton();
	mesh_dirty = false;

	if (material.is_null()) {
		if (shared_shader.is_null()) {
			shared_shader = rs->shader_create();
			rs->shader_set_code(shared_shader, TERRAIN_SHADER_CODE);
		}
		shared_shader_users++;
		material = rs->material_create();
		rs->material_set_shader(material, shared_shader);
		material_dirty = true;
	}

	if (mesh.is_null()) {
		mesh = rs->mesh_create();
	} else {
		rs->mesh_clear(mesh);
	}

	const int cs = chunk_size;
	const int grid = cs + 1;
	const float sp = vertex_spacing;

	// A grid of (cs + 1)^2 vertices, then one skirt vertex hanging under each
	// edge vertex. The shader tells skirts apart by their y of -1.
	PackedVector3Array vertices;
	vertices.resize(grid * grid + 4 * cs);
	Vector<int> skirt_of;
	skirt_of.resize(grid * grid);
	skirt_of.fill(-1);
	{
		Vector3 *w = vertices.ptrw();
		for (int z = 0; z <= cs; z++) {
			for (int x = 0; x <= cs; x++) {
				w[z * grid + x] = Vector3(x * sp, 0.0, z * sp);
			}
		}
		int next = grid * grid;
		for (int z = 0; z <= cs; z++) {
			for (int x = 0; x <= cs; x++) {
				if (x == 0 || z == 0 || x == cs || z == cs) {
					skirt_of.write[z * grid + x] = next;
					w[next++] = Vector3(x * sp, -1.0, z * sp);
				}
			}
		}
	}

	const auto build_indices = [&](int p_step) {
		PackedInt32Array indices;
		const auto at = [&](int x, int z) { return z * grid + x; };
		// Clockwise seen from above, which is Godot's front face, split along
		// the same diagonal as HeightMapShape3D.
		for (int z = 0; z < cs; z += p_step) {
			for (int x = 0; x < cs; x += p_step) {
				const int a = at(x, z);
				const int b = at(x + p_step, z);
				const int c = at(x, z + p_step);
				const int d = at(x + p_step, z + p_step);
				indices.push_back(a);
				indices.push_back(b);
				indices.push_back(c);
				indices.push_back(b);
				indices.push_back(d);
				indices.push_back(c);
			}
		}
		const auto add_skirt = [&](int t0, int t1, const Vector3 &p_outward) {
			const int b0 = skirt_of[t0];
			const int b1 = skirt_of[t1];
			const Vector3 n = (vertices[t1] - vertices[t0]).cross(vertices[b0] - vertices[t0]);
			if (n.dot(p_outward) > 0.0) {
				indices.push_back(t0);
				indices.push_back(b0);
				indices.push_back(t1);
				indices.push_back(t1);
				indices.push_back(b0);
				indices.push_back(b1);
			} else {
				indices.push_back(t0);
				indices.push_back(t1);
				indices.push_back(b0);
				indices.push_back(t1);
				indices.push_back(b1);
				indices.push_back(b0);
			}
		};
		for (int i = 0; i < cs; i += p_step) {
			add_skirt(at(i, 0), at(i + p_step, 0), Vector3(0, 0, -1));
			add_skirt(at(i, cs), at(i + p_step, cs), Vector3(0, 0, 1));
			add_skirt(at(0, i), at(0, i + p_step), Vector3(-1, 0, 0));
			add_skirt(at(cs, i), at(cs, i + p_step), Vector3(1, 0, 0));
		}
		return indices;
	};

	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = vertices;
	arrays[RSE::ARRAY_INDEX] = build_indices(1);

	Dictionary lods;
	for (int lod = 1; lod < LOD_COUNT && (1 << lod) <= cs / 4; lod++) {
		const float error = sp * LOD_BASE_ERROR * float(1 << (lod - 1));
		lods[error] = build_indices(1 << lod);
	}

	rs->mesh_add_surface_from_arrays(mesh, RSE::PRIMITIVE_TRIANGLES, arrays, Array(), lods);
	rs->mesh_surface_set_material(mesh, 0, material);
}

static Ref<Image> make_layer_array_image(const Ref<Texture2D> &p_texture) {
	if (p_texture.is_null()) {
		return Ref<Image>();
	}
	Ref<Image> image = p_texture->get_image();
	if (image.is_null() || image->is_empty()) {
		return Ref<Image>();
	}
	return image;
}

// A texture array needs every slice the same size, format and mip count. When
// the layers' textures already agree they go in untouched, compression and
// all; otherwise everything is brought to uncompressed RGBA8 at the largest
// size, which costs video memory but always works.
static Vector<Ref<Image>> normalize_slices(const Vector<Ref<Image>> &p_images, bool &r_converted) {
	r_converted = false;
	if (p_images.is_empty()) {
		return p_images;
	}
	const Ref<Image> &first = p_images[0];
	bool uniform = first->has_mipmaps();
	int max_w = 0;
	int max_h = 0;
	for (const Ref<Image> &image : p_images) {
		uniform = uniform && image->get_width() == first->get_width() && image->get_height() == first->get_height() && image->get_format() == first->get_format() && image->has_mipmaps() == first->has_mipmaps();
		max_w = MAX(max_w, image->get_width());
		max_h = MAX(max_h, image->get_height());
	}
	if (uniform) {
		return p_images;
	}
	r_converted = true;
	max_w = MIN((int)Math::next_power_of_2((uint32_t)max_w), 4096);
	max_h = MIN((int)Math::next_power_of_2((uint32_t)max_h), 4096);
	Vector<Ref<Image>> result;
	for (const Ref<Image> &source : p_images) {
		Ref<Image> image = source->duplicate();
		if (image->is_compressed()) {
			image->decompress();
		}
		image->clear_mipmaps();
		image->convert(Image::FORMAT_RGBA8);
		if (image->get_width() != max_w || image->get_height() != max_h) {
			image->resize(max_w, max_h, Image::INTERPOLATE_BILINEAR);
		}
		image->generate_mipmaps();
		result.push_back(image);
	}
	return result;
}

void Terrain3D::_rebuild_layer_textures() {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (layer_albedo_array.is_valid()) {
		rs->free_rid(layer_albedo_array);
		layer_albedo_array = RID();
	}
	if (layer_normal_array.is_valid()) {
		rs->free_rid(layer_normal_array);
		layer_normal_array = RID();
	}
	if (data.is_null()) {
		return;
	}

	Vector<Ref<Image>> albedo;
	Vector<Ref<Image>> normal;
	for (const Ref<TerrainLayer> &layer : data->get_layer_list()) {
		if (layer.is_null()) {
			continue;
		}
		const Ref<Image> a = make_layer_array_image(layer->get_albedo_texture());
		if (a.is_valid()) {
			albedo.push_back(a);
		}
		const Ref<Image> n = make_layer_array_image(layer->get_normal_texture());
		if (n.is_valid()) {
			normal.push_back(n);
		}
	}

	bool converted_a = false;
	bool converted_n = false;
	albedo = normalize_slices(albedo, converted_a);
	normal = normalize_slices(normal, converted_n);
	const bool converted = converted_a || converted_n;
	if (converted != (bool)get_meta(SNAME("_layer_textures_converted"), false)) {
		set_meta(SNAME("_layer_textures_converted"), converted ? Variant(true) : Variant());
		update_configuration_warnings();
	}

	if (!albedo.is_empty()) {
		layer_albedo_array = rs->texture_2d_layered_create(albedo, RSE::TEXTURE_LAYERED_2D_ARRAY);
	}
	if (!normal.is_empty()) {
		layer_normal_array = rs->texture_2d_layered_create(normal, RSE::TEXTURE_LAYERED_2D_ARRAY);
	}
	material_dirty = true;
}

Ref<Image> Terrain3D::_make_upload_image(const Vector2i &p_region, int p_kind) const {
	const TerrainData::Region *region = data->get_region(p_region);
	const int rs = data->get_region_size();
	ERR_FAIL_NULL_V(region, Ref<Image>());

	if (p_kind == UPLOAD_HEIGHT) {
		// Holes travel to the GPU as NaN heights, which the vertex shader turns
		// into dropped triangles. The data keeps the real height underneath so
		// filling a hole back in restores the ground it had.
		if (region->holes.is_null()) {
			return region->height;
		}
		Vector<uint8_t> bytes = region->height->get_data();
		float *h = (float *)bytes.ptrw();
		const uint8_t *holes = region->holes->ptr();
		for (int i = 0; i < rs * rs; i++) {
			if (holes[i]) {
				h[i] = Math::NaN;
			}
		}
		return Image::create_from_data(rs, rs, false, Image::FORMAT_RF, bytes);
	}
	if (p_kind == UPLOAD_NORMAL) {
		const Ref<Image> *normals = normal_maps.getptr(p_region);
		return normals ? *normals : Ref<Image>();
	}
	if (p_kind >= UPLOAD_SPLAT0 && p_kind < UPLOAD_COLOR) {
		const int m = p_kind - UPLOAD_SPLAT0;
		if (region->splat[m].is_valid()) {
			return region->splat[m];
		}
		Ref<Image> fill = Image::create_empty(rs, rs, false, Image::FORMAT_RGBA8);
		fill->fill(m == 0 ? Color(1, 0, 0, 0) : Color(0, 0, 0, 0));
		return fill;
	}
	if (p_kind == UPLOAD_COLOR) {
		if (region->color.is_valid()) {
			return region->color;
		}
		Ref<Image> fill = Image::create_empty(rs, rs, false, Image::FORMAT_RGBA8);
		fill->fill(Color(1, 1, 1, 0));
		return fill;
	}
	return Ref<Image>();
}

void Terrain3D::_update_normals(const Rect2i &p_vertex_rect) {
	if (data.is_null()) {
		return;
	}
	const int rs = data->get_region_size();
	const Rect2i rect = p_vertex_rect;
	const Rect2i read_rect = rect.grow(1);
	Vector<float> heights;
	Vector<uint8_t> valid;
	heights.resize(read_rect.size.x * read_rect.size.y);
	valid.resize(read_rect.size.x * read_rect.size.y);
	data->read_heights(read_rect, heights.ptrw(), valid.ptrw());
	const float *h = heights.ptr();
	const uint8_t *ok = valid.ptr();
	const int rw = read_rect.size.x;
	const float two_sp = 2.0f * vertex_spacing;

	const Vector2i r0 = data->vertex_to_region(rect.position);
	const Vector2i r1 = data->vertex_to_region(rect.get_end() - Vector2i(1, 1));
	for (int rz = r0.y; rz <= r1.y; rz++) {
		for (int rx = r0.x; rx <= r1.x; rx++) {
			const Vector2i coord(rx, rz);
			if (!data->has_region(coord)) {
				continue;
			}
			Ref<Image> *map = normal_maps.getptr(coord);
			if (!map) {
				Ref<Image> created = Image::create_empty(rs, rs, false, Image::FORMAT_RGH);
				map = &normal_maps.insert(coord, created)->value;
			}
			uint16_t *dst = (uint16_t *)(*map)->ptrw();
			const Rect2i isect = Rect2i(coord * rs, Size2i(rs, rs)).intersection(rect);
			for (int z = isect.position.y; z < isect.get_end().y; z++) {
				for (int x = isect.position.x; x < isect.get_end().x; x++) {
					const int i = (z - read_rect.position.y) * rw + (x - read_rect.position.x);
					const float c = h[i];
					const float l = ok[i - 1] ? h[i - 1] : c;
					const float r = ok[i + 1] ? h[i + 1] : c;
					const float d = ok[i - rw] ? h[i - rw] : c;
					const float u = ok[i + rw] ? h[i + rw] : c;
					const Vector3 n = Vector3(l - r, two_sp, d - u).normalized();
					uint16_t *px = dst + ((z - coord.y * rs) * rs + (x - coord.x * rs)) * 2;
					px[0] = Math::make_half_float(n.x);
					px[1] = Math::make_half_float(n.z);
				}
			}
			if (normal_array.is_valid()) {
				const int *slice = region_slices.getptr(coord);
				if (slice) {
					RenderingServer::get_singleton()->texture_2d_update(normal_array, *map, *slice);
				}
			}
		}
	}
}

void Terrain3D::_update_region_textures(const Vector2i &p_region, uint32_t p_maps) {
	const int *slice = region_slices.getptr(p_region);
	if (!slice) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if ((p_maps & (TerrainData::MAP_HEIGHT | TerrainData::MAP_HOLES)) && height_array.is_valid()) {
		rs->texture_2d_update(height_array, _make_upload_image(p_region, UPLOAD_HEIGHT), *slice);
	}
	if (p_maps & TerrainData::MAP_SPLAT) {
		for (int m = 0; m < splat_array_count; m++) {
			rs->texture_2d_update(splat_arrays[m], _make_upload_image(p_region, UPLOAD_SPLAT0 + m), *slice);
		}
	}
	if ((p_maps & TerrainData::MAP_COLOR) && color_array.is_valid()) {
		rs->texture_2d_update(color_array, _make_upload_image(p_region, UPLOAD_COLOR), *slice);
	}
}

void Terrain3D::_update_instance_settings(const Chunk &p_chunk) const {
	RenderingServer *rs = RenderingServer::get_singleton();
	static const RSE::ShadowCastingSetting shadow_modes[] = {
		RSE::SHADOW_CASTING_SETTING_OFF,
		RSE::SHADOW_CASTING_SETTING_ON,
		RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED,
		RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY,
	};
	rs->instance_geometry_set_cast_shadows_setting(p_chunk.instance, shadow_modes[cast_shadow]);
	rs->instance_set_layer_mask(p_chunk.instance, render_layers);
	rs->instance_geometry_set_lod_bias(p_chunk.instance, lod_bias * p_chunk.lod_scale);
	rs->instance_set_visible(p_chunk.instance, is_visible_in_tree());
}

void Terrain3D::_update_chunk(const Vector2i &p_chunk) {
	Chunk *chunk = chunks.getptr(p_chunk);
	if (!chunk) {
		return;
	}
	// Bounds over the chunk's (cs + 1)^2 vertices, including the far edge it
	// shares with its neighbour.
	const int cs = chunk_size;
	const Rect2i rect(p_chunk * cs, Size2i(cs + 1, cs + 1));
	Vector<float> heights;
	Vector<uint8_t> valid;
	heights.resize(rect.size.x * rect.size.y);
	valid.resize(rect.size.x * rect.size.y);
	data->read_heights(rect, heights.ptrw(), valid.ptrw());
	float lo = Math::INF;
	float hi = -Math::INF;
	for (int i = 0; i < heights.size(); i++) {
		if (valid[i]) {
			lo = MIN(lo, heights[i]);
			hi = MAX(hi, heights[i]);
		}
	}
	if (lo > hi) {
		lo = hi = 0.0;
	}
	chunk->min_height = lo;
	chunk->max_height = hi;

	// The shared mesh gives each level of detail one fixed error, but real
	// error depends on the ground: nothing on a flat field, metres on a rocky
	// slope. Measure how far each level's triangles stray from the full
	// surface here, and scale this chunk's bias so a level is only chosen
	// once its real error is below a pixel. Flat chunks then simplify early
	// and rough ones hold their shape, and the seam between two levels stays
	// too small to see.
	const int w = cs + 1;
	const float *h = heights.ptr();
	const uint8_t *ok = valid.ptr();
	float worst_ratio = 0.0;
	for (int lod = 1; lod < LOD_COUNT && (1 << lod) <= cs / 4; lod++) {
		const int step = 1 << lod;
		float error = 0.0;
		for (int qz = 0; qz < cs; qz += step) {
			for (int qx = 0; qx < cs; qx += step) {
				const int i00 = qz * w + qx;
				const int i10 = i00 + step;
				const int i01 = i00 + step * w;
				const int i11 = i01 + step;
				if (!ok[i00] || !ok[i10] || !ok[i01] || !ok[i11]) {
					continue;
				}
				for (int z = 0; z <= step; z++) {
					for (int x = 0; x <= step; x++) {
						const int i = i00 + z * w + x;
						if (!ok[i]) {
							continue;
						}
						const float fx = float(x) / step;
						const float fz = float(z) / step;
						const float coarse = fx + fz <= 1.0f
								? h[i00] + fx * (h[i10] - h[i00]) + fz * (h[i01] - h[i00])
								: h[i11] + (1.0f - fx) * (h[i01] - h[i11]) + (1.0f - fz) * (h[i10] - h[i11]);
						error = MAX(error, Math::abs(h[i] - coarse));
					}
				}
			}
		}
		const float assumed = vertex_spacing * LOD_BASE_ERROR * float(1 << (lod - 1));
		worst_ratio = MAX(worst_ratio, error / assumed);
	}
	chunk->lod_scale = CLAMP(worst_ratio, 0.05f, 10000.0f);
	RenderingServer::get_singleton()->instance_geometry_set_lod_bias(chunk->instance, lod_bias * chunk->lod_scale);

	// Any crack between two levels of detail is bounded by the chunk's own
	// height range, so a skirt that deep always covers it.
	const float skirt = (hi - lo) * 0.5f + vertex_spacing;
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->instance_geometry_set_shader_parameter(chunk->instance, SNAME("skirt_depth"), skirt);
	const float size = cs * vertex_spacing;
	rs->instance_set_custom_aabb(chunk->instance, AABB(Vector3(0, lo - skirt, 0), Vector3(size, hi - lo + skirt, size)));
}

void Terrain3D::_update_chunks_in(const Rect2i &p_vertex_rect) {
	const int cs = chunk_size;
	const Vector2i c0(TerrainData::floor_div(p_vertex_rect.position.x, cs), TerrainData::floor_div(p_vertex_rect.position.y, cs));
	const Vector2i c1(TerrainData::floor_div(p_vertex_rect.get_end().x - 1, cs), TerrainData::floor_div(p_vertex_rect.get_end().y - 1, cs));
	for (int cz = c0.y; cz <= c1.y; cz++) {
		for (int cx = c0.x; cx <= c1.x; cx++) {
			_update_chunk(Vector2i(cx, cz));
		}
	}
}

void Terrain3D::_rebuild_layout() {
	RenderingServer *rs = RenderingServer::get_singleton();
	_free_rendering();
	if (!is_inside_world() || data.is_null() || data->get_region_count() == 0) {
		_free_collision();
		emit_signal(SNAME("terrain_changed"), Rect2());
		update_gizmos();
		return;
	}
	if (mesh.is_null()) {
		_rebuild_mesh();
	}

	const int rs_size = data->get_region_size();
	if (chunk_size > rs_size) {
		// Both are powers of two, so clamping is all it takes for chunks to
		// tile regions exactly.
		chunk_size = rs_size;
		_rebuild_mesh();
	}

	// Slices in a fixed order, so a reload lays the arrays out the same way.
	Vector<Vector2i> coords = data->get_region_coord_list();
	coords.sort();
	Rect2i bounds;
	for (int i = 0; i < coords.size(); i++) {
		region_slices.insert(coords[i], i);
		slice_regions.push_back(coords[i]);
		bounds = i == 0 ? Rect2i(coords[i], Size2i(1, 1)) : bounds.merge(Rect2i(coords[i], Size2i(1, 1)));
	}
	region_map_rect = bounds;

	{
		Vector<uint8_t> bytes;
		bytes.resize(bounds.size.x * bounds.size.y * sizeof(float));
		float *px = (float *)bytes.ptrw();
		memset(px, 0, bytes.size());
		for (int i = 0; i < coords.size(); i++) {
			const Vector2i m = coords[i] - bounds.position;
			px[m.y * bounds.size.x + m.x] = float(i + 1);
		}
		const Ref<Image> map = Image::create_from_data(bounds.size.x, bounds.size.y, false, Image::FORMAT_RF, bytes);
		region_map_texture = rs->texture_2d_create(map);
	}

	for (const Vector2i &coord : coords) {
		_update_normals(Rect2i(coord * rs_size, Size2i(rs_size, rs_size)));
	}

	Vector<Ref<Image>> heights;
	Vector<Ref<Image>> normals;
	Vector<Ref<Image>> colors;
	for (const Vector2i &coord : coords) {
		heights.push_back(_make_upload_image(coord, UPLOAD_HEIGHT));
		normals.push_back(_make_upload_image(coord, UPLOAD_NORMAL));
		colors.push_back(_make_upload_image(coord, UPLOAD_COLOR));
	}
	height_array = rs->texture_2d_layered_create(heights, RSE::TEXTURE_LAYERED_2D_ARRAY);
	normal_array = rs->texture_2d_layered_create(normals, RSE::TEXTURE_LAYERED_2D_ARRAY);
	color_array = rs->texture_2d_layered_create(colors, RSE::TEXTURE_LAYERED_2D_ARRAY);

	splat_array_count = MAX(1, (data->get_layer_count() + 3) / 4);
	for (int m = 0; m < splat_array_count; m++) {
		Vector<Ref<Image>> splat;
		for (const Vector2i &coord : coords) {
			splat.push_back(_make_upload_image(coord, UPLOAD_SPLAT0 + m));
		}
		splat_arrays[m] = rs->texture_2d_layered_create(splat, RSE::TEXTURE_LAYERED_2D_ARRAY);
	}

	_rebuild_layer_textures();

	const RID scenario = get_world_3d()->get_scenario();
	const int per_region = rs_size / chunk_size;
	const Vector3 origin = _get_origin();
	for (const Vector2i &coord : coords) {
		for (int z = 0; z < per_region; z++) {
			for (int x = 0; x < per_region; x++) {
				const Vector2i chunk_coord = coord * per_region + Vector2i(x, z);
				Chunk chunk;
				chunk.instance = rs->instance_create2(mesh, scenario);
				rs->instance_attach_object_instance_id(chunk.instance, get_instance_id());
				rs->instance_geometry_set_flag(chunk.instance, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, true);
				rs->instance_set_transform(chunk.instance, Transform3D(Basis(), origin + Vector3(chunk_coord.x, 0, chunk_coord.y) * (chunk_size * vertex_spacing)));
				_update_instance_settings(chunk);
				chunks.insert(chunk_coord, chunk);
				_update_chunk(chunk_coord);
			}
		}
	}

	_update_collision_settings();
	for (const Vector2i &coord : coords) {
		_update_region_collision(coord);
		_update_region_occluder(coord);
	}
	last_collision_update_usec = OS::get_singleton()->get_ticks_usec();

	_update_material_params();
	_mark_changed(data->get_vertex_bounds());
	update_gizmos();
}

void Terrain3D::_update_material_params() {
	if (material.is_null()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	const Vector3 origin = _get_origin();

	rs->material_set_param(material, SNAME("heightmaps"), height_array);
	rs->material_set_param(material, SNAME("normalmaps"), normal_array);
	rs->material_set_param(material, SNAME("colormaps"), color_array);
	for (int m = 0; m < TerrainData::SPLAT_MAP_COUNT; m++) {
		rs->material_set_param(material, StringName(vformat("splatmaps%d", m)), m < splat_array_count ? splat_arrays[m] : RID());
	}
	rs->material_set_param(material, SNAME("region_map"), region_map_texture);
	rs->material_set_param(material, SNAME("region_map_origin"), region_map_rect.position);
	rs->material_set_param(material, SNAME("region_map_size"), region_map_rect.size.max(Size2i(1, 1)));
	rs->material_set_param(material, SNAME("region_size"), data.is_valid() ? data->get_region_size() : 256);
	rs->material_set_param(material, SNAME("chunk_size"), chunk_size);
	rs->material_set_param(material, SNAME("vertex_spacing"), vertex_spacing);
	rs->material_set_param(material, SNAME("terrain_origin"), origin);
	rs->material_set_param(material, SNAME("splat_map_count"), splat_array_count);

	const int layer_count = data.is_valid() ? MIN(data->get_layer_count(), TerrainData::MAX_LAYERS) : 0;
	rs->material_set_param(material, SNAME("layer_count"), layer_count);
	rs->material_set_param(material, SNAME("layer_albedo"), layer_albedo_array);
	rs->material_set_param(material, SNAME("layer_normal"), layer_normal_array);

	PackedFloat32Array tint;
	PackedFloat32Array params;
	PackedFloat32Array textures;
	tint.resize(TerrainData::MAX_LAYERS * 4);
	params.resize(TerrainData::MAX_LAYERS * 4);
	textures.resize(TerrainData::MAX_LAYERS * 4);
	tint.fill(1.0);
	params.fill(0.0);
	textures.fill(-1.0);
	int albedo_slice = 0;
	int normal_slice = 0;
	for (int i = 0; i < layer_count; i++) {
		const Ref<TerrainLayer> layer = data->get_layer(i);
		if (layer.is_null()) {
			continue;
		}
		// Linear, because the shader multiplies the sampled albedo with it.
		const Color c = layer->get_albedo_color().srgb_to_linear();
		tint.set(i * 4 + 0, c.r);
		tint.set(i * 4 + 1, c.g);
		tint.set(i * 4 + 2, c.b);
		tint.set(i * 4 + 3, layer->get_roughness());
		params.set(i * 4 + 0, 1.0 / layer->get_tile_size());
		params.set(i * 4 + 1, layer->get_normal_strength());
		params.set(i * 4 + 2, layer->is_triplanar() ? 1.0 : 0.0);
		params.set(i * 4 + 3, layer->get_detiling());
		// Slices are handed out in layer order, skipping layers whose texture
		// failed to load, exactly as _rebuild_layer_textures() packed them.
		if (make_layer_array_image(layer->get_albedo_texture()).is_valid()) {
			textures.set(i * 4 + 0, albedo_slice++);
		}
		if (make_layer_array_image(layer->get_normal_texture()).is_valid()) {
			textures.set(i * 4 + 1, normal_slice++);
		}
	}
	rs->material_set_param(material, SNAME("layer_tint"), tint);
	rs->material_set_param(material, SNAME("layer_params"), params);
	rs->material_set_param(material, SNAME("layer_textures"), textures);

	Ref<TerrainMaterial> tm = terrain_material;
	if (tm.is_null()) {
		tm.instantiate();
	}
	rs->material_set_param(material, SNAME("height_blending"), tm->is_height_blending());
	rs->material_set_param(material, SNAME("height_blend_sharpness"), tm->get_height_blend_sharpness());
	rs->material_set_param(material, SNAME("auto_slope"), tm->is_auto_slope());
	rs->material_set_param(material, SNAME("auto_slope_layer"), tm->get_auto_slope_layer());
	const float lo = Math::deg_to_rad(MAX(tm->get_auto_slope_angle() - tm->get_auto_slope_blend() * 0.5f, 0.0f));
	const float hi = Math::deg_to_rad(MIN(tm->get_auto_slope_angle() + tm->get_auto_slope_blend() * 0.5f, 90.0f));
	rs->material_set_param(material, SNAME("auto_slope_range"), Vector2(1.0 - Math::cos(lo), MAX(1.0 - Math::cos(hi), 1.0 - Math::cos(lo) + 0.001)));
	rs->material_set_param(material, SNAME("macro_variation"), tm->get_macro_variation());
	rs->material_set_param(material, SNAME("macro_variation_size"), tm->get_macro_variation_size());
	rs->material_set_param(material, SNAME("wetness_darkening"), tm->get_wetness_darkening());

	rs->material_set_param(material, SNAME("debug_view"), (int)debug_view);
	if (debug_view == DEBUG_VIEW_HEIGHT && data.is_valid()) {
		rs->material_set_param(material, SNAME("debug_height_range"), data->get_height_range());
	}

	rs->material_set_param(material, SNAME("brush_enabled"), brush_visible);
	rs->material_set_param(material, SNAME("brush_position"), brush_position);
	rs->material_set_param(material, SNAME("brush_radius"), brush_radius);
	rs->material_set_param(material, SNAME("brush_hardness"), brush_hardness);
	rs->material_set_param(material, SNAME("brush_color"), brush_color);
	rs->material_set_param(material, SNAME("brush_segment"), brush_segment);
	rs->material_set_param(material, SNAME("brush_segment_start"), brush_segment_start);
}

void Terrain3D::_update_transforms() {
	RenderingServer *rs = RenderingServer::get_singleton();
	const Vector3 origin = _get_origin();
	const float chunk_metres = chunk_size * vertex_spacing;
	for (const KeyValue<Vector2i, Chunk> &E : chunks) {
		rs->instance_set_transform(E.value.instance, Transform3D(Basis(), origin + Vector3(E.key.x, 0, E.key.y) * chunk_metres));
	}
	for (const KeyValue<Vector2i, RID> &E : occluder_instances) {
		rs->instance_set_transform(E.value, Transform3D(Basis(), origin));
	}
#ifndef PHYSICS_3D_DISABLED
	if (static_body.is_valid()) {
		PhysicsServer3D::get_singleton()->body_set_state(static_body, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D(Basis(), origin));
	}
#endif
	material_dirty = true;
	update_configuration_warnings();
}

void Terrain3D::_update_visibility() {
	RenderingServer *rs = RenderingServer::get_singleton();
	const bool visible = is_visible_in_tree();
	for (const KeyValue<Vector2i, Chunk> &E : chunks) {
		rs->instance_set_visible(E.value.instance, visible);
	}
	for (const KeyValue<Vector2i, RID> &E : occluder_instances) {
		rs->instance_set_visible(E.value, visible && occlusion_culling);
	}
}

/* -------------------------------------------------------------------------- */
/*                            Collision and occlusion                         */
/* -------------------------------------------------------------------------- */

void Terrain3D::_free_collision() {
#ifndef PHYSICS_3D_DISABLED
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (static_body.is_valid()) {
		ps->free_rid(static_body);
		static_body = RID();
	}
	for (KeyValue<Vector2i, RID> &E : collision_shapes) {
		ps->free_rid(E.value);
	}
	collision_shapes.clear();
#endif
}

void Terrain3D::_update_collision_settings() {
#ifndef PHYSICS_3D_DISABLED
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!collision_enabled || !is_inside_world() || data.is_null()) {
		_free_collision();
		return;
	}
	if (static_body.is_null()) {
		static_body = ps->body_create();
		ps->body_set_mode(static_body, PhysicsServer3D::BODY_MODE_STATIC);
		ps->body_attach_object_instance_id(static_body, get_instance_id());
		ps->body_set_space(static_body, get_world_3d()->get_space());
		ps->body_set_state(static_body, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D(Basis(), _get_origin()));
	}
	ps->body_set_collision_layer(static_body, collision_layer);
	ps->body_set_collision_mask(static_body, collision_mask);
	ps->body_set_collision_priority(static_body, collision_priority);

	// Shapes are keyed by region, but a body indexes them by position, so any
	// change to which regions exist rebuilds the list.
	ps->body_clear_shapes(static_body);
	for (KeyValue<Vector2i, RID> &E : collision_shapes) {
		ps->free_rid(E.value);
	}
	collision_shapes.clear();
	const int rs = data->get_region_size();
	for (const Vector2i &coord : slice_regions) {
		const RID shape = ps->heightmap_shape_create();
		collision_shapes.insert(coord, shape);
		Dictionary d;
		d["width"] = 2;
		d["depth"] = 2;
		PackedFloat32Array flat;
		flat.resize(4);
		flat.fill(0.0);
		d["heights"] = flat;
		d["min_height"] = 0.0;
		d["max_height"] = 0.0;
		ps->shape_set_data(shape, d);
		const Vector3 centre = Vector3(coord.x * rs + rs * 0.5f, 0, coord.y * rs + rs * 0.5f) * vertex_spacing;
		ps->body_add_shape(static_body, shape, Transform3D(Basis().scaled(Vector3(vertex_spacing, 1.0, vertex_spacing)), centre));
	}
#endif
}

void Terrain3D::_update_region_collision(const Vector2i &p_region) {
#ifndef PHYSICS_3D_DISABLED
	const RID *shape = collision_shapes.getptr(p_region);
	if (!shape || data.is_null()) {
		return;
	}
	const int rs = data->get_region_size();
	const int w = rs + 1;
	PackedFloat32Array heights;
	heights.resize(w * w);
	float *h = heights.ptrw();
	float lo = Math::INF;
	float hi = -Math::INF;
	const Vector2i base = p_region * rs;
	for (int z = 0; z < w; z++) {
		for (int x = 0; x < w; x++) {
			float value;
			if (data->get_render_height(base + Vector2i(x, z), value)) {
				lo = MIN(lo, value);
				hi = MAX(hi, value);
			} else {
				// Both physics engines read NaN as a hole.
				value = Math::NaN;
			}
			h[z * w + x] = value;
		}
	}
	if (lo > hi) {
		lo = hi = 0.0;
	}
	Dictionary d;
	d["width"] = w;
	d["depth"] = w;
	d["heights"] = heights;
	d["min_height"] = lo;
	d["max_height"] = hi;
	PhysicsServer3D::get_singleton()->shape_set_data(*shape, d);
#endif
}

void Terrain3D::_update_region_occluder(const Vector2i &p_region) {
	RenderingServer *rs = RenderingServer::get_singleton();
	RID *occluder = occluders.getptr(p_region);
	if (!occlusion_culling || data.is_null() || !is_inside_world()) {
		if (occluder) {
			rs->free_rid(occluder_instances[p_region]);
			rs->free_rid(*occluder);
			occluder_instances.erase(p_region);
			occluders.erase(p_region);
		}
		return;
	}

	const int size = data->get_region_size();
	const int step = MIN(OCCLUDER_STEP, size);
	const int cells = size / step;
	const Vector2i base = p_region * size;

	// Each occluder vertex sits at the lowest height anywhere in the cells
	// around it, so the occluder always stays under the real surface. An
	// occluder poking above the ground would hide things that are visible.
	const Rect2i read_rect(base - Vector2i(step, step), Size2i(size + 2 * step + 1, size + 2 * step + 1));
	Vector<float> heights;
	Vector<uint8_t> valid;
	Vector<uint8_t> holes;
	heights.resize(read_rect.size.x * read_rect.size.y);
	valid.resize(heights.size());
	holes.resize(heights.size());
	data->read_heights(read_rect, heights.ptrw(), valid.ptrw());
	data->read_holes(read_rect, holes.ptrw());

	const int vw = cells + 1;
	PackedVector3Array vertices;
	vertices.resize(vw * vw);
	Vector<uint8_t> vertex_ok;
	vertex_ok.resize(vw * vw);
	for (int cz = 0; cz <= cells; cz++) {
		for (int cx = 0; cx <= cells; cx++) {
			const Vector2i centre(cx * step + step, cz * step + step); // In read_rect space.
			float lo = Math::INF;
			bool any = false;
			for (int z = centre.y - step; z <= centre.y + step; z++) {
				for (int x = centre.x - step; x <= centre.x + step; x++) {
					const int i = z * read_rect.size.x + x;
					if (valid[i] && !holes[i]) {
						lo = MIN(lo, heights[i]);
						any = true;
					}
				}
			}
			vertex_ok.write[cz * vw + cx] = any;
			vertices.write[cz * vw + cx] = Vector3((base.x + cx * step) * vertex_spacing, any ? lo - 0.05f * step * vertex_spacing : 0.0f, (base.y + cz * step) * vertex_spacing);
		}
	}

	PackedInt32Array indices;
	for (int cz = 0; cz < cells; cz++) {
		for (int cx = 0; cx < cells; cx++) {
			// A cell with a hole anywhere in it occludes nothing, or it would
			// hide what can be seen through the hole.
			bool clear = vertex_ok[cz * vw + cx] && vertex_ok[cz * vw + cx + 1] && vertex_ok[(cz + 1) * vw + cx] && vertex_ok[(cz + 1) * vw + cx + 1];
			for (int z = cz * step + step; clear && z <= cz * step + 2 * step; z++) {
				for (int x = cx * step + step; clear && x <= cx * step + 2 * step; x++) {
					const int i = z * read_rect.size.x + x;
					clear = valid[i] && !holes[i];
				}
			}
			if (!clear) {
				continue;
			}
			const int a = cz * vw + cx;
			const int b = a + 1;
			const int c = a + vw;
			const int d = c + 1;
			indices.push_back(a);
			indices.push_back(b);
			indices.push_back(c);
			indices.push_back(b);
			indices.push_back(d);
			indices.push_back(c);
		}
	}

	if (!occluder) {
		occluder = &occluders.insert(p_region, rs->occluder_create())->value;
		const RID instance = rs->instance_create2(*occluder, get_world_3d()->get_scenario());
		rs->instance_set_transform(instance, Transform3D(Basis(), _get_origin()));
		rs->instance_set_visible(instance, is_visible_in_tree());
		occluder_instances.insert(p_region, instance);
	}
	rs->occluder_set_mesh(*occluder, vertices, indices);
}

void Terrain3D::update_collision() {
	for (const Vector2i &region : dirty_collision) {
		if (region_slices.has(region)) {
			_update_region_collision(region);
			_update_region_occluder(region);
		}
	}
	dirty_collision.clear();
	last_collision_update_usec = OS::get_singleton()->get_ticks_usec();
	update_gizmos();
}

/* -------------------------------------------------------------------------- */
/*                                   Queries                                  */
/* -------------------------------------------------------------------------- */

Vector2 Terrain3D::global_to_vertex(const Vector3 &p_global_position) const {
	const Vector3 origin = _get_origin();
	return Vector2(p_global_position.x - origin.x, p_global_position.z - origin.z) / vertex_spacing;
}

Vector3 Terrain3D::vertex_to_global(const Vector2i &p_vertex) const {
	const Vector3 origin = _get_origin();
	float h = 0.0;
	if (data.is_valid()) {
		data->get_render_height(p_vertex, h);
	}
	return origin + Vector3(p_vertex.x * vertex_spacing, h, p_vertex.y * vertex_spacing);
}

float Terrain3D::get_height(const Vector3 &p_global_position) const {
	float h;
	if (data.is_null() || !data->get_height_interpolated(global_to_vertex(p_global_position), h)) {
		return Math::NaN;
	}
	return _get_origin().y + h;
}

Vector3 Terrain3D::get_normal(const Vector3 &p_global_position) const {
	if (data.is_null()) {
		return Vector3();
	}
	return data->get_normal_interpolated(global_to_vertex(p_global_position), vertex_spacing);
}

PackedFloat32Array Terrain3D::get_layer_weights(const Vector3 &p_global_position) const {
	PackedFloat32Array result;
	if (data.is_null()) {
		return result;
	}
	const Vector2 v = global_to_vertex(p_global_position);
	float w[TerrainData::MAX_LAYERS];
	data->get_layer_weights_interpolated(v, w);
	const int count = data->get_layer_count();

	_apply_auto_slope(data->get_normal_interpolated(v, vertex_spacing), w);

	result.resize(count);
	for (int i = 0; i < count; i++) {
		result.set(i, w[i]);
	}
	return result;
}

void Terrain3D::_apply_auto_slope(const Vector3 &p_normal, float *r_weights) const {
	// Match what is drawn: auto slope paints cliffs without touching the data,
	// and footsteps on a cliff should still sound like rock.
	if (terrain_material.is_null() || !terrain_material->is_auto_slope() || data.is_null() || terrain_material->get_auto_slope_layer() >= data->get_layer_count()) {
		return;
	}
	const float lo = 1.0 - Math::cos(Math::deg_to_rad(MAX(terrain_material->get_auto_slope_angle() - terrain_material->get_auto_slope_blend() * 0.5f, 0.0f)));
	const float hi = 1.0 - Math::cos(Math::deg_to_rad(MIN(terrain_material->get_auto_slope_angle() + terrain_material->get_auto_slope_blend() * 0.5f, 90.0f)));
	const float t = CLAMP((1.0f - p_normal.y - lo) / MAX(hi - lo, 0.001f), 0.0f, 1.0f);
	const float a = t * t * (3.0f - 2.0f * t);
	for (int i = 0; i < TerrainData::MAX_LAYERS; i++) {
		r_weights[i] *= 1.0f - a;
	}
	r_weights[terrain_material->get_auto_slope_layer()] += a;
}

bool Terrain3D::sample_surface(const Vector2 &p_local, SurfaceSample &r_sample) const {
	if (data.is_null()) {
		return false;
	}
	const Vector2 v = p_local / vertex_spacing;
	if (!data->get_height_interpolated(v, r_sample.height)) {
		return false;
	}
	r_sample.normal = data->get_normal_interpolated(v, vertex_spacing);
	data->get_layer_weights_interpolated(v, r_sample.weights);
	_apply_auto_slope(r_sample.normal, r_sample.weights);
	r_sample.color = data->get_color_interpolated(v);
	return true;
}

int Terrain3D::get_dominant_layer(const Vector3 &p_global_position) const {
	if (data.is_null() || Math::is_nan(get_height(p_global_position))) {
		return -1;
	}
	const PackedFloat32Array w = get_layer_weights(p_global_position);
	int best = -1;
	float best_w = 0.0;
	for (int i = 0; i < w.size(); i++) {
		if (w[i] > best_w) {
			best_w = w[i];
			best = i;
		}
	}
	return best;
}

String Terrain3D::get_surface_name(const Vector3 &p_global_position) const {
	const int layer = get_dominant_layer(p_global_position);
	if (layer < 0) {
		return String();
	}
	const Ref<TerrainLayer> l = data->get_layer(layer);
	return l.is_valid() ? l->get_display_name() : String();
}

float Terrain3D::get_wetness(const Vector3 &p_global_position) const {
	if (data.is_null()) {
		return 0.0;
	}
	return data->get_color_interpolated(global_to_vertex(p_global_position)).a;
}

AABB Terrain3D::get_terrain_aabb() const {
	if (data.is_null() || chunks.is_empty()) {
		return AABB();
	}
	float lo = Math::INF;
	float hi = -Math::INF;
	for (const KeyValue<Vector2i, Chunk> &E : chunks) {
		lo = MIN(lo, E.value.min_height);
		hi = MAX(hi, E.value.max_height);
	}
	const Rect2i bounds = data->get_vertex_bounds();
	const Vector3 origin = _get_origin();
	return AABB(origin + Vector3(bounds.position.x * vertex_spacing, lo, bounds.position.y * vertex_spacing),
			Vector3(bounds.size.x * vertex_spacing, MAX(hi - lo, 0.001f), bounds.size.y * vertex_spacing));
}

bool Terrain3D::intersect_ray_fast(const Vector3 &p_from, const Vector3 &p_direction, float p_max_distance, Vector3 &r_position) const {
	if (data.is_null() || data->get_region_count() == 0 || p_direction.is_zero_approx()) {
		return false;
	}
	const Vector3 dir = p_direction.normalized();
	AABB box = get_terrain_aabb();
	if (!box.has_volume()) {
		// Not laid out yet, as in a scene still loading: bound it from the data.
		const Vector2 range = data->get_height_range();
		const Rect2i bounds = data->get_vertex_bounds();
		const Vector3 origin = _get_origin();
		box = AABB(origin + Vector3(bounds.position.x * vertex_spacing, range.x, bounds.position.y * vertex_spacing),
				Vector3(bounds.size.x * vertex_spacing, MAX(range.y - range.x, 0.001f), bounds.size.y * vertex_spacing));
	}
	box = box.grow(vertex_spacing);

	// Clip the ray to the terrain's box, then march it in half vertex steps
	// and bisect the first step that goes under the surface.
	Vector3 enter;
	Vector3 exit;
	real_t t0 = 0.0;
	real_t t1 = p_max_distance;
	if (!box.has_point(p_from)) {
		bool inside = false;
		if (!box.find_intersects_ray(p_from, dir, inside, &enter)) {
			return false;
		}
		t0 = p_from.distance_to(enter);
	}
	if (box.intersects_segment(p_from + dir * p_max_distance, p_from + dir * t0, &exit)) {
		t1 = MIN(t1, p_from.distance_to(exit) + vertex_spacing);
	}
	if (t0 > t1) {
		return false;
	}

	const float step = vertex_spacing * 0.5f;
	float prev_t = t0;
	bool prev_above = true;
	bool have_prev = false;
	for (float t = t0; t <= t1 + step; t += step) {
		const Vector3 p = p_from + dir * MIN(t, t1);
		const float h = get_height(p);
		if (Math::is_nan(h)) {
			have_prev = false;
			prev_t = t;
			continue;
		}
		const bool above = p.y >= h;
		if (have_prev && prev_above && !above) {
			float lo = prev_t;
			float hi = MIN(t, t1);
			for (int i = 0; i < 16; i++) {
				const float mid = (lo + hi) * 0.5f;
				const Vector3 m = p_from + dir * mid;
				const float mh = get_height(m);
				if (!Math::is_nan(mh) && m.y < mh) {
					hi = mid;
				} else {
					lo = mid;
				}
			}
			r_position = p_from + dir * hi;
			return true;
		}
		if (!have_prev && !above) {
			// Starting underground: there is nothing to hit from below.
			return false;
		}
		prev_above = above;
		have_prev = true;
		prev_t = t;
		if (t >= t1) {
			break;
		}
	}
	return false;
}

Dictionary Terrain3D::intersect_ray(const Vector3 &p_from, const Vector3 &p_direction, float p_max_distance) const {
	Dictionary result;
	Vector3 hit;
	if (intersect_ray_fast(p_from, p_direction, p_max_distance, hit)) {
		result["position"] = hit;
		result["normal"] = get_normal(hit);
	}
	return result;
}

PackedVector3Array Terrain3D::get_picking_faces(int p_max_triangles) const {
	PackedVector3Array faces;
	if (data.is_null() || data->get_region_count() == 0) {
		return faces;
	}
	const int rs = data->get_region_size();
	const int64_t vertex_count = (int64_t)data->get_region_count() * rs * rs;
	int step = 1;
	while (step < rs && vertex_count / ((int64_t)step * step) * 2 > p_max_triangles) {
		step *= 2;
	}
	for (const Vector2i &coord : data->get_region_coord_list()) {
		const Vector2i base = coord * rs;
		for (int z = 0; z < rs; z += step) {
			for (int x = 0; x < rs; x += step) {
				float h00, h10, h01, h11;
				const Vector2i v = base + Vector2i(x, z);
				if (!data->get_render_height(v, h00) || !data->get_render_height(v + Vector2i(step, 0), h10) ||
						!data->get_render_height(v + Vector2i(0, step), h01) || !data->get_render_height(v + Vector2i(step, step), h11)) {
					continue;
				}
				const Vector3 a(v.x * vertex_spacing, h00, v.y * vertex_spacing);
				const Vector3 b((v.x + step) * vertex_spacing, h10, v.y * vertex_spacing);
				const Vector3 c(v.x * vertex_spacing, h01, (v.y + step) * vertex_spacing);
				const Vector3 d((v.x + step) * vertex_spacing, h11, (v.y + step) * vertex_spacing);
				faces.push_back(a);
				faces.push_back(b);
				faces.push_back(c);
				faces.push_back(b);
				faces.push_back(d);
				faces.push_back(c);
			}
		}
	}
	return faces;
}

/* -------------------------------------------------------------------------- */
/*                                  Settings                                  */
/* -------------------------------------------------------------------------- */

void Terrain3D::set_vertex_spacing(float p_spacing) {
	p_spacing = CLAMP(p_spacing, 0.05f, 64.0f);
	if (vertex_spacing == p_spacing) {
		return;
	}
	vertex_spacing = p_spacing;
	mesh_dirty = true;
	update_gizmos();
}

void Terrain3D::set_chunk_size(int p_size) {
	int size = 16;
	while (size < p_size && size < 256) {
		size *= 2;
	}
	if (data.is_valid()) {
		size = MIN(size, data->get_region_size());
	}
	if (chunk_size == size) {
		return;
	}
	chunk_size = size;
	mesh_dirty = true;
}

void Terrain3D::set_lod_bias(float p_bias) {
	lod_bias = MAX(p_bias, 0.001f);
	for (const KeyValue<Vector2i, Chunk> &E : chunks) {
		_update_instance_settings(E.value);
	}
}

void Terrain3D::set_cast_shadow(ShadowCasting p_mode) {
	ERR_FAIL_INDEX((int)p_mode, 4);
	cast_shadow = p_mode;
	for (const KeyValue<Vector2i, Chunk> &E : chunks) {
		_update_instance_settings(E.value);
	}
}

void Terrain3D::set_render_layers(uint32_t p_layers) {
	render_layers = p_layers;
	for (const KeyValue<Vector2i, Chunk> &E : chunks) {
		_update_instance_settings(E.value);
	}
}

void Terrain3D::set_occlusion_culling(bool p_enabled) {
	if (occlusion_culling == p_enabled) {
		return;
	}
	occlusion_culling = p_enabled;
	for (const Vector2i &region : slice_regions) {
		_update_region_occluder(region);
	}
}

void Terrain3D::set_collision_enabled(bool p_enabled) {
	if (collision_enabled == p_enabled) {
		return;
	}
	collision_enabled = p_enabled;
	_update_collision_settings();
	for (const Vector2i &region : slice_regions) {
		_update_region_collision(region);
	}
	notify_property_list_changed();
}

void Terrain3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
#ifndef PHYSICS_3D_DISABLED
	if (static_body.is_valid()) {
		PhysicsServer3D::get_singleton()->body_set_collision_layer(static_body, collision_layer);
	}
#endif
}

void Terrain3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
#ifndef PHYSICS_3D_DISABLED
	if (static_body.is_valid()) {
		PhysicsServer3D::get_singleton()->body_set_collision_mask(static_body, collision_mask);
	}
#endif
}

void Terrain3D::set_collision_priority(real_t p_priority) {
	collision_priority = p_priority;
#ifndef PHYSICS_3D_DISABLED
	if (static_body.is_valid()) {
		PhysicsServer3D::get_singleton()->body_set_collision_priority(static_body, collision_priority);
	}
#endif
}

void Terrain3D::set_debug_view(DebugView p_view) {
	debug_view = p_view;
	material_dirty = true;
	if (is_inside_world()) {
		_update_material_params();
		material_dirty = false;
	}
}

void Terrain3D::set_brush_preview(bool p_visible, const Vector3 &p_position, float p_radius, float p_hardness, const Color &p_color) {
	brush_visible = p_visible;
	brush_position = p_position;
	brush_radius = p_radius;
	brush_hardness = p_hardness;
	brush_color = p_color;
	if (material.is_valid()) {
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->material_set_param(material, SNAME("brush_enabled"), brush_visible);
		rs->material_set_param(material, SNAME("brush_position"), brush_position);
		rs->material_set_param(material, SNAME("brush_radius"), brush_radius);
		rs->material_set_param(material, SNAME("brush_hardness"), brush_hardness);
		rs->material_set_param(material, SNAME("brush_color"), brush_color);
	}
}

void Terrain3D::set_brush_segment(bool p_enabled, const Vector3 &p_start) {
	brush_segment = p_enabled;
	brush_segment_start = p_start;
	if (material.is_valid()) {
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->material_set_param(material, SNAME("brush_segment"), brush_segment);
		rs->material_set_param(material, SNAME("brush_segment_start"), brush_segment_start);
	}
}

PackedStringArray Terrain3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (data.is_null()) {
		warnings.push_back(RTR("This terrain has no data. Select it and use Terrain > New Terrain... in the Terrain dock, or assign a TerrainData."));
	} else if (data->get_region_count() == 0) {
		warnings.push_back(RTR("The terrain data has no regions. Use the Regions tool in the Terrain dock to add some."));
	}
	if (is_inside_tree()) {
		const Basis basis = get_global_transform().basis;
		if (!basis.is_equal_approx(Basis())) {
			warnings.push_back(RTR("Terrain3D ignores rotation and scale; only its position is used. Change vertex_spacing to resize it."));
		}
	}
	if ((bool)get_meta(SNAME("_layer_textures_converted"), false)) {
		warnings.push_back(RTR("The layers' textures differ in size, format or mipmaps, so they were converted to uncompressed RGBA8, which uses four times the video memory. Import every layer texture with the same settings (VRAM Compressed, mipmaps on, same size) to avoid this."));
	}
	return warnings;
}

void Terrain3D::_validate_property(PropertyInfo &p_property) const {
	if ((p_property.name == "collision_layer" || p_property.name == "collision_mask" || p_property.name == "collision_priority") && !collision_enabled) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (p_property.name == "debug_view") {
		// A viewing aid, not part of the scene.
		p_property.usage = PROPERTY_USAGE_EDITOR;
	}
}

/* -------------------------------------------------------------------------- */
/*                                 Navigation                                 */
/* -------------------------------------------------------------------------- */

void Terrain3D::navmesh_parse_init() {
#ifndef NAVIGATION_3D_DISABLED
	ERR_FAIL_NULL(NavigationServer3D::get_singleton());
	if (!_navmesh_source_geometry_parser.is_valid()) {
		_navmesh_source_geometry_parsing_callback = callable_mp_static(&Terrain3D::navmesh_parse_source_geometry);
		_navmesh_source_geometry_parser = NavigationServer3D::get_singleton()->source_geometry_parser_create();
		NavigationServer3D::get_singleton()->source_geometry_parser_set_callback(_navmesh_source_geometry_parser, _navmesh_source_geometry_parsing_callback);
	}
#endif
}

void Terrain3D::navmesh_parse_source_geometry(const Ref<NavigationMesh> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData3D> p_source_geometry_data, Node *p_node) {
#ifndef NAVIGATION_3D_DISABLED
	Terrain3D *terrain = Object::cast_to<Terrain3D>(p_node);
	if (terrain == nullptr || terrain->data.is_null() || terrain->data->get_region_count() == 0) {
		return;
	}

	const NavigationMesh::ParsedGeometryType parsed = p_navigation_mesh->get_parsed_geometry_type();
	bool wanted = parsed == NavigationMesh::PARSED_GEOMETRY_MESH_INSTANCES || parsed == NavigationMesh::PARSED_GEOMETRY_BOTH;
#ifndef PHYSICS_3D_DISABLED
	wanted = wanted || ((parsed == NavigationMesh::PARSED_GEOMETRY_STATIC_COLLIDERS || parsed == NavigationMesh::PARSED_GEOMETRY_BOTH) && terrain->collision_enabled && (terrain->collision_layer & p_navigation_mesh->get_collision_mask()));
#endif
	if (!wanted) {
		return;
	}

	const Ref<TerrainData> &data = terrain->data;
	const float sp = terrain->vertex_spacing;
	const Vector3 origin = terrain->_get_origin();

	// With a baking AABB set, only the terrain inside it is handed over: a
	// whole terrain at full resolution can be millions of triangles.
	Rect2i rect = data->get_vertex_bounds();
	rect.size += Vector2i(1, 1);
	AABB baking_aabb = p_navigation_mesh->get_filter_baking_aabb();
	if (baking_aabb.has_volume()) {
		baking_aabb.position += p_navigation_mesh->get_filter_baking_aabb_offset();
		const Vector2 lo = terrain->global_to_vertex(baking_aabb.position).floor();
		const Vector2 hi = terrain->global_to_vertex(baking_aabb.get_end()).ceil();
		rect = rect.intersection(Rect2i(Vector2i(lo), Vector2i(hi - lo) + Vector2i(1, 1)));
		if (!rect.has_area()) {
			return;
		}
	}

	Vector<float> heights;
	Vector<uint8_t> valid;
	Vector<uint8_t> holes;
	heights.resize(rect.size.x * rect.size.y);
	valid.resize(heights.size());
	holes.resize(heights.size());
	for (int z = 0; z < rect.size.y; z++) {
		for (int x = 0; x < rect.size.x; x++) {
			float h;
			const bool ok = data->get_render_height(rect.position + Vector2i(x, z), h);
			heights.write[z * rect.size.x + x] = ok ? h : 0.0f;
			valid.write[z * rect.size.x + x] = ok;
		}
	}

	PackedVector3Array faces;
	const int w = rect.size.x;
	for (int z = 0; z + 1 < rect.size.y; z++) {
		for (int x = 0; x + 1 < rect.size.x; x++) {
			const int i = z * w + x;
			if (!valid[i] || !valid[i + 1] || !valid[i + w] || !valid[i + w + 1]) {
				continue;
			}
			const Vector2i v = rect.position + Vector2i(x, z);
			const Vector3 a = origin + Vector3(v.x * sp, heights[i], v.y * sp);
			const Vector3 b = origin + Vector3((v.x + 1) * sp, heights[i + 1], v.y * sp);
			const Vector3 c = origin + Vector3(v.x * sp, heights[i + w], (v.y + 1) * sp);
			const Vector3 d = origin + Vector3((v.x + 1) * sp, heights[i + w + 1], (v.y + 1) * sp);
			faces.push_back(a);
			faces.push_back(b);
			faces.push_back(c);
			faces.push_back(b);
			faces.push_back(d);
			faces.push_back(c);
		}
	}
	if (!faces.is_empty()) {
		p_source_geometry_data->add_faces(faces, Transform3D());
	}
#endif
}

/* -------------------------------------------------------------------------- */
/*                                  Bindings                                  */
/* -------------------------------------------------------------------------- */

void Terrain3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_data", "data"), &Terrain3D::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &Terrain3D::get_data);
	ClassDB::bind_method(D_METHOD("set_terrain_material", "material"), &Terrain3D::set_terrain_material);
	ClassDB::bind_method(D_METHOD("get_terrain_material"), &Terrain3D::get_terrain_material);
	ClassDB::bind_method(D_METHOD("set_vertex_spacing", "spacing"), &Terrain3D::set_vertex_spacing);
	ClassDB::bind_method(D_METHOD("get_vertex_spacing"), &Terrain3D::get_vertex_spacing);
	ClassDB::bind_method(D_METHOD("set_chunk_size", "size"), &Terrain3D::set_chunk_size);
	ClassDB::bind_method(D_METHOD("get_chunk_size"), &Terrain3D::get_chunk_size);
	ClassDB::bind_method(D_METHOD("set_lod_bias", "bias"), &Terrain3D::set_lod_bias);
	ClassDB::bind_method(D_METHOD("get_lod_bias"), &Terrain3D::get_lod_bias);
	ClassDB::bind_method(D_METHOD("set_cast_shadow", "mode"), &Terrain3D::set_cast_shadow);
	ClassDB::bind_method(D_METHOD("get_cast_shadow"), &Terrain3D::get_cast_shadow);
	ClassDB::bind_method(D_METHOD("set_render_layers", "layers"), &Terrain3D::set_render_layers);
	ClassDB::bind_method(D_METHOD("get_render_layers"), &Terrain3D::get_render_layers);
	ClassDB::bind_method(D_METHOD("set_occlusion_culling", "enabled"), &Terrain3D::set_occlusion_culling);
	ClassDB::bind_method(D_METHOD("is_occlusion_culling"), &Terrain3D::is_occlusion_culling);
	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &Terrain3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &Terrain3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &Terrain3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &Terrain3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Terrain3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Terrain3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_collision_priority", "priority"), &Terrain3D::set_collision_priority);
	ClassDB::bind_method(D_METHOD("get_collision_priority"), &Terrain3D::get_collision_priority);
	ClassDB::bind_method(D_METHOD("set_debug_view", "view"), &Terrain3D::set_debug_view);
	ClassDB::bind_method(D_METHOD("get_debug_view"), &Terrain3D::get_debug_view);

	ClassDB::bind_method(D_METHOD("get_height", "global_position"), &Terrain3D::get_height);
	ClassDB::bind_method(D_METHOD("get_normal", "global_position"), &Terrain3D::get_normal);
	ClassDB::bind_method(D_METHOD("get_layer_weights", "global_position"), &Terrain3D::get_layer_weights);
	ClassDB::bind_method(D_METHOD("get_dominant_layer", "global_position"), &Terrain3D::get_dominant_layer);
	ClassDB::bind_method(D_METHOD("get_surface_name", "global_position"), &Terrain3D::get_surface_name);
	ClassDB::bind_method(D_METHOD("get_wetness", "global_position"), &Terrain3D::get_wetness);
	ClassDB::bind_method(D_METHOD("intersect_ray", "from", "direction", "max_distance"), &Terrain3D::intersect_ray, DEFVAL(10000.0));
	ClassDB::bind_method(D_METHOD("get_terrain_aabb"), &Terrain3D::get_terrain_aabb);
	ClassDB::bind_method(D_METHOD("global_to_vertex", "global_position"), &Terrain3D::global_to_vertex);
	ClassDB::bind_method(D_METHOD("vertex_to_global", "vertex"), &Terrain3D::vertex_to_global);
	ClassDB::bind_method(D_METHOD("update_collision"), &Terrain3D::update_collision);
	ClassDB::bind_method(D_METHOD("get_physics_body"), &Terrain3D::get_physics_body);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, "TerrainData"), "set_data", "get_data");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_material", PROPERTY_HINT_RESOURCE_TYPE, "TerrainMaterial"), "set_terrain_material", "get_terrain_material");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "vertex_spacing", PROPERTY_HINT_RANGE, "0.05,64,0.01,or_greater,suffix:m"), "set_vertex_spacing", "get_vertex_spacing");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "chunk_size", PROPERTY_HINT_ENUM, "16:16,32:32,64:64,128:128,256:256"), "set_chunk_size", "get_chunk_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_bias", PROPERTY_HINT_RANGE, "0.001,128,0.001,or_greater"), "set_lod_bias", "get_lod_bias");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cast_shadow", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only"), "set_cast_shadow", "get_cast_shadow");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_layers", PROPERTY_HINT_LAYERS_3D_RENDER), "set_render_layers", "get_render_layers");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "occlusion_culling"), "set_occlusion_culling", "is_occlusion_culling");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_view", PROPERTY_HINT_ENUM, "Disabled,Height,Normals,Layers,Color,Wetness,Grid"), "set_debug_view", "get_debug_view");

	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_priority"), "set_collision_priority", "get_collision_priority");

	ADD_SIGNAL(MethodInfo("terrain_changed", PropertyInfo(Variant::RECT2, "area")));

	BIND_ENUM_CONSTANT(DEBUG_VIEW_DISABLED);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_HEIGHT);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_NORMALS);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_LAYERS);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_COLOR);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_WETNESS);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_GRID);

	BIND_ENUM_CONSTANT(SHADOW_CASTING_OFF);
	BIND_ENUM_CONSTANT(SHADOW_CASTING_ON);
	BIND_ENUM_CONSTANT(SHADOW_CASTING_DOUBLE_SIDED);
	BIND_ENUM_CONSTANT(SHADOW_CASTING_SHADOWS_ONLY);
}
