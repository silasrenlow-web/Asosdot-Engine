/**************************************************************************/
/*  voxel_terrain_3d.cpp                                                  */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "voxel_terrain_3d.h"

#include "terrain_shaders.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/3d/world_3d.h"
#include "scene/resources/mesh.h"
#include "servers/rendering/rendering_server.h"

#ifndef PHYSICS_3D_DISABLED
#include "servers/physics_3d/physics_server_3d.h"
#endif

#ifndef NAVIGATION_3D_DISABLED
#include "scene/resources/3d/navigation_mesh_source_geometry_data_3d.h"
#include "scene/resources/navigation_mesh.h"
#include "servers/navigation_3d/navigation_server_3d.h"

Callable VoxelTerrain3D::_navmesh_source_geometry_parsing_callback;
RID VoxelTerrain3D::_navmesh_source_geometry_parser;
#endif

Vector3 VoxelTerrain3D::editor_viewer;
bool VoxelTerrain3D::has_editor_viewer = false;

namespace {

RID shared_shader;
int shared_shader_users = 0;

// Finished meshes are handed to the renderer for at most this long a frame;
// the rest wait for the next one.
constexpr uint64_t APPLY_BUDGET_USEC = 4000;

constexpr int CELLS = VoxelMesher::CELLS;
constexpr int SAMPLES = VoxelMesher::SAMPLES;

uint64_t custom_format() {
	return ((uint64_t)RSE::ARRAY_CUSTOM_RGBA8_UNORM << RSE::ARRAY_FORMAT_CUSTOM0_SHIFT) |
			((uint64_t)RSE::ARRAY_CUSTOM_RGBA8_UNORM << RSE::ARRAY_FORMAT_CUSTOM1_SHIFT) |
			((uint64_t)RSE::ARRAY_CUSTOM_RGBA8_UNORM << RSE::ARRAY_FORMAT_CUSTOM2_SHIFT) |
			((uint64_t)RSE::ARRAY_CUSTOM_RGBA8_UNORM << RSE::ARRAY_FORMAT_CUSTOM3_SHIFT);
}

Array make_arrays(const VoxelMesher::Output &p_output) {
	Array arrays;
	arrays.resize(RSE::ARRAY_MAX);
	arrays[RSE::ARRAY_VERTEX] = p_output.vertices;
	arrays[RSE::ARRAY_NORMAL] = p_output.normals;
	for (int g = 0; g < 4; g++) {
		arrays[RSE::ARRAY_CUSTOM0 + g] = p_output.weights[g];
	}
	arrays[RSE::ARRAY_INDEX] = p_output.indices;
	return arrays;
}

struct LayerLookup {
	const VoxelTerrainData *data = nullptr;
	Vector3i origin;
	int step = 1;
};

// The layer just under the surface at a vertex: the first voxel in the ground
// going down from it along the normal.
int layer_under(void *p_lookup, const Vector3 &p_cell_position, const Vector3 &p_normal) {
	const LayerLookup *lookup = (const LayerLookup *)p_lookup;
	const Vector3 at = Vector3(lookup->origin) + p_cell_position * lookup->step;
	for (float depth : { 0.5f, 1.5f, 2.5f }) {
		float sdf;
		uint8_t layer;
		lookup->data->read(Vector3i((at - p_normal * depth).round()), 1, Vector3i(1, 1, 1), &sdf, &layer);
		if (sdf < 0.0f) {
			return layer;
		}
	}
	return -1;
}

void mesh_block(const Ref<VoxelTerrainData> &p_data, const Vector3i &p_origin, int p_step, float p_skirt, bool p_collision, VoxelMesher::Output &r_output) {
	LocalVector<float> sdf;
	LocalVector<uint8_t> material;
	sdf.resize(VoxelMesher::SAMPLE_COUNT);
	material.resize(VoxelMesher::SAMPLE_COUNT);
	// One sample of margin all round, for normals across block edges.
	p_data->read(p_origin - Vector3i(p_step, p_step, p_step), p_step, Vector3i(SAMPLES, SAMPLES, SAMPLES), sdf.ptr(), material.ptr());
	VoxelMesher::Input input;
	input.sdf = sdf.ptr();
	input.material = material.ptr();
	input.cell_size = p_step * p_data->get_voxel_size();
	input.skirt = p_skirt;
	input.collision = p_collision;
	LayerLookup lookup;
	if (p_step > 1) {
		lookup.data = p_data.ptr();
		lookup.origin = p_origin;
		lookup.step = p_step;
		input.layer_at = &layer_under;
		input.layer_userdata = &lookup;
	}
	VoxelMesher::build(input, r_output);
}

} // namespace

/* -------------------------------------------------------------------------- */
/*                                  Lifecycle                                 */
/* -------------------------------------------------------------------------- */

VoxelTerrain3D::VoxelTerrain3D() {
	set_notify_transform(true);
}

VoxelTerrain3D::~VoxelTerrain3D() {
	_wait_for_jobs();
	_free_all();
	layer_textures.clear();
	if (material.is_valid()) {
		RenderingServer *rs = RenderingServer::get_singleton();
		rs->free_rid(material);
		shared_shader_users--;
		if (shared_shader_users == 0 && shared_shader.is_valid()) {
			rs->free_rid(shared_shader);
			shared_shader = RID();
		}
	}
}

Vector3 VoxelTerrain3D::_get_origin() const {
	return is_inside_tree() ? get_global_transform().origin : get_transform().origin;
}

float VoxelTerrain3D::_get_voxel_size() const {
	return data.is_valid() ? data->get_voxel_size() : 1.0f;
}

AABB VoxelTerrain3D::_chunk_box(const Vector4i &p_key) {
	const int size = CELLS << p_key.w;
	return AABB(Vector3(p_key.x, p_key.y, p_key.z) * size, Vector3(size, size, size));
}

void VoxelTerrain3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			set_process_internal(true);
		} break;

		case NOTIFICATION_ENTER_WORLD: {
			lod_dirty = true;
			layers_dirty = true;
			material_dirty = true;
			_process_updates();
		} break;

		case NOTIFICATION_EXIT_WORLD: {
			_wait_for_jobs();
			_free_all();
			lod_dirty = true;
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			_update_transforms();
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

void VoxelTerrain3D::set_data(const Ref<VoxelTerrainData> &p_data) {
	if (data == p_data) {
		return;
	}
	if (data.is_valid()) {
		data->disconnect(SNAME("changed_area"), callable_mp(this, &VoxelTerrain3D::_data_changed_area));
		data->disconnect(SNAME("generator_changed"), callable_mp(this, &VoxelTerrain3D::_data_generator_changed));
		data->disconnect(SNAME("layers_changed"), callable_mp(this, &VoxelTerrain3D::_data_layers_changed));
	}
	_wait_for_jobs();
	_free_all();
	data = p_data;
	if (data.is_valid()) {
		data->connect(SNAME("changed_area"), callable_mp(this, &VoxelTerrain3D::_data_changed_area));
		data->connect(SNAME("generator_changed"), callable_mp(this, &VoxelTerrain3D::_data_generator_changed));
		data->connect(SNAME("layers_changed"), callable_mp(this, &VoxelTerrain3D::_data_layers_changed));
	}
	lod_dirty = true;
	layers_dirty = true;
	update_configuration_warnings();
	emit_signal(SNAME("terrain_changed"), Rect2());
}

void VoxelTerrain3D::set_terrain_material(const Ref<TerrainMaterial> &p_material) {
	if (terrain_material == p_material) {
		return;
	}
	if (terrain_material.is_valid()) {
		terrain_material->disconnect_changed(callable_mp(this, &VoxelTerrain3D::_material_changed));
	}
	terrain_material = p_material;
	if (terrain_material.is_valid()) {
		terrain_material->connect_changed(callable_mp(this, &VoxelTerrain3D::_material_changed));
	}
	_material_changed();
}

void VoxelTerrain3D::_data_changed_area(const AABB &p_voxel_box) {
	for (KeyValue<Vector4i, Chunk> &E : chunks) {
		// A block reads a sample past each face, and normals one more.
		const int step = _chunk_step(E.key);
		if (_chunk_box(E.key).grow(step + 1).intersects(p_voxel_box)) {
			E.value.dirty = true;
		}
	}
	// An edit can make ground where there was none to mesh.
	lod_dirty = true;
	// Foliage listens for this, in metres like everything it stores.
	const float vs = _get_voxel_size();
	emit_signal(SNAME("terrain_changed"), Rect2(p_voxel_box.position.x * vs, p_voxel_box.position.z * vs, p_voxel_box.size.x * vs, p_voxel_box.size.z * vs).grow(vs));
}

void VoxelTerrain3D::_data_generator_changed() {
	_mark_all_dirty();
	update_configuration_warnings();
	// Everywhere may have changed.
	emit_signal(SNAME("terrain_changed"), Rect2());
}

void VoxelTerrain3D::_data_layers_changed() {
	layers_dirty = true;
}

void VoxelTerrain3D::_material_changed() {
	material_dirty = true;
}

void VoxelTerrain3D::_mark_all_dirty() {
	for (KeyValue<Vector4i, Chunk> &E : chunks) {
		E.value.dirty = true;
	}
	lod_dirty = true;
}

void VoxelTerrain3D::rebuild() {
	_mark_all_dirty();
}

/* -------------------------------------------------------------------------- */
/*                                   Updates                                  */
/* -------------------------------------------------------------------------- */

void VoxelTerrain3D::_process_updates() {
	if (!is_inside_world()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (material.is_null()) {
		if (shared_shader.is_null()) {
			shared_shader = rs->shader_create_from_code(voxel_shader_code(), "VoxelTerrain3D");
		}
		shared_shader_users++;
		material = rs->material_create();
		rs->material_set_shader(material, shared_shader);
		material_dirty = true;
	}
	if (layers_dirty) {
		layers_dirty = false;
		if (data.is_valid()) {
			layer_textures.update(data->get_layer_list());
		} else {
			layer_textures.clear();
		}
		const bool converted = layer_textures.is_converted();
		if (converted != (bool)get_meta(SNAME("_layer_textures_converted"), false)) {
			set_meta(SNAME("_layer_textures_converted"), converted ? Variant(true) : Variant());
			update_configuration_warnings();
		}
		material_dirty = true;
	}
	if (material_dirty) {
		material_dirty = false;
		_update_material_params();
	}

	if (data.is_null()) {
		if (!chunks.is_empty()) {
			_wait_for_jobs();
			_free_all();
		}
		return;
	}

	const Vector3 viewer = _get_viewer();
	if (lod_dirty || viewer.distance_to(last_viewer) > CELLS * 0.25f) {
		_update_lod(viewer);
		last_viewer = viewer;
		lod_dirty = false;
		retire_pending = true;
	}
	_finish_jobs();
	if (retire_pending) {
		_retire_chunks();
	}
	_start_jobs(viewer);

	const bool meshing = is_meshing();
	if (was_meshing && !meshing) {
		emit_signal(SNAME("meshing_finished"));
	}
	was_meshing = meshing;
}

void VoxelTerrain3D::_update_material_params() {
	if (material.is_null()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	layer_textures.set_layer_params(material, data.is_valid() ? data->get_layer_list() : Vector<Ref<TerrainLayer>>());
	TerrainLayerTextures::set_material_params(material, terrain_material);
	rs->material_set_param(material, SNAME("debug_view"), (int)debug_view);
	rs->material_set_param(material, SNAME("brush_enabled"), brush_visible);
	rs->material_set_param(material, SNAME("brush_position"), brush_position);
	rs->material_set_param(material, SNAME("brush_radius"), brush_radius);
	rs->material_set_param(material, SNAME("brush_hardness"), brush_hardness);
	rs->material_set_param(material, SNAME("brush_color"), brush_color);
}

Vector3 VoxelTerrain3D::_get_viewer() const {
	Vector3 global = _get_origin();
	const Node3D *node = viewer_path.is_empty() ? nullptr : Object::cast_to<Node3D>(get_node_or_null(viewer_path));
	if (node && node->is_inside_tree()) {
		global = node->get_global_position();
	} else if (Engine::get_singleton()->is_editor_hint() && has_editor_viewer) {
		global = editor_viewer;
	} else if (is_inside_tree() && get_viewport() && get_viewport()->get_camera_3d()) {
		global = get_viewport()->get_camera_3d()->get_global_position();
	}
	return global_to_voxel(global);
}

void VoxelTerrain3D::_select(const Vector4i &p_key, const Vector3 &p_viewer, HashMap<Vector4i, bool> &r_wanted) const {
	const AABB box = _chunk_box(p_key);
	const float vs = _get_voxel_size();
	const Vector3 closest = p_viewer.clamp(box.position, box.get_end());
	const float distance = closest.distance_to(p_viewer) * vs;
	if (distance > view_distance) {
		return;
	}
	// Skip blocks with nothing in them: all sky, or all rock with no caves.
	// A block reads a sample past each face, so look that far too.
	const int step = _chunk_step(p_key);
	const AABB padded = box.grow(step);
	const Ref<VoxelGenerator> generator = data->get_generator();
	bool solid = false;
	if (!(generator.is_valid() && generator->may_contain_surface(padded, vs, solid)) && !data->has_edits_in(padded)) {
		return;
	}

	const int lod = p_key.w;
	const bool collide = collision_enabled && distance <= collision_distance;
	// Each level of detail covers twice the distance of the one before, so
	// neighbouring blocks never differ by more than one level.
	if (lod > 0 && (distance < lod_distance * float(1 << (lod - 1)) || collide)) {
		for (int i = 0; i < 8; i++) {
			_select(Vector4i(p_key.x * 2 + (i & 1), p_key.y * 2 + ((i >> 1) & 1), p_key.z * 2 + ((i >> 2) & 1), lod - 1), p_viewer, r_wanted);
		}
		return;
	}
	r_wanted.insert(p_key, collide && lod == 0);
}

void VoxelTerrain3D::_update_lod(const Vector3 &p_viewer) {
	HashMap<Vector4i, bool> wanted;
	const float vs = _get_voxel_size();
	const float reach = view_distance / vs;
	int top = 0;
	while (top < MAX_LOD && float(CELLS << top) < reach) {
		top++;
	}
	const int size = CELLS << top;
	const Vector3i lo(VoxelTerrainData::floor_div((int)Math::floor(p_viewer.x - reach), size), VoxelTerrainData::floor_div((int)Math::floor(p_viewer.y - reach), size), VoxelTerrainData::floor_div((int)Math::floor(p_viewer.z - reach), size));
	const Vector3i hi(VoxelTerrainData::floor_div((int)Math::ceil(p_viewer.x + reach), size), VoxelTerrainData::floor_div((int)Math::ceil(p_viewer.y + reach), size), VoxelTerrainData::floor_div((int)Math::ceil(p_viewer.z + reach), size));
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				_select(Vector4i(x, y, z, top), p_viewer, wanted);
			}
		}
	}

	static uint32_t next_generation = 1;
	for (KeyValue<Vector4i, Chunk> &E : chunks) {
		E.value.wanted = false;
	}
	for (const KeyValue<Vector4i, bool> &E : wanted) {
		Chunk *chunk = chunks.getptr(E.key);
		if (!chunk) {
			chunk = &chunks.insert(E.key, Chunk())->value;
			chunk->generation = next_generation++;
		}
		chunk->wanted = true;
		chunk->wants_collision = E.value;
		if (chunk->has_collision && !chunk->wants_collision) {
			// Out of range: drop the shape, keep the mesh.
			_free_collision(*chunk);
		} else if (chunk->ready && chunk->wants_collision && !chunk->has_collision) {
			chunk->dirty = true;
		}
	}
}

void VoxelTerrain3D::_start_jobs(const Vector3 &p_viewer) {
	const int max_jobs = MAX(1, WorkerThreadPool::get_singleton()->get_thread_count() - 1);
	if ((int)jobs.size() >= max_jobs) {
		return;
	}
	struct Candidate {
		Vector4i key;
		float priority = 0.0;
		bool operator<(const Candidate &p_other) const { return priority < p_other.priority; }
	};
	LocalVector<Candidate> candidates;
	for (const KeyValue<Vector4i, Chunk> &E : chunks) {
		const Chunk &chunk = E.value;
		if (!chunk.wanted || !chunk.dirty || chunk.building) {
			continue;
		}
		const AABB box = _chunk_box(E.key);
		Candidate c;
		c.key = E.key;
		// Nearest first, in block sizes so a far coarse block counts as near
		// as the fine one beside the viewer. Filling a hole beats refreshing
		// a block that already has a mesh.
		c.priority = box.get_center().distance_to(p_viewer) / box.size.x + (chunk.ready ? 1000.0f : 0.0f);
		candidates.push_back(c);
	}
	candidates.sort();

	for (const Candidate &c : candidates) {
		if ((int)jobs.size() >= max_jobs) {
			break;
		}
		Chunk &chunk = chunks[c.key];
		Job *job = memnew(Job);
		job->key = c.key;
		job->generation = chunk.generation;
		job->data = data;
		job->step = _chunk_step(c.key);
		job->origin = Vector3i(c.key.x, c.key.y, c.key.z) * (CELLS << c.key.w);
		job->voxel_size = data->get_voxel_size();
		job->collision = chunk.wants_collision;
		job->task = WorkerThreadPool::get_singleton()->add_native_task(&Job::run, job, false, "VoxelTerrain3D");
		jobs.push_back(job);
		chunk.building = true;
		chunk.dirty = false;
	}
}

void VoxelTerrain3D::Job::run(void *p_job) {
	Job *job = (Job *)p_job;
	// Skirts hang two cells: enough for the crack against the next level,
	// whose cells are twice the size.
	const float skirt = job->step * job->voxel_size * 2.0f;
	mesh_block(job->data, job->origin, job->step, skirt, job->collision, job->output);
}

void VoxelTerrain3D::_finish_jobs() {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	const uint64_t start = OS::get_singleton()->get_ticks_usec();
	for (uint32_t i = 0; i < jobs.size();) {
		Job *job = jobs[i];
		if (!pool->is_task_completed(job->task)) {
			i++;
			continue;
		}
		pool->wait_for_task_completion(job->task);
		Chunk *chunk = chunks.getptr(job->key);
		if (chunk && chunk->generation == job->generation) {
			chunk->building = false;
			_apply(job->key, *chunk, *job);
			chunk->ready = true;
			retire_pending = true;
		}
		memdelete(job);
		jobs.remove_at_unordered(i);
		if (OS::get_singleton()->get_ticks_usec() - start > APPLY_BUDGET_USEC) {
			break;
		}
	}
}

void VoxelTerrain3D::_wait_for_jobs() {
	WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
	for (Job *job : jobs) {
		pool->wait_for_task_completion(job->task);
		Chunk *chunk = chunks.getptr(job->key);
		if (chunk && chunk->generation == job->generation) {
			chunk->building = false;
			chunk->dirty = true;
		}
		memdelete(job);
	}
	jobs.clear();
}

void VoxelTerrain3D::_apply(const Vector4i &p_key, Chunk &r_chunk, Job &p_job) {
	RenderingServer *rs = RenderingServer::get_singleton();
	const VoxelMesher::Output &out = p_job.output;
	if (out.indices.is_empty()) {
		if (r_chunk.instance.is_valid()) {
			rs->free_rid(r_chunk.instance);
			r_chunk.instance = RID();
		}
		if (r_chunk.mesh.is_valid()) {
			rs->free_rid(r_chunk.mesh);
			r_chunk.mesh = RID();
		}
	} else {
		if (r_chunk.mesh.is_null()) {
			r_chunk.mesh = rs->mesh_create();
		} else {
			rs->mesh_clear(r_chunk.mesh);
		}
		rs->mesh_add_surface_from_arrays(r_chunk.mesh, RSE::PRIMITIVE_TRIANGLES, make_arrays(out), Array(), Dictionary(), custom_format());
		rs->mesh_surface_set_material(r_chunk.mesh, 0, material);
		if (r_chunk.instance.is_null()) {
			r_chunk.instance = rs->instance_create2(r_chunk.mesh, get_world_3d()->get_scenario());
			rs->instance_attach_object_instance_id(r_chunk.instance, get_instance_id());
			rs->instance_geometry_set_flag(r_chunk.instance, RSE::INSTANCE_FLAG_USE_BAKED_LIGHT, true);
			rs->instance_set_transform(r_chunk.instance, Transform3D(Basis(), _get_origin() + _chunk_box(p_key).position * _get_voxel_size()));
			_update_instance_settings(p_key, r_chunk);
		}
	}

#ifndef PHYSICS_3D_DISABLED
	if (p_job.collision && collision_enabled && !out.collision_faces.is_empty()) {
		PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
		if (r_chunk.shape.is_null()) {
			r_chunk.shape = ps->concave_polygon_shape_create();
		}
		Dictionary d;
		d["faces"] = out.collision_faces;
		d["backface_collision"] = false;
		ps->shape_set_data(r_chunk.shape, d);
		if (r_chunk.body.is_null()) {
			r_chunk.body = ps->body_create();
			ps->body_set_mode(r_chunk.body, PhysicsServer3D::BODY_MODE_STATIC);
			ps->body_attach_object_instance_id(r_chunk.body, get_instance_id());
			ps->body_add_shape(r_chunk.body, r_chunk.shape);
			ps->body_set_space(r_chunk.body, get_world_3d()->get_space());
			ps->body_set_state(r_chunk.body, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D(Basis(), _get_origin() + _chunk_box(p_key).position * _get_voxel_size()));
			_update_body_settings(r_chunk);
		}
		r_chunk.has_collision = true;
	} else {
		_free_collision(r_chunk);
	}
#endif
}

void VoxelTerrain3D::_retire_chunks() {
	retire_pending = false;
	// A chunk that has dropped out of the selection stays until everything
	// overlapping it that replaces it has a mesh, so moving the camera never
	// opens a hole while the new blocks are being built.
	LocalVector<Vector4i> gone;
	for (const KeyValue<Vector4i, Chunk> &E : chunks) {
		if (E.value.wanted) {
			continue;
		}
		const AABB box = _chunk_box(E.key);
		bool covered = true;
		for (const KeyValue<Vector4i, Chunk> &W : chunks) {
			if (W.value.wanted && !W.value.ready && _chunk_box(W.key).intersects(box)) {
				covered = false;
				break;
			}
		}
		if (covered) {
			gone.push_back(E.key);
		}
	}
	for (const Vector4i &key : gone) {
		_free_chunk(chunks[key]);
		chunks.erase(key);
	}
}

void VoxelTerrain3D::_free_collision(Chunk &r_chunk) {
#ifndef PHYSICS_3D_DISABLED
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (r_chunk.body.is_valid()) {
		ps->free_rid(r_chunk.body);
		r_chunk.body = RID();
	}
	if (r_chunk.shape.is_valid()) {
		ps->free_rid(r_chunk.shape);
		r_chunk.shape = RID();
	}
#endif
	r_chunk.has_collision = false;
}

void VoxelTerrain3D::_free_chunk(Chunk &r_chunk) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (r_chunk.instance.is_valid()) {
		rs->free_rid(r_chunk.instance);
		r_chunk.instance = RID();
	}
	if (r_chunk.mesh.is_valid()) {
		rs->free_rid(r_chunk.mesh);
		r_chunk.mesh = RID();
	}
	_free_collision(r_chunk);
}

void VoxelTerrain3D::_free_all() {
	for (KeyValue<Vector4i, Chunk> &E : chunks) {
		_free_chunk(E.value);
	}
	chunks.clear();
	was_meshing = false;
}

void VoxelTerrain3D::_update_instance_settings(const Vector4i &p_key, const Chunk &p_chunk) const {
	if (p_chunk.instance.is_null()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	static const RSE::ShadowCastingSetting shadow_modes[] = {
		RSE::SHADOW_CASTING_SETTING_OFF,
		RSE::SHADOW_CASTING_SETTING_ON,
		RSE::SHADOW_CASTING_SETTING_DOUBLE_SIDED,
		RSE::SHADOW_CASTING_SETTING_SHADOWS_ONLY,
	};
	rs->instance_geometry_set_cast_shadows_setting(p_chunk.instance, shadow_modes[cast_shadow]);
	rs->instance_set_layer_mask(p_chunk.instance, render_layers);
	rs->instance_set_visible(p_chunk.instance, is_visible_in_tree());
	rs->instance_geometry_set_shader_parameter(p_chunk.instance, SNAME("lod_level"), float(p_key.w));
}

void VoxelTerrain3D::_update_body_settings(const Chunk &p_chunk) const {
#ifndef PHYSICS_3D_DISABLED
	if (p_chunk.body.is_null()) {
		return;
	}
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	ps->body_set_collision_layer(p_chunk.body, collision_layer);
	ps->body_set_collision_mask(p_chunk.body, collision_mask);
	ps->body_set_collision_priority(p_chunk.body, collision_priority);
#endif
}

void VoxelTerrain3D::_update_transforms() {
	RenderingServer *rs = RenderingServer::get_singleton();
	const Vector3 origin = _get_origin();
	const float vs = _get_voxel_size();
	for (const KeyValue<Vector4i, Chunk> &E : chunks) {
		const Transform3D xform(Basis(), origin + _chunk_box(E.key).position * vs);
		if (E.value.instance.is_valid()) {
			rs->instance_set_transform(E.value.instance, xform);
		}
#ifndef PHYSICS_3D_DISABLED
		if (E.value.body.is_valid()) {
			PhysicsServer3D::get_singleton()->body_set_state(E.value.body, PhysicsServer3D::BODY_STATE_TRANSFORM, xform);
		}
#endif
	}
	lod_dirty = true;
	update_configuration_warnings();
}

void VoxelTerrain3D::_update_visibility() {
	RenderingServer *rs = RenderingServer::get_singleton();
	const bool visible = is_visible_in_tree();
	for (const KeyValue<Vector4i, Chunk> &E : chunks) {
		if (E.value.instance.is_valid()) {
			rs->instance_set_visible(E.value.instance, visible);
		}
	}
}

bool VoxelTerrain3D::is_meshing() const {
	for (const KeyValue<Vector4i, Chunk> &E : chunks) {
		if (E.value.wanted && (E.value.dirty || E.value.building)) {
			return true;
		}
	}
	return false;
}

int VoxelTerrain3D::get_chunk_count() const {
	int count = 0;
	for (const KeyValue<Vector4i, Chunk> &E : chunks) {
		count += E.value.instance.is_valid() ? 1 : 0;
	}
	return count;
}

/* -------------------------------------------------------------------------- */
/*                                   Queries                                  */
/* -------------------------------------------------------------------------- */

Vector3 VoxelTerrain3D::global_to_voxel(const Vector3 &p_global_position) const {
	return (p_global_position - _get_origin()) / _get_voxel_size();
}

Vector3 VoxelTerrain3D::voxel_to_global(const Vector3 &p_voxel_position) const {
	return _get_origin() + p_voxel_position * _get_voxel_size();
}

float VoxelTerrain3D::get_distance(const Vector3 &p_global_position) const {
	if (data.is_null()) {
		return Math::INF;
	}
	return data->sample_sdf(global_to_voxel(p_global_position)) * _get_voxel_size();
}

Vector3 VoxelTerrain3D::get_normal(const Vector3 &p_global_position) const {
	if (data.is_null()) {
		return Vector3(0, 1, 0);
	}
	return data->sample_normal(global_to_voxel(p_global_position));
}

int VoxelTerrain3D::get_layer(const Vector3 &p_global_position) const {
	if (data.is_null()) {
		return -1;
	}
	// The surface is made of whatever is under it: of the eight voxels
	// around a point just below it, the one deepest in the ground.
	const Vector3 v = global_to_voxel(p_global_position);
	const Vector3 below = v - data->sample_normal(v) * 0.5f;
	float sdf[8];
	uint8_t layers[8];
	data->read(Vector3i(below.floor()), 1, Vector3i(2, 2, 2), sdf, layers);
	int deepest = 0;
	for (int i = 1; i < 8; i++) {
		if (sdf[i] < sdf[deepest]) {
			deepest = i;
		}
	}
	return layers[deepest];
}

String VoxelTerrain3D::get_surface_name(const Vector3 &p_global_position) const {
	const int layer = get_layer(p_global_position);
	if (layer < 0 || layer >= data->get_layer_count()) {
		return String();
	}
	const Ref<TerrainLayer> l = data->get_layer(layer);
	return l.is_valid() ? l->get_display_name() : String();
}

bool VoxelTerrain3D::intersect_ray_fast(const Vector3 &p_from, const Vector3 &p_direction, float p_max_distance, Vector3 &r_position) const {
	if (data.is_null() || p_direction.is_zero_approx()) {
		return false;
	}
	const Vector3 dir = p_direction.normalized();
	const Vector3 from = global_to_voxel(p_from);
	const float max_t = p_max_distance / _get_voxel_size();

	// March along the ray by a fraction of the distance to the surface: the
	// generator's distances are only estimates on slopes, so it never takes
	// the whole of it. Then bisect the step that went underground.
	float prev_t = 0.0;
	float prev_sdf = data->sample_sdf(from);
	if (prev_sdf < 0.0f) {
		// Starting underground: there is nothing to hit from inside.
		return false;
	}
	float t = 0.0;
	for (int guard = 0; guard < 20000 && t < max_t; guard++) {
		t = MIN(t + CLAMP(prev_sdf * 0.5f, 0.1f, 4.0f), max_t);
		const float sdf = data->sample_sdf(from + dir * t);
		if (sdf < 0.0f) {
			float lo = prev_t;
			float hi = t;
			for (int i = 0; i < 16; i++) {
				const float mid = (lo + hi) * 0.5f;
				if (data->sample_sdf(from + dir * mid) < 0.0f) {
					hi = mid;
				} else {
					lo = mid;
				}
			}
			r_position = voxel_to_global(from + dir * hi);
			return true;
		}
		prev_t = t;
		prev_sdf = sdf;
	}
	return false;
}

bool VoxelTerrain3D::TopSurface::sample(const Vector2 &p_local, float &r_height, Vector3 &r_normal, int &r_layer) const {
	const Vector2 g = (p_local - origin) / spacing;
	const int x = (int)Math::floor(g.x);
	const int z = (int)Math::floor(g.y);
	if (x < 0 || z < 0 || x + 1 >= width || z + 1 >= depth) {
		return false;
	}
	const float fx = g.x - x;
	const float fz = g.y - z;
	const int i = z * width + x;
	const float h00 = heights[i];
	const float h10 = heights[i + 1];
	const float h01 = heights[i + width];
	const float h11 = heights[i + width + 1];
	if (Math::is_nan(h00) || Math::is_nan(h10) || Math::is_nan(h01) || Math::is_nan(h11)) {
		return false;
	}
	r_height = Math::lerp(Math::lerp(h00, h10, fx), Math::lerp(h01, h11, fx), fz);
	const float dx = Math::lerp(h10 - h00, h11 - h01, fz) / spacing;
	const float dz = Math::lerp(h01 - h00, h11 - h10, fx) / spacing;
	r_normal = Vector3(-dx, 1.0, -dz).normalized();
	r_layer = layers[(fz < 0.5f ? z : z + 1) * width + (fx < 0.5f ? x : x + 1)];
	return true;
}

void VoxelTerrain3D::get_top_surface(const Rect2 &p_local_area, TopSurface &r_surface) const {
	r_surface.width = 0;
	r_surface.depth = 0;
	r_surface.heights.clear();
	r_surface.layers.clear();
	if (data.is_null()) {
		return;
	}
	const float vs = _get_voxel_size();
	// A column of margin all round, so every point in the area has four.
	const Vector2i c0 = Vector2i((p_local_area.position / vs).floor()) - Vector2i(1, 1);
	const Vector2i c1 = Vector2i((p_local_area.get_end() / vs).ceil()) + Vector2i(1, 1);
	const Vector2i size = c1 - c0 + Vector2i(1, 1);
	r_surface.origin = Vector2(c0) * vs;
	r_surface.spacing = vs;
	r_surface.width = size.x;
	r_surface.depth = size.y;
	r_surface.heights.resize(size.x * size.y);
	r_surface.layers.resize(size.x * size.y);

	// Search from above the highest the ground can be to below the lowest:
	// the generator's range, widened by any edits over these columns.
	bool any = false;
	int top = 0;
	int bottom = 0;
	const Ref<VoxelGenerator> generator = data->get_generator();
	if (generator.is_valid()) {
		float lo;
		float hi;
		generator->get_surface_bounds(lo, hi);
		top = (int)Math::ceil(hi / vs) + 2;
		bottom = (int)Math::floor(lo / vs) - 2;
		any = true;
	}
	int edit_lo;
	int edit_hi;
	if (data->get_edited_span(Rect2i(c0, size), edit_lo, edit_hi)) {
		top = any ? MAX(top, edit_hi + 2) : edit_hi + 2;
		bottom = any ? MIN(bottom, edit_lo - 2) : edit_lo - 2;
		any = true;
	}
	if (!any) {
		for (float &h : r_surface.heights) {
			h = Math::NaN;
		}
		return;
	}
	data->find_top_surface(c0, size, top, bottom, r_surface.heights.ptr(), r_surface.layers.ptr());
	for (float &h : r_surface.heights) {
		h *= vs;
	}
}

bool VoxelTerrain3D::find_ground(const Vector3 &p_local, float p_range, float &r_height, Vector3 &r_normal, int &r_layer) const {
	if (data.is_null()) {
		return false;
	}
	const float vs = _get_voxel_size();
	float height;
	if (!data->find_ground(p_local / vs, MAX(p_range / vs, 1.0f), height, r_layer)) {
		return false;
	}
	r_height = height * vs;
	r_normal = data->sample_normal(Vector3(p_local.x / vs, height, p_local.z / vs));
	return true;
}

Dictionary VoxelTerrain3D::intersect_ray(const Vector3 &p_from, const Vector3 &p_direction, float p_max_distance) const {
	Dictionary result;
	Vector3 hit;
	if (intersect_ray_fast(p_from, p_direction, p_max_distance, hit)) {
		result["position"] = hit;
		result["normal"] = get_normal(hit);
	}
	return result;
}

/* -------------------------------------------------------------------------- */
/*                                   Editing                                  */
/* -------------------------------------------------------------------------- */

void VoxelTerrain3D::add_sphere(const Vector3 &p_global_position, float p_radius, int p_layer) {
	ERR_FAIL_COND(data.is_null());
	const float vs = _get_voxel_size();
	(void)data->edit_sphere(VoxelTerrainData::OPERATION_ADD, global_to_voxel(p_global_position), p_radius / vs, 1e6, 1.0, p_layer);
}

void VoxelTerrain3D::dig_sphere(const Vector3 &p_global_position, float p_radius) {
	ERR_FAIL_COND(data.is_null());
	const float vs = _get_voxel_size();
	(void)data->edit_sphere(VoxelTerrainData::OPERATION_REMOVE, global_to_voxel(p_global_position), p_radius / vs, 1e6, 1.0);
}

void VoxelTerrain3D::smooth_sphere(const Vector3 &p_global_position, float p_radius, float p_strength) {
	ERR_FAIL_COND(data.is_null());
	const float vs = _get_voxel_size();
	(void)data->edit_sphere(VoxelTerrainData::OPERATION_SMOOTH, global_to_voxel(p_global_position), p_radius / vs, p_strength, 0.5);
}

void VoxelTerrain3D::flatten_sphere(const Vector3 &p_global_position, float p_radius, float p_strength) {
	ERR_FAIL_COND(data.is_null());
	const float vs = _get_voxel_size();
	const Vector3 center = global_to_voxel(p_global_position);
	(void)data->edit_sphere(VoxelTerrainData::OPERATION_FLATTEN, center, p_radius / vs, p_strength, 0.5, -1, center.y);
}

void VoxelTerrain3D::paint_sphere(const Vector3 &p_global_position, float p_radius, int p_layer) {
	ERR_FAIL_COND(data.is_null());
	ERR_FAIL_INDEX(p_layer, VoxelTerrainData::MAX_LAYERS);
	const float vs = _get_voxel_size();
	(void)data->edit_sphere(VoxelTerrainData::OPERATION_PAINT, global_to_voxel(p_global_position), p_radius / vs, 1.0, 1.0, p_layer);
}

bool VoxelTerrain3D::_bake(const AABB &p_global_box, int p_lod, bool p_faces_only, VoxelMesher::Output &r_output) const {
	ERR_FAIL_COND_V(data.is_null(), false);
	const int lod = CLAMP(p_lod, 0, MAX_LOD);
	const int size = CELLS << lod;
	const int step = 1 << lod;
	const float vs = _get_voxel_size();
	const Vector3 lo_v = global_to_voxel(p_global_box.position);
	const Vector3 hi_v = global_to_voxel(p_global_box.get_end());
	const Vector3i lo(VoxelTerrainData::floor_div((int)Math::floor(lo_v.x), size), VoxelTerrainData::floor_div((int)Math::floor(lo_v.y), size), VoxelTerrainData::floor_div((int)Math::floor(lo_v.z), size));
	const Vector3i hi(VoxelTerrainData::floor_div((int)Math::ceil(hi_v.x) - 1, size), VoxelTerrainData::floor_div((int)Math::ceil(hi_v.y) - 1, size), VoxelTerrainData::floor_div((int)Math::ceil(hi_v.z) - 1, size));
	const Vector3i span = hi - lo + Vector3i(1, 1, 1);
	ERR_FAIL_COND_V_MSG((int64_t)span.x * span.y * span.z > 4096, false, "The box covers too many blocks to bake at this level of detail; bake a smaller box or a coarser level.");

	// Blocks are meshed one at a time and appended, relative to the
	// terrain's origin, without skirts.
	for (int z = lo.z; z <= hi.z; z++) {
		for (int y = lo.y; y <= hi.y; y++) {
			for (int x = lo.x; x <= hi.x; x++) {
				const Vector3i origin = Vector3i(x, y, z) * size;
				const Ref<VoxelGenerator> generator = data->get_generator();
				bool solid = false;
				const AABB box(Vector3(origin - Vector3i(step, step, step)), Vector3(size + 2 * step, size + 2 * step, size + 2 * step));
				if (!(generator.is_valid() && generator->may_contain_surface(box, vs, solid)) && !data->has_edits_in(box)) {
					continue;
				}
				VoxelMesher::Output out;
				mesh_block(data, origin, step, 0.0, p_faces_only, out);
				if (out.indices.is_empty()) {
					continue;
				}
				const Vector3 offset = Vector3(origin) * vs;
				if (p_faces_only) {
					for (int i = 0; i < out.collision_faces.size(); i++) {
						r_output.collision_faces.push_back(out.collision_faces[i] + offset);
					}
					continue;
				}
				const int base = r_output.vertices.size();
				for (int i = 0; i < out.vertices.size(); i++) {
					r_output.vertices.push_back(out.vertices[i] + offset);
				}
				r_output.normals.append_array(out.normals);
				for (int g = 0; g < 4; g++) {
					r_output.weights[g].append_array(out.weights[g]);
				}
				for (int i = 0; i < out.surface_index_count; i++) {
					r_output.indices.push_back(out.indices[i] + base);
				}
			}
		}
	}
	r_output.surface_index_count = r_output.indices.size();
	return true;
}

Ref<ArrayMesh> VoxelTerrain3D::bake_mesh(const AABB &p_global_box, int p_lod) const {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	VoxelMesher::Output out;
	if (!_bake(p_global_box, p_lod, false, out) || out.indices.is_empty()) {
		return mesh;
	}
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = out.vertices;
	arrays[Mesh::ARRAY_NORMAL] = out.normals;
	for (int g = 0; g < 4; g++) {
		arrays[Mesh::ARRAY_CUSTOM0 + g] = out.weights[g];
	}
	arrays[Mesh::ARRAY_INDEX] = out.indices;
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), Dictionary(), custom_format());
	return mesh;
}

/* -------------------------------------------------------------------------- */
/*                                  Settings                                  */
/* -------------------------------------------------------------------------- */

void VoxelTerrain3D::set_view_distance(float p_distance) {
	view_distance = CLAMP(p_distance, 16.0f, 100000.0f);
	lod_dirty = true;
}

void VoxelTerrain3D::set_lod_distance(float p_distance) {
	lod_distance = CLAMP(p_distance, 8.0f, 100000.0f);
	lod_dirty = true;
}

void VoxelTerrain3D::set_cast_shadow(ShadowCasting p_mode) {
	ERR_FAIL_INDEX((int)p_mode, 4);
	cast_shadow = p_mode;
	for (const KeyValue<Vector4i, Chunk> &E : chunks) {
		_update_instance_settings(E.key, E.value);
	}
}

void VoxelTerrain3D::set_render_layers(uint32_t p_layers) {
	render_layers = p_layers;
	for (const KeyValue<Vector4i, Chunk> &E : chunks) {
		_update_instance_settings(E.key, E.value);
	}
}

void VoxelTerrain3D::set_viewer_path(const NodePath &p_path) {
	viewer_path = p_path;
	lod_dirty = true;
}

void VoxelTerrain3D::set_collision_enabled(bool p_enabled) {
	if (collision_enabled == p_enabled) {
		return;
	}
	collision_enabled = p_enabled;
	if (!collision_enabled) {
		for (KeyValue<Vector4i, Chunk> &E : chunks) {
			_free_collision(E.value);
		}
	}
	lod_dirty = true;
	notify_property_list_changed();
}

void VoxelTerrain3D::set_collision_layer(uint32_t p_layer) {
	collision_layer = p_layer;
	for (const KeyValue<Vector4i, Chunk> &E : chunks) {
		_update_body_settings(E.value);
	}
}

void VoxelTerrain3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
	for (const KeyValue<Vector4i, Chunk> &E : chunks) {
		_update_body_settings(E.value);
	}
}

void VoxelTerrain3D::set_collision_priority(real_t p_priority) {
	collision_priority = p_priority;
	for (const KeyValue<Vector4i, Chunk> &E : chunks) {
		_update_body_settings(E.value);
	}
}

void VoxelTerrain3D::set_collision_distance(float p_distance) {
	collision_distance = CLAMP(p_distance, 0.0f, 10000.0f);
	lod_dirty = true;
}

void VoxelTerrain3D::set_debug_view(DebugView p_view) {
	debug_view = p_view;
	material_dirty = true;
}

void VoxelTerrain3D::set_editor_viewer(const Vector3 &p_position) {
	editor_viewer = p_position;
	has_editor_viewer = true;
}

void VoxelTerrain3D::set_brush_preview(bool p_visible, const Vector3 &p_position, float p_radius, float p_hardness, const Color &p_color) {
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

PackedStringArray VoxelTerrain3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	if (data.is_null()) {
		warnings.push_back(RTR("This voxel terrain has no data. Select it and use Terrain > New Voxel Terrain in the Terrain dock, or assign a VoxelTerrainData."));
	} else if (data->get_generator().is_null() && data->get_block_count() == 0) {
		warnings.push_back(RTR("The voxel terrain data has no generator and no edits, so there is nothing to draw. Give it a VoxelGenerator."));
	}
	if (is_inside_tree()) {
		const Basis basis = get_global_transform().basis;
		if (!basis.is_equal_approx(Basis())) {
			warnings.push_back(RTR("VoxelTerrain3D ignores rotation and scale; only its position is used. Change the data's voxel_size to resize it."));
		}
	}
	if ((bool)get_meta(SNAME("_layer_textures_converted"), false)) {
		warnings.push_back(RTR("The layers' textures differ in size, format or mipmaps, so they were converted to uncompressed RGBA8, which uses four times the video memory. Import every layer texture with the same settings (VRAM Compressed, mipmaps on, same size) to avoid this."));
	}
	return warnings;
}

void VoxelTerrain3D::_validate_property(PropertyInfo &p_property) const {
	if ((p_property.name == "collision_layer" || p_property.name == "collision_mask" || p_property.name == "collision_priority" || p_property.name == "collision_distance") && !collision_enabled) {
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

void VoxelTerrain3D::navmesh_parse_init() {
#ifndef NAVIGATION_3D_DISABLED
	ERR_FAIL_NULL(NavigationServer3D::get_singleton());
	if (!_navmesh_source_geometry_parser.is_valid()) {
		_navmesh_source_geometry_parsing_callback = callable_mp_static(&VoxelTerrain3D::navmesh_parse_source_geometry);
		_navmesh_source_geometry_parser = NavigationServer3D::get_singleton()->source_geometry_parser_create();
		NavigationServer3D::get_singleton()->source_geometry_parser_set_callback(_navmesh_source_geometry_parser, _navmesh_source_geometry_parsing_callback);
	}
#endif
}

void VoxelTerrain3D::navmesh_parse_source_geometry(const Ref<NavigationMesh> &p_navigation_mesh, Ref<NavigationMeshSourceGeometryData3D> p_source_geometry_data, Node *p_node) {
#ifndef NAVIGATION_3D_DISABLED
	VoxelTerrain3D *terrain = Object::cast_to<VoxelTerrain3D>(p_node);
	if (terrain == nullptr || terrain->data.is_null()) {
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

	// The ground goes on forever, so something has to bound it: the baking
	// AABB when one is set, otherwise the full detail area around the viewer.
	AABB box = p_navigation_mesh->get_filter_baking_aabb();
	if (box.has_volume()) {
		box.position += p_navigation_mesh->get_filter_baking_aabb_offset();
	} else {
		bool any = false;
		const float vs = terrain->_get_voxel_size();
		for (const KeyValue<Vector4i, Chunk> &E : terrain->chunks) {
			if (E.key.w != 0 || !E.value.wanted) {
				continue;
			}
			const AABB chunk_box = _chunk_box(E.key);
			const AABB global(terrain->_get_origin() + chunk_box.position * vs, chunk_box.size * vs);
			box = any ? box.merge(global) : global;
			any = true;
		}
		if (!any) {
			return;
		}
	}
	// Meshed here from the voxels rather than read back from the GPU.
	VoxelMesher::Output out;
	if (terrain->_bake(box, 0, true, out) && !out.collision_faces.is_empty()) {
		p_source_geometry_data->add_faces(out.collision_faces, Transform3D(Basis(), terrain->_get_origin()));
	}
#endif
}

/* -------------------------------------------------------------------------- */
/*                                  Bindings                                  */
/* -------------------------------------------------------------------------- */

void VoxelTerrain3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_data", "data"), &VoxelTerrain3D::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &VoxelTerrain3D::get_data);
	ClassDB::bind_method(D_METHOD("set_terrain_material", "material"), &VoxelTerrain3D::set_terrain_material);
	ClassDB::bind_method(D_METHOD("get_terrain_material"), &VoxelTerrain3D::get_terrain_material);
	ClassDB::bind_method(D_METHOD("set_view_distance", "distance"), &VoxelTerrain3D::set_view_distance);
	ClassDB::bind_method(D_METHOD("get_view_distance"), &VoxelTerrain3D::get_view_distance);
	ClassDB::bind_method(D_METHOD("set_lod_distance", "distance"), &VoxelTerrain3D::set_lod_distance);
	ClassDB::bind_method(D_METHOD("get_lod_distance"), &VoxelTerrain3D::get_lod_distance);
	ClassDB::bind_method(D_METHOD("set_cast_shadow", "mode"), &VoxelTerrain3D::set_cast_shadow);
	ClassDB::bind_method(D_METHOD("get_cast_shadow"), &VoxelTerrain3D::get_cast_shadow);
	ClassDB::bind_method(D_METHOD("set_render_layers", "layers"), &VoxelTerrain3D::set_render_layers);
	ClassDB::bind_method(D_METHOD("get_render_layers"), &VoxelTerrain3D::get_render_layers);
	ClassDB::bind_method(D_METHOD("set_viewer_path", "path"), &VoxelTerrain3D::set_viewer_path);
	ClassDB::bind_method(D_METHOD("get_viewer_path"), &VoxelTerrain3D::get_viewer_path);
	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &VoxelTerrain3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &VoxelTerrain3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_layer", "layer"), &VoxelTerrain3D::set_collision_layer);
	ClassDB::bind_method(D_METHOD("get_collision_layer"), &VoxelTerrain3D::get_collision_layer);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &VoxelTerrain3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &VoxelTerrain3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_collision_priority", "priority"), &VoxelTerrain3D::set_collision_priority);
	ClassDB::bind_method(D_METHOD("get_collision_priority"), &VoxelTerrain3D::get_collision_priority);
	ClassDB::bind_method(D_METHOD("set_collision_distance", "distance"), &VoxelTerrain3D::set_collision_distance);
	ClassDB::bind_method(D_METHOD("get_collision_distance"), &VoxelTerrain3D::get_collision_distance);
	ClassDB::bind_method(D_METHOD("set_debug_view", "view"), &VoxelTerrain3D::set_debug_view);
	ClassDB::bind_method(D_METHOD("get_debug_view"), &VoxelTerrain3D::get_debug_view);

	ClassDB::bind_method(D_METHOD("global_to_voxel", "global_position"), &VoxelTerrain3D::global_to_voxel);
	ClassDB::bind_method(D_METHOD("voxel_to_global", "voxel_position"), &VoxelTerrain3D::voxel_to_global);
	ClassDB::bind_method(D_METHOD("get_distance", "global_position"), &VoxelTerrain3D::get_distance);
	ClassDB::bind_method(D_METHOD("get_normal", "global_position"), &VoxelTerrain3D::get_normal);
	ClassDB::bind_method(D_METHOD("get_layer", "global_position"), &VoxelTerrain3D::get_layer);
	ClassDB::bind_method(D_METHOD("get_surface_name", "global_position"), &VoxelTerrain3D::get_surface_name);
	ClassDB::bind_method(D_METHOD("intersect_ray", "from", "direction", "max_distance"), &VoxelTerrain3D::intersect_ray, DEFVAL(1000.0));

	ClassDB::bind_method(D_METHOD("add_sphere", "global_position", "radius", "layer"), &VoxelTerrain3D::add_sphere, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("dig_sphere", "global_position", "radius"), &VoxelTerrain3D::dig_sphere);
	ClassDB::bind_method(D_METHOD("smooth_sphere", "global_position", "radius", "strength"), &VoxelTerrain3D::smooth_sphere, DEFVAL(0.5));
	ClassDB::bind_method(D_METHOD("flatten_sphere", "global_position", "radius", "strength"), &VoxelTerrain3D::flatten_sphere, DEFVAL(0.5));
	ClassDB::bind_method(D_METHOD("paint_sphere", "global_position", "radius", "layer"), &VoxelTerrain3D::paint_sphere);
	ClassDB::bind_method(D_METHOD("bake_mesh", "global_box", "lod"), &VoxelTerrain3D::bake_mesh, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("rebuild"), &VoxelTerrain3D::rebuild);
	ClassDB::bind_method(D_METHOD("is_meshing"), &VoxelTerrain3D::is_meshing);
	ClassDB::bind_method(D_METHOD("get_chunk_count"), &VoxelTerrain3D::get_chunk_count);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, "VoxelTerrainData"), "set_data", "get_data");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "terrain_material", PROPERTY_HINT_RESOURCE_TYPE, "TerrainMaterial"), "set_terrain_material", "get_terrain_material");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "view_distance", PROPERTY_HINT_RANGE, "16,4096,1,or_greater,suffix:m"), "set_view_distance", "get_view_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_distance", PROPERTY_HINT_RANGE, "8,1024,1,or_greater,suffix:m"), "set_lod_distance", "get_lod_distance");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "viewer_path", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_viewer_path", "get_viewer_path");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cast_shadow", PROPERTY_HINT_ENUM, "Off,On,Double-Sided,Shadows Only"), "set_cast_shadow", "get_cast_shadow");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_layers", PROPERTY_HINT_LAYERS_3D_RENDER), "set_render_layers", "get_render_layers");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "debug_view", PROPERTY_HINT_ENUM, "Disabled,Normals,Layers,Level of Detail"), "set_debug_view", "get_debug_view");

	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_layer", "get_collision_layer");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_priority"), "set_collision_priority", "get_collision_priority");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_distance", PROPERTY_HINT_RANGE, "0,1024,1,or_greater,suffix:m"), "set_collision_distance", "get_collision_distance");

	ADD_SIGNAL(MethodInfo("meshing_finished"));
	ADD_SIGNAL(MethodInfo("terrain_changed", PropertyInfo(Variant::RECT2, "area")));

	BIND_ENUM_CONSTANT(DEBUG_VIEW_DISABLED);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_NORMALS);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_LAYERS);
	BIND_ENUM_CONSTANT(DEBUG_VIEW_LOD);

	BIND_ENUM_CONSTANT(SHADOW_CASTING_OFF);
	BIND_ENUM_CONSTANT(SHADOW_CASTING_ON);
	BIND_ENUM_CONSTANT(SHADOW_CASTING_DOUBLE_SIDED);
	BIND_ENUM_CONSTANT(SHADOW_CASTING_SHADOWS_ONLY);
}
