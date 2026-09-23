/**************************************************************************/
/*  foliage_3d.cpp                                                        */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "foliage_3d.h"

#include "terrain_3d.h"

#include "core/config/engine.h"
#include "core/math/random_pcg.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/templates/hashfuncs.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"

#ifndef PHYSICS_3D_DISABLED
#include "core/math/convex_hull.h"
#include "servers/physics_3d/physics_server_3d.h"
#endif

#ifndef NAVIGATION_3D_DISABLED
#include "scene/resources/3d/navigation_mesh_source_geometry_data_3d.h"
#include "scene/resources/navigation_mesh.h"
#include "servers/navigation_3d/navigation_server_3d.h"

Callable Foliage3D::_navmesh_source_geometry_parsing_callback;
RID Foliage3D::_navmesh_source_geometry_parser;
#endif

Vector3 Foliage3D::editor_focus;
bool Foliage3D::editor_focus_valid = false;

namespace {

// Grass cells never hold more than this many blades, however dense the type.
constexpr int MAX_GRASS_PER_CELL = 40000;
// A grass type with no visibility range still has to stop somewhere.
constexpr float DEFAULT_GRASS_RANGE = 100.0;
constexpr float MAX_GRASS_RANGE = 1000.0;
constexpr int GRASS_FLOATS = 20; // 3x4 transform, colour, and the ground's normal as custom data.
constexpr int OBJECT_FLOATS = 12;

uint32_t cell_seed(const Vector3i &p_key) {
	uint32_t h = hash_murmur3_one_32((uint32_t)p_key.x);
	h = hash_murmur3_one_32((uint32_t)p_key.y, h);
	h = hash_murmur3_one_32((uint32_t)p_key.z, h);
	return hash_fmix32(h);
}

void write_transform(float *w, const Basis &p_basis, const Vector3 &p_origin) {
	w[0] = p_basis.rows[0][0];
	w[1] = p_basis.rows[0][1];
	w[2] = p_basis.rows[0][2];
	w[3] = p_origin.x;
	w[4] = p_basis.rows[1][0];
	w[5] = p_basis.rows[1][1];
	w[6] = p_basis.rows[1][2];
	w[7] = p_origin.y;
	w[8] = p_basis.rows[2][0];
	w[9] = p_basis.rows[2][1];
	w[10] = p_basis.rows[2][2];
	w[11] = p_origin.z;
}

// Yaw, then a tilt off vertical, then leaning toward the ground's normal,
// then scale. Tilt direction and amount come from the instance's variation
// value, so an object keeps its lean across rebuilds.
Basis placement_basis(const FoliageType *p_type, const Vector3 &p_normal, float p_yaw, float p_scale, float p_variation) {
	Basis b;
	if (p_type->is_random_rotation()) {
		b = Basis(Vector3(0, 1, 0), p_yaw);
	}
	if (p_type->get_random_tilt() > 0.0f) {
		const float dir = Math::fract(p_variation * 7.31f) * Math::TAU;
		const float amount = Math::deg_to_rad(p_type->get_random_tilt()) * Math::fract(p_variation * 13.7f);
		b = Basis(Vector3(Math::cos(dir), 0, Math::sin(dir)), amount) * b;
	}
	if (p_type->get_align_to_normal() > 0.0f && p_normal.y > 0.0f) {
		const Vector3 up = Vector3(0, 1, 0).lerp(p_normal, p_type->get_align_to_normal()).normalized();
		if (!up.is_equal_approx(Vector3(0, 1, 0))) {
			b = Basis(Quaternion(Vector3(0, 1, 0), up)) * b;
		}
	}
	return b.scaled(Vector3(p_scale, p_scale, p_scale));
}

float slope_degrees(const Vector3 &p_normal) {
	return Math::rad_to_deg(Math::acos(CLAMP(p_normal.y, -1.0f, 1.0f)));
}

} // namespace

/* -------------------------------------------------------------------------- */
/*                                  Lifecycle                                 */
/* -------------------------------------------------------------------------- */

Foliage3D::Foliage3D() {
	set_notify_transform(true);
}

Foliage3D::~Foliage3D() {
	_free_all();
}

void Foliage3D::set_editor_focus(const Vector3 &p_global_position) {
	editor_focus = p_global_position;
	editor_focus_valid = true;
}

void Foliage3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_process_internal(true);
			layout_dirty = true;
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			_free_all();
			layout_dirty = true;
		} break;

		case NOTIFICATION_EXIT_TREE: {
			Terrain3D *terrain = _peek_terrain();
			if (terrain && terrain->is_connected(SNAME("terrain_changed"), callable_mp(this, &Foliage3D::_terrain_changed))) {
				terrain->disconnect(SNAME("terrain_changed"), callable_mp(this, &Foliage3D::_terrain_changed));
			}
			terrain_id = ObjectID();
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			_update_transforms();
		} break;

		case NOTIFICATION_VISIBILITY_CHANGED: {
			RenderingServer *rs = RenderingServer::get_singleton();
			const bool visible = is_visible_in_tree();
			for (const KeyValue<Vector3i, Cell> &E : grass_cells) {
				if (E.value.instance.is_valid()) {
					rs->instance_set_visible(E.value.instance, visible);
				}
			}
			for (const KeyValue<Vector3i, Cell> &E : object_cells) {
				if (E.value.instance.is_valid()) {
					rs->instance_set_visible(E.value.instance, visible);
				}
			}
		} break;

		case NOTIFICATION_INTERNAL_PROCESS: {
			_process_updates();
		} break;
	}
}

Terrain3D *Foliage3D::_peek_terrain() const {
	return ObjectDB::get_instance<Terrain3D>(terrain_id);
}

Terrain3D *Foliage3D::_get_terrain() {
	Terrain3D *found = is_inside_tree() ? Object::cast_to<Terrain3D>(get_node_or_null(terrain_path)) : nullptr;
	const ObjectID found_id = found ? found->get_instance_id() : ObjectID();
	if (found_id == terrain_id) {
		return found;
	}
	Terrain3D *previous = _peek_terrain();
	if (previous && previous->is_connected(SNAME("terrain_changed"), callable_mp(this, &Foliage3D::_terrain_changed))) {
		previous->disconnect(SNAME("terrain_changed"), callable_mp(this, &Foliage3D::_terrain_changed));
	}
	terrain_id = found_id;
	if (found) {
		found->connect(SNAME("terrain_changed"), callable_mp(this, &Foliage3D::_terrain_changed));
	}
	layout_dirty = true;
	update_configuration_warnings();
	return found;
}

bool Foliage3D::_get_focus(Vector3 &r_local) const {
	const Terrain3D *terrain = _peek_terrain();
	if (!terrain) {
		return false;
	}
	Vector3 focus;
	if (Engine::get_singleton()->is_editor_hint()) {
		if (!editor_focus_valid) {
			return false;
		}
		focus = editor_focus;
	} else {
		const Viewport *viewport = get_viewport();
		const Camera3D *camera = viewport ? viewport->get_camera_3d() : nullptr;
		if (!camera) {
			return false;
		}
		focus = camera->get_global_position();
	}
	r_local = focus - terrain->get_global_position();
	return true;
}

/* -------------------------------------------------------------------------- */
/*                                   Changes                                  */
/* -------------------------------------------------------------------------- */

void Foliage3D::set_data(const Ref<FoliageData> &p_data) {
	if (data == p_data) {
		return;
	}
	if (data.is_valid()) {
		data->disconnect(SNAME("types_changed"), callable_mp(this, &Foliage3D::_data_types_changed));
		data->disconnect(SNAME("type_changed"), callable_mp(this, &Foliage3D::_data_type_changed));
		data->disconnect(SNAME("density_changed"), callable_mp(this, &Foliage3D::_data_density_changed));
		data->disconnect(SNAME("instances_changed"), callable_mp(this, &Foliage3D::_data_instances_changed));
	}
	data = p_data;
	if (data.is_valid()) {
		data->connect(SNAME("types_changed"), callable_mp(this, &Foliage3D::_data_types_changed));
		data->connect(SNAME("type_changed"), callable_mp(this, &Foliage3D::_data_type_changed));
		data->connect(SNAME("density_changed"), callable_mp(this, &Foliage3D::_data_density_changed));
		data->connect(SNAME("instances_changed"), callable_mp(this, &Foliage3D::_data_instances_changed));
	}
	layout_dirty = true;
	update_configuration_warnings();
}

void Foliage3D::_data_types_changed() {
	layout_dirty = true;
}

void Foliage3D::_data_type_changed(int p_type) {
	// A changed type can mean a different mesh, density or rules everywhere,
	// so everything it grew goes.
	LocalVector<Vector3i> doomed;
	for (const KeyValue<Vector3i, Cell> &E : grass_cells) {
		if (E.key.z == p_type) {
			doomed.push_back(E.key);
		}
	}
	for (const Vector3i &key : doomed) {
		_free_cell(grass_cells[key]);
		grass_cells.erase(key);
	}
	dirty_object_types.insert(p_type);
}

Vector2i Foliage3D::_cell_of(const Vector2 &p_local, float p_size) const {
	return Vector2i((int)Math::floor(p_local.x / p_size), (int)Math::floor(p_local.y / p_size));
}

void Foliage3D::_mark_cells(HashSet<Vector3i> &r_set, const HashMap<Vector3i, Cell> &p_cells, int p_type, const Rect2 &p_area, float p_size, bool p_create) {
	const Vector2i c0 = _cell_of(p_area.position, p_size);
	const Vector2i c1 = _cell_of(p_area.get_end(), p_size);
	if ((int64_t)(c1.x - c0.x + 1) * (c1.y - c0.y + 1) > 65536) {
		// Too big an area to walk cell by cell; only existing cells matter.
		for (const KeyValue<Vector3i, Cell> &E : p_cells) {
			if (p_type < 0 || E.key.z == p_type) {
				r_set.insert(E.key);
			}
		}
		return;
	}
	const int t0 = p_type < 0 ? 0 : p_type;
	const int t1 = p_type < 0 ? (data.is_valid() ? data->get_type_count() - 1 : -1) : p_type;
	for (int t = t0; t <= t1; t++) {
		for (int cz = c0.y; cz <= c1.y; cz++) {
			for (int cx = c0.x; cx <= c1.x; cx++) {
				const Vector3i key(cx, cz, t);
				if (p_create || p_cells.has(key)) {
					r_set.insert(key);
				}
			}
		}
	}
}

void Foliage3D::_data_density_changed(int p_type, const Rect2 &p_area) {
	if (!p_area.has_area()) {
		for (const KeyValue<Vector3i, Cell> &E : grass_cells) {
			if (E.key.z == p_type) {
				dirty_grass.insert(E.key);
			}
		}
		return;
	}
	// Painting where no cell exists yet is picked up by the next pass that
	// looks for missing cells, so only existing cells are marked.
	_mark_cells(dirty_grass, grass_cells, p_type, p_area, grass_cell_size, false);
}

void Foliage3D::_data_instances_changed(int p_type, const Rect2 &p_area) {
	if (p_area == Rect2()) {
		// No area means the whole list was replaced.
		dirty_object_types.insert(p_type);
		return;
	}
	_mark_cells(dirty_objects, object_cells, p_type, p_area.grow(0.01), object_cell_size, true);
}

void Foliage3D::_terrain_changed(const Rect2 &p_area) {
	if (!p_area.has_area()) {
		layout_dirty = true;
		return;
	}
	_mark_cells(dirty_grass, grass_cells, -1, p_area, grass_cell_size, false);
	_mark_cells(dirty_objects, object_cells, -1, p_area, object_cell_size, false);
}

/* -------------------------------------------------------------------------- */
/*                                   Updates                                  */
/* -------------------------------------------------------------------------- */

void Foliage3D::_free_cell(Cell &r_cell) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (r_cell.instance.is_valid()) {
		rs->free_rid(r_cell.instance);
		r_cell.instance = RID();
	}
	if (r_cell.multimesh.is_valid()) {
		rs->free_rid(r_cell.multimesh);
		r_cell.multimesh = RID();
	}
#ifndef PHYSICS_3D_DISABLED
	if (r_cell.body.is_valid()) {
		PhysicsServer3D::get_singleton()->free_rid(r_cell.body);
		r_cell.body = RID();
	}
#endif
	for (const ObjectID &id : r_cell.scenes) {
		Node *node = ObjectDB::get_instance<Node>(id);
		if (node) {
			if (node->get_parent()) {
				node->get_parent()->remove_child(node);
			}
			memdelete(node);
		}
	}
	r_cell.scenes.clear();
	r_cell.count = 0;
}

void Foliage3D::_free_all() {
	for (KeyValue<Vector3i, Cell> &E : grass_cells) {
		_free_cell(E.value);
	}
	grass_cells.clear();
	for (KeyValue<Vector3i, Cell> &E : object_cells) {
		_free_cell(E.value);
	}
	object_cells.clear();
	dirty_grass.clear();
	dirty_objects.clear();
	dirty_object_types.clear();
}

void Foliage3D::regenerate() {
	layout_dirty = true;
}

void Foliage3D::_process_updates() {
	if (!is_inside_world()) {
		return;
	}
	Terrain3D *terrain = _get_terrain();
	if (!terrain || data.is_null() || terrain->get_data().is_null()) {
		if (!grass_cells.is_empty() || !object_cells.is_empty()) {
			_free_all();
		}
		return;
	}

	if (layout_dirty) {
		layout_dirty = false;
		_free_all();
		for (int t = 0; t < data->get_type_count(); t++) {
			dirty_object_types.insert(t);
		}
	}

	if (!dirty_object_types.is_empty()) {
		LocalVector<int> types;
		for (const int t : dirty_object_types) {
			types.push_back(t);
		}
		dirty_object_types.clear();
		for (const int t : types) {
			_rebuild_object_type(terrain, t);
		}
	}
	if (!dirty_objects.is_empty()) {
		LocalVector<Vector3i> cells;
		for (const Vector3i &key : dirty_objects) {
			cells.push_back(key);
		}
		dirty_objects.clear();
		for (const Vector3i &key : cells) {
			_rebuild_object_cell(terrain, key);
		}
	}

	_update_grass(terrain);
}

void Foliage3D::_update_grass(Terrain3D *p_terrain) {
	Vector3 focus3;
	if (!_get_focus(focus3)) {
		return;
	}
	const Vector2 focus(focus3.x, focus3.z);
	const float s = grass_cell_size;

	// Which cells each grass type wants around the camera.
	HashSet<Vector3i> needed;
	LocalVector<float> ranges;
	ranges.resize(data->get_type_count());
	for (int t = 0; t < data->get_type_count(); t++) {
		ranges[t] = 0.0;
		const Ref<FoliageType> type = data->get_type(t);
		if (type.is_null() || type->get_kind() != FoliageType::KIND_GRASS) {
			continue;
		}
		const FoliageData::TypeData *td = data->get_type_data(t);
		if (!td || td->density.is_empty()) {
			continue;
		}
		float range = type->get_visibility_range() > 0.0f ? type->get_visibility_range() : DEFAULT_GRASS_RANGE;
		range = MIN(range, MAX_GRASS_RANGE);
		ranges[t] = range;
		const Vector2i c0 = _cell_of(focus - Vector2(range, range), s);
		const Vector2i c1 = _cell_of(focus + Vector2(range, range), s);
		for (int cz = c0.y; cz <= c1.y; cz++) {
			for (int cx = c0.x; cx <= c1.x; cx++) {
				const Rect2 rect(Vector2(cx, cz) * s, Vector2(s, s));
				const Vector2 nearest = focus.clamp(rect.position, rect.get_end());
				if (nearest.distance_to(focus) <= range) {
					needed.insert(Vector3i(cx, cz, t));
				}
			}
		}
	}

	// Drop cells well behind the camera. The extra cell of slack stops a cell
	// on the boundary being thrown away and regrown as the camera jitters.
	LocalVector<Vector3i> doomed;
	for (const KeyValue<Vector3i, Cell> &E : grass_cells) {
		if (needed.has(E.key)) {
			continue;
		}
		const float range = E.key.z < (int)ranges.size() ? ranges[E.key.z] : 0.0f;
		const Rect2 rect(Vector2(E.key.x, E.key.y) * s, Vector2(s, s));
		if (range <= 0.0f || focus.clamp(rect.position, rect.get_end()).distance_to(focus) > range + s) {
			doomed.push_back(E.key);
		}
	}
	for (const Vector3i &key : doomed) {
		_free_cell(grass_cells[key]);
		grass_cells.erase(key);
		dirty_grass.erase(key);
	}

	// Grow what is missing or stale, nearest first, within the frame budget.
	LocalVector<Pair<float, Vector3i>> work;
	for (const Vector3i &key : needed) {
		if (!grass_cells.has(key) || dirty_grass.has(key)) {
			const Rect2 rect(Vector2(key.x, key.y) * s, Vector2(s, s));
			work.push_back(Pair<float, Vector3i>(focus.clamp(rect.position, rect.get_end()).distance_squared_to(focus), key));
		}
	}
	if (work.is_empty()) {
		return;
	}
	struct ByDistance {
		bool operator()(const Pair<float, Vector3i> &a, const Pair<float, Vector3i> &b) const { return a.first < b.first; }
	};
	work.sort_custom<ByDistance>();

	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	const uint64_t budget = (uint64_t)(generation_budget_msec * 1000.0f);
	for (uint32_t i = 0; i < work.size(); i++) {
		_generate_grass_cell(p_terrain, work[i].second);
		dirty_grass.erase(work[i].second);
		if (OS::get_singleton()->get_ticks_usec() - start > budget) {
			break;
		}
	}
}

void Foliage3D::_generate_grass_cell(Terrain3D *p_terrain, const Vector3i &p_key) {
	Cell *existing = grass_cells.getptr(p_key);
	if (existing) {
		_free_cell(*existing);
	} else {
		existing = &grass_cells.insert(p_key, Cell())->value;
	}
	Cell &cell = *existing;

	const int t = p_key.z;
	const Ref<FoliageType> type = data->get_type(t);
	if (type.is_null()) {
		return;
	}
	const Ref<Mesh> mesh = type->get_render_mesh();
	if (mesh.is_null()) {
		return;
	}

	const float s = grass_cell_size;
	const Vector2 origin = Vector2(p_key.x, p_key.y) * s;

	// Skip the sampling entirely for a cell nothing has been painted in.
	{
		const FoliageData::TypeData *td = data->get_type_data(t);
		const Vector2i t0 = data->local_to_texel(origin);
		const Vector2i t1 = data->local_to_texel(origin + Vector2(s, s));
		const Vector2i d0(TerrainData::floor_div(t0.x - 1, FoliageData::DENSITY_CELL), TerrainData::floor_div(t0.y - 1, FoliageData::DENSITY_CELL));
		const Vector2i d1(TerrainData::floor_div(t1.x + 1, FoliageData::DENSITY_CELL), TerrainData::floor_div(t1.y + 1, FoliageData::DENSITY_CELL));
		bool any = false;
		for (int dz = d0.y; dz <= d1.y && !any; dz++) {
			for (int dx = d0.x; dx <= d1.x && !any; dx++) {
				any = td->density.has(Vector2i(dx, dz));
			}
		}
		if (!any) {
			return;
		}
	}

	const int max_count = MIN((int)(type->get_density() * s * s), MAX_GRASS_PER_CELL);
	const int n = MAX(1, (int)Math::ceil(Math::sqrt((float)max_count)));
	const float step = s / n;
	const int layer_count = p_terrain->get_data()->get_layer_count();
	const float scale_min = type->get_scale_min();
	const float scale_max = type->get_scale_max();
	const float variation = type->get_color_variation();
	const bool tint = type->is_terrain_tint();
	const float offset = type->get_height_offset();
	const FoliageType *type_ptr = type.ptr();

	LocalVector<float> buffer;
	buffer.reserve(n * n * GRASS_FLOATS);
	RandomPCG rng(cell_seed(p_key));
	Terrain3D::SurfaceSample sample;
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			// Every random number is drawn whether or not the blade survives,
			// so repainting density never reshuffles the blades that stay.
			const float jx = rng.randf();
			const float jz = rng.randf();
			const float accept = rng.randf();
			const float rs = rng.randf();
			const float ry = rng.randf();
			const float rv = rng.randf();

			const Vector2 local = origin + Vector2((i + jx) * step, (j + jz) * step);
			const float density = data->sample_density(t, local);
			if (accept >= density) {
				continue;
			}
			if (!p_terrain->sample_surface(local, sample)) {
				continue;
			}
			if (!type_ptr->accepts(slope_degrees(sample.normal), sample.weights, layer_count)) {
				continue;
			}
			// Thin blades toward a painted edge, so it feathers out.
			const float scale = Math::lerp(scale_min, scale_max, rs) * (0.55f + 0.45f * density);
			const Basis basis = placement_basis(type_ptr, sample.normal, ry * Math::TAU, scale, rv);
			const Vector3 position(local.x, sample.height + offset, local.y);

			const uint32_t at = buffer.size();
			buffer.resize(at + GRASS_FLOATS);
			float *w = buffer.ptr() + at;
			write_transform(w, basis, position);
			const float bright = 1.0f + (rv * 2.0f - 1.0f) * variation;
			Color c(bright, bright, bright);
			if (tint) {
				// Grass takes the ground's tint, and darkens where it is wet.
				const float wet = 1.0f - 0.35f * sample.color.a;
				c.r *= sample.color.r * wet;
				c.g *= sample.color.g * wet;
				c.b *= sample.color.b * wet;
			}
			w[12] = c.r;
			w[13] = c.g;
			w[14] = c.b;
			w[15] = 1.0f;
			// The ground's normal, which the built-in grass shades with.
			w[16] = sample.normal.x;
			w[17] = sample.normal.y;
			w[18] = sample.normal.z;
			w[19] = rv;
		}
	}

	const int count = buffer.size() / GRASS_FLOATS;
	if (count == 0) {
		return;
	}

	RenderingServer *rs = RenderingServer::get_singleton();
	Vector<float> packed;
	packed.resize(buffer.size());
	memcpy(packed.ptrw(), buffer.ptr(), buffer.size() * sizeof(float));
	cell.multimesh = rs->multimesh_create();
	rs->multimesh_set_mesh(cell.multimesh, mesh->get_rid());
	rs->multimesh_allocate_data(cell.multimesh, count, RSE::MULTIMESH_TRANSFORM_3D, true, true);
	rs->multimesh_set_buffer(cell.multimesh, packed);
	cell.count = count;
	cell.origin = p_terrain->get_global_position();

	cell.instance = rs->instance_create2(cell.multimesh, get_world_3d()->get_scenario());
	rs->instance_set_transform(cell.instance, Transform3D(Basis(), cell.origin));
	rs->instance_set_visible(cell.instance, is_visible_in_tree());
	rs->instance_attach_object_instance_id(cell.instance, get_instance_id());
	rs->instance_geometry_set_cast_shadows_setting(cell.instance, type->is_cast_shadow() ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
	const Ref<Material> material = type->get_render_material();
	if (material.is_valid()) {
		rs->instance_geometry_set_material_override(cell.instance, material->get_rid());
	}
	const float range = type->get_visibility_range() > 0.0f ? MIN(type->get_visibility_range(), MAX_GRASS_RANGE) : DEFAULT_GRASS_RANGE;
	rs->instance_geometry_set_visibility_range(cell.instance, 0.0, range + s, 0.0, 0.0, RSE::VISIBILITY_RANGE_FADE_DISABLED);
	if (type->uses_builtin_grass()) {
		rs->instance_geometry_set_shader_parameter(cell.instance, SNAME("fade_distance"), range);
	}
}

bool Foliage3D::compute_placement(int p_type, const Vector2 &p_local, float p_yaw, float p_scale, float p_variation, float p_height_offset, Transform3D &r_local_transform, bool p_check_rules) const {
	const Terrain3D *terrain = _peek_terrain();
	if (!terrain || data.is_null() || terrain->get_data().is_null()) {
		return false;
	}
	const Ref<FoliageType> type = data->get_type(p_type);
	if (type.is_null()) {
		return false;
	}
	Terrain3D::SurfaceSample sample;
	if (!terrain->sample_surface(p_local, sample)) {
		return false;
	}
	if (p_check_rules && !type->accepts(slope_degrees(sample.normal), sample.weights, terrain->get_data()->get_layer_count())) {
		return false;
	}
	const Basis basis = placement_basis(type.ptr(), sample.normal, p_yaw, p_scale, p_variation);
	r_local_transform = Transform3D(basis, Vector3(p_local.x, sample.height + p_height_offset + type->get_height_offset(), p_local.y));
	return true;
}

void Foliage3D::_rebuild_object_type(Terrain3D *p_terrain, int p_type) {
	LocalVector<Vector3i> doomed;
	for (const KeyValue<Vector3i, Cell> &E : object_cells) {
		if (E.key.z == p_type) {
			doomed.push_back(E.key);
		}
	}
	for (const Vector3i &key : doomed) {
		_free_cell(object_cells[key]);
		object_cells.erase(key);
		dirty_objects.erase(key);
	}
	if (p_type >= data->get_type_count()) {
		return;
	}
	const Ref<FoliageType> type = data->get_type(p_type);
	if (type.is_null() || type->get_kind() != FoliageType::KIND_OBJECT) {
		return;
	}
	const FoliageData::TypeData *td = data->get_type_data(p_type);
	const float *src = td->instances.ptr();
	const int count = td->instances.size() / FoliageData::INSTANCE_STRIDE;
	HashMap<Vector2i, LocalVector<int>> buckets;
	for (int i = 0; i < count; i++) {
		const float *inst = src + i * FoliageData::INSTANCE_STRIDE;
		const Vector2i cell = _cell_of(Vector2(inst[0], inst[1]), object_cell_size);
		LocalVector<int> *list = buckets.getptr(cell);
		if (!list) {
			list = &buckets.insert(cell, LocalVector<int>())->value;
		}
		list->push_back(i);
	}
	for (const KeyValue<Vector2i, LocalVector<int>> &E : buckets) {
		_build_object_cell(p_terrain, Vector3i(E.key.x, E.key.y, p_type), E.value);
	}
}

void Foliage3D::_rebuild_object_cell(Terrain3D *p_terrain, const Vector3i &p_key) {
	Cell *existing = object_cells.getptr(p_key);
	if (existing) {
		_free_cell(*existing);
		object_cells.erase(p_key);
	}
	if (p_key.z >= data->get_type_count()) {
		return;
	}
	const FoliageData::TypeData *td = data->get_type_data(p_key.z);
	const float *src = td->instances.ptr();
	const int count = td->instances.size() / FoliageData::INSTANCE_STRIDE;
	LocalVector<int> indices;
	for (int i = 0; i < count; i++) {
		const float *inst = src + i * FoliageData::INSTANCE_STRIDE;
		if (_cell_of(Vector2(inst[0], inst[1]), object_cell_size) == Vector2i(p_key.x, p_key.y)) {
			indices.push_back(i);
		}
	}
	if (!indices.is_empty()) {
		_build_object_cell(p_terrain, p_key, indices);
	}
}

void Foliage3D::_build_object_cell(Terrain3D *p_terrain, const Vector3i &p_key, const LocalVector<int> &p_instances) {
	const Ref<FoliageType> type = data->get_type(p_key.z);
	if (type.is_null() || type->get_kind() != FoliageType::KIND_OBJECT) {
		return;
	}
	const FoliageData::TypeData *td = data->get_type_data(p_key.z);
	const float *src = td->instances.ptr();

	Cell cell;
	cell.origin = p_terrain->get_global_position();
	LocalVector<Transform3D> transforms;
	for (const int i : p_instances) {
		const float *inst = src + i * FoliageData::INSTANCE_STRIDE;
		Transform3D xform;
		// The rules were checked when the object was placed. Rechecking them
		// here would make objects vanish when a nearby edit tips the slope.
		if (compute_placement(p_key.z, Vector2(inst[0], inst[1]), inst[3], inst[4], inst[5], inst[2], xform, false)) {
			transforms.push_back(xform);
		}
	}
	cell.count = transforms.size();
	if (transforms.is_empty()) {
		object_cells.insert(p_key, cell);
		return;
	}

	const Ref<PackedScene> scene = type->get_scene();
	if (scene.is_valid()) {
		for (const Transform3D &xform : transforms) {
			Node *node = scene->instantiate();
			Node3D *node_3d = Object::cast_to<Node3D>(node);
			if (!node_3d) {
				if (node) {
					memdelete(node);
				}
				ERR_PRINT_ONCE("A foliage scene's root must be a Node3D.");
				break;
			}
			// Internal and ownerless, so the instances never land in the saved
			// scene or the Scene dock.
			node_3d->set_as_top_level(true);
			add_child(node_3d, false, INTERNAL_MODE_BACK);
			node_3d->set_global_transform(Transform3D(xform.basis, cell.origin + xform.origin));
			cell.scenes.push_back(node_3d->get_instance_id());
		}
		object_cells.insert(p_key, cell);
		return;
	}

	const Ref<Mesh> mesh = type->get_render_mesh();
	if (mesh.is_valid()) {
		RenderingServer *rs = RenderingServer::get_singleton();
		Vector<float> buffer;
		buffer.resize(transforms.size() * OBJECT_FLOATS);
		float *w = buffer.ptrw();
		for (uint32_t i = 0; i < transforms.size(); i++) {
			write_transform(w + i * OBJECT_FLOATS, transforms[i].basis, transforms[i].origin);
		}
		cell.multimesh = rs->multimesh_create();
		rs->multimesh_set_mesh(cell.multimesh, mesh->get_rid());
		rs->multimesh_allocate_data(cell.multimesh, transforms.size(), RSE::MULTIMESH_TRANSFORM_3D, false, false);
		rs->multimesh_set_buffer(cell.multimesh, buffer);
		cell.instance = rs->instance_create2(cell.multimesh, get_world_3d()->get_scenario());
		rs->instance_set_transform(cell.instance, Transform3D(Basis(), cell.origin));
		rs->instance_set_visible(cell.instance, is_visible_in_tree());
		rs->instance_attach_object_instance_id(cell.instance, get_instance_id());
		rs->instance_geometry_set_cast_shadows_setting(cell.instance, type->is_cast_shadow() ? RSE::SHADOW_CASTING_SETTING_ON : RSE::SHADOW_CASTING_SETTING_OFF);
		const Ref<Material> material = type->get_render_material();
		if (material.is_valid()) {
			rs->instance_geometry_set_material_override(cell.instance, material->get_rid());
		}
		if (type->get_visibility_range() > 0.0f) {
			rs->instance_geometry_set_visibility_range(cell.instance, 0.0, type->get_visibility_range() + object_cell_size * 0.5f, 0.0, 0.0, RSE::VISIBILITY_RANGE_FADE_DISABLED);
		}
	}

#ifndef PHYSICS_3D_DISABLED
	const Ref<Shape3D> shape = type->get_collision_shape();
	if (collision_enabled && shape.is_valid()) {
		PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
		cell.body = ps->body_create();
		ps->body_set_mode(cell.body, PhysicsServer3D::BODY_MODE_STATIC);
		ps->body_attach_object_instance_id(cell.body, get_instance_id());
		ps->body_set_collision_layer(cell.body, collision_layer);
		ps->body_set_collision_mask(cell.body, collision_mask);
		ps->body_set_state(cell.body, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D(Basis(), cell.origin));
		for (const Transform3D &xform : transforms) {
			ps->body_add_shape(cell.body, shape->get_rid(), xform);
		}
		ps->body_set_space(cell.body, get_world_3d()->get_space());
	}
#endif

	object_cells.insert(p_key, cell);
}

void Foliage3D::_update_transforms() {
	const Terrain3D *terrain = _peek_terrain();
	if (!terrain) {
		return;
	}
	const Vector3 origin = terrain->get_global_position();
	RenderingServer *rs = RenderingServer::get_singleton();
	const auto move = [&](Cell &r_cell) {
		if (r_cell.origin == origin) {
			return;
		}
		const Vector3 delta = origin - r_cell.origin;
		r_cell.origin = origin;
		if (r_cell.instance.is_valid()) {
			rs->instance_set_transform(r_cell.instance, Transform3D(Basis(), origin));
		}
#ifndef PHYSICS_3D_DISABLED
		if (r_cell.body.is_valid()) {
			PhysicsServer3D::get_singleton()->body_set_state(r_cell.body, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D(Basis(), origin));
		}
#endif
		for (const ObjectID &id : r_cell.scenes) {
			Node3D *node = ObjectDB::get_instance<Node3D>(id);
			if (node) {
				node->set_global_position(node->get_global_position() + delta);
			}
		}
	};
	for (KeyValue<Vector3i, Cell> &E : grass_cells) {
		move(E.value);
	}
	for (KeyValue<Vector3i, Cell> &E : object_cells) {
		move(E.value);
	}
}

Vector<Transform3D> Foliage3D::compute_object_transforms(int p_type) const {
	Vector<Transform3D> result;
	const Terrain3D *terrain = _peek_terrain();
	if (!terrain || data.is_null() || p_type < 0 || p_type >= data->get_type_count()) {
		return result;
	}
	const Vector3 origin = terrain->get_global_position();
	const FoliageData::TypeData *td = data->get_type_data(p_type);
	const float *src = td->instances.ptr();
	const int count = td->instances.size() / FoliageData::INSTANCE_STRIDE;
	for (int i = 0; i < count; i++) {
		const float *inst = src + i * FoliageData::INSTANCE_STRIDE;
		Transform3D xform;
		if (compute_placement(p_type, Vector2(inst[0], inst[1]), inst[3], inst[4], inst[5], inst[2], xform, false)) {
			xform.origin += origin;
			result.push_back(xform);
		}
	}
	return result;
}

/* -------------------------------------------------------------------------- */
/*                                  Settings                                  */
/* -------------------------------------------------------------------------- */

void Foliage3D::set_terrain_path(const NodePath &p_path) {
	terrain_path = p_path;
	layout_dirty = true;
	update_configuration_warnings();
}

void Foliage3D::set_grass_cell_size(float p_size) {
	grass_cell_size = CLAMP(p_size, 2.0f, 256.0f);
	layout_dirty = true;
}

void Foliage3D::set_object_cell_size(float p_size) {
	object_cell_size = CLAMP(p_size, 4.0f, 1024.0f);
	layout_dirty = true;
}

void Foliage3D::set_generation_budget_msec(float p_msec) {
	generation_budget_msec = CLAMP(p_msec, 0.1f, 100.0f);
}

void Foliage3D::set_collision_enabled(bool p_enabled) {
	collision_enabled = p_enabled;
	layout_dirty = true;
	notify_property_list_changed();
}

void Foliage3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
#ifndef PHYSICS_3D_DISABLED
	for (const KeyValue<Vector3i, Cell> &E : object_cells) {
		if (E.value.body.is_valid()) {
			PhysicsServer3D::get_singleton()->body_set_collision_layer(E.value.body, collision_layer);
		}
	}
#endif
}

void Foliage3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
#ifndef PHYSICS_3D_DISABLED
	for (const KeyValue<Vector3i, Cell> &E : object_cells) {
		if (E.value.body.is_valid()) {
			PhysicsServer3D::get_singleton()->body_set_collision_mask(E.value.body, collision_mask);
		}
	}
#endif
}

PackedStringArray Foliage3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (is_inside_tree() && !Object::cast_to<Terrain3D>(get_node_or_null(terrain_path))) {
		warnings.push_back(RTR("Foliage3D needs a Terrain3D to grow on. Make it a child of one, or point terrain_path at one."));
	}
	if (data.is_null()) {
		warnings.push_back(RTR("This foliage has no data. Add a grass or object type in the Terrain dock's foliage list, or assign a FoliageData."));
	}
	return warnings;
}

void Foliage3D::_validate_property(PropertyInfo &p_property) const {
	if ((p_property.name == "collision_layer" || p_property.name == "collision_mask") && !collision_enabled) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

/* -------------------------------------------------------------------------- */
/*                                 Navigation                                 */
/* -------------------------------------------------------------------------- */

void Foliage3D::navmesh_parse_init() {
#ifndef NAVIGATION_3D_DISABLED
	ERR_FAIL_NULL(NavigationServer3D::get_singleton());
	if (!_navmesh_source_geometry_parser.is_valid()) {
		_navmesh_source_geometry_parsing_callback = callable_mp_static(&Foliage3D::navmesh_parse_source_geometry);
		_navmesh_source_geometry_parser = NavigationServer3D::get_singleton()->source_geometry_parser_create();
		NavigationServer3D::get_singleton()->source_geometry_parser_set_callback(_navmesh_source_geometry_parser, _navmesh_source_geometry_parsing_callback);
	}
#endif
}

#if !defined(NAVIGATION_3D_DISABLED) && !defined(PHYSICS_3D_DISABLED)
static void add_shape_to_navigation(const RID &p_shape, const Transform3D &p_xform, Ref<NavigationMeshSourceGeometryData3D> &p_source) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	const Variant data = ps->shape_get_data(p_shape);
	Array arr;
	arr.resize(RSE::ARRAY_MAX);
	switch (ps->shape_get_type(p_shape)) {
		case PhysicsServer3D::SHAPE_BOX: {
			BoxMesh::create_mesh_array(arr, Vector3(data) * 2.0);
			p_source->add_mesh_array(arr, p_xform);
		} break;
		case PhysicsServer3D::SHAPE_SPHERE: {
			const real_t radius = data;
			SphereMesh::create_mesh_array(arr, radius, radius * 2.0);
			p_source->add_mesh_array(arr, p_xform);
		} break;
		case PhysicsServer3D::SHAPE_CAPSULE: {
			const Dictionary d = data;
			CapsuleMesh::create_mesh_array(arr, d["radius"], d["height"]);
			p_source->add_mesh_array(arr, p_xform);
		} break;
		case PhysicsServer3D::SHAPE_CYLINDER: {
			const Dictionary d = data;
			CylinderMesh::create_mesh_array(arr, d["radius"], d["radius"], d["height"]);
			p_source->add_mesh_array(arr, p_xform);
		} break;
		case PhysicsServer3D::SHAPE_CONVEX_POLYGON: {
			const PackedVector3Array points = data;
			Geometry3D::MeshData md;
			if (ConvexHullComputer::convex_hull(points, md) == OK) {
				PackedVector3Array faces;
				for (const Geometry3D::MeshData::Face &face : md.faces) {
					for (uint32_t k = 2; k < face.indices.size(); ++k) {
						faces.push_back(md.vertices[face.indices[0]]);
						faces.push_back(md.vertices[face.indices[k - 1]]);
						faces.push_back(md.vertices[face.indices[k]]);
					}
				}
				p_source->add_faces(faces, p_xform);
			}
		} break;
		case PhysicsServer3D::SHAPE_CONCAVE_POLYGON: {
			const Dictionary d = data;
			p_source->add_faces(PackedVector3Array(d["faces"]), p_xform);
		} break;
		default:
			break;
	}
}
#endif

void Foliage3D::navmesh_parse_source_geometry(const Ref<NavigationMesh> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData3D> p_source_geometry_data, Node *p_node) {
#ifndef NAVIGATION_3D_DISABLED
	Foliage3D *foliage = Object::cast_to<Foliage3D>(p_node);
	if (foliage == nullptr || foliage->data.is_null()) {
		return;
	}
	const NavigationMesh::ParsedGeometryType parsed = p_navigation_mesh->get_parsed_geometry_type();
	const bool meshes = parsed == NavigationMesh::PARSED_GEOMETRY_MESH_INSTANCES || parsed == NavigationMesh::PARSED_GEOMETRY_BOTH;
	for (int t = 0; t < foliage->data->get_type_count(); t++) {
		const Ref<FoliageType> type = foliage->data->get_type(t);
		// Grass is walked through, not around. Scenes are parsed like any
		// other children, since the parser descends into internal nodes too.
		if (type.is_null() || type->get_kind() != FoliageType::KIND_OBJECT || type->get_scene().is_valid()) {
			continue;
		}
		const Vector<Transform3D> transforms = foliage->compute_object_transforms(t);
		if (meshes) {
			const Ref<Mesh> mesh = type->get_render_mesh();
			if (mesh.is_valid()) {
				for (const Transform3D &xform : transforms) {
					p_source_geometry_data->add_mesh(mesh, xform);
				}
			}
		}
#ifndef PHYSICS_3D_DISABLED
		else if (foliage->collision_enabled && (foliage->collision_layer & p_navigation_mesh->get_collision_mask()) && type->get_collision_shape().is_valid()) {
			const RID shape = type->get_collision_shape()->get_rid();
			for (const Transform3D &xform : transforms) {
				add_shape_to_navigation(shape, xform, p_source_geometry_data);
			}
		}
#endif
	}
#endif
}

/* -------------------------------------------------------------------------- */
/*                                  Bindings                                  */
/* -------------------------------------------------------------------------- */

void Foliage3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_terrain_path", "path"), &Foliage3D::set_terrain_path);
	ClassDB::bind_method(D_METHOD("get_terrain_path"), &Foliage3D::get_terrain_path);
	ClassDB::bind_method(D_METHOD("set_data", "data"), &Foliage3D::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &Foliage3D::get_data);
	ClassDB::bind_method(D_METHOD("set_grass_cell_size", "size"), &Foliage3D::set_grass_cell_size);
	ClassDB::bind_method(D_METHOD("get_grass_cell_size"), &Foliage3D::get_grass_cell_size);
	ClassDB::bind_method(D_METHOD("set_object_cell_size", "size"), &Foliage3D::set_object_cell_size);
	ClassDB::bind_method(D_METHOD("get_object_cell_size"), &Foliage3D::get_object_cell_size);
	ClassDB::bind_method(D_METHOD("set_generation_budget_msec", "msec"), &Foliage3D::set_generation_budget_msec);
	ClassDB::bind_method(D_METHOD("get_generation_budget_msec"), &Foliage3D::get_generation_budget_msec);
	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &Foliage3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &Foliage3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &Foliage3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &Foliage3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Foliage3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Foliage3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("regenerate"), &Foliage3D::regenerate);

	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "terrain_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Terrain3D"), "set_terrain_path", "get_terrain_path");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, "FoliageData"), "set_data", "get_data");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grass_cell_size", PROPERTY_HINT_RANGE, "2,256,0.1,suffix:m"), "set_grass_cell_size", "get_grass_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "object_cell_size", PROPERTY_HINT_RANGE, "4,1024,0.1,suffix:m"), "set_object_cell_size", "get_object_cell_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "generation_budget_msec", PROPERTY_HINT_RANGE, "0.1,100,0.1,suffix:ms"), "set_generation_budget_msec", "get_generation_budget_msec");

	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
}
