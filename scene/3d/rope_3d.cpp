/**************************************************************************/
/*  rope_3d.cpp                                                           */
/**************************************************************************/
/*  Asosdot built-in rope node with Verlet physics.                       */
/**************************************************************************/

#include "rope_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"
#ifndef PHYSICS_3D_DISABLED
#include "servers/physics_3d/physics_server_3d.h"
#endif

Rope3D::Rope3D() {
	set_notify_transform(true);
	tube_mesh.instantiate();
}

Rope3D::~Rope3D() {
#ifndef PHYSICS_3D_DISABLED
	if (collision_shape.is_valid()) {
		PhysicsServer3D::get_singleton()->free_rid(collision_shape);
		collision_shape = RID();
	}
#endif
}

/* -------------------------------------------------------------------------- */
/*                                 Simulation                                  */
/* -------------------------------------------------------------------------- */

void Rope3D::_rebuild_points() {
	points_dirty = false;

	const int count = MAX(segment_count, 1) + 1;
	points.resize(count);

	// Lay the rope out along -Y from the start attachment, which is the pose
	// most people expect before physics has run: hanging straight down.
	Vector3 origin = get_global_transform().origin;
	Vector3 start;
	if (_resolve_attachment(true, start)) {
		origin = start;
	}

	const real_t step = MAX(rope_length, (real_t)0.001) / real_t(count - 1);
	for (int i = 0; i < count; i++) {
		Point &p = points[i];
		p.position = origin + Vector3(0, -step * real_t(i), 0);
		p.previous = p.position;
		p.pinned = false;
	}

	_refresh_attachment_cache();
	_apply_attachments();
	_update_mesh();
}

bool Rope3D::_resolve_attachment(bool p_start, Vector3 &r_position) const {
	const bool enabled = p_start ? attach_start_enabled : attach_end_enabled;
	if (!enabled) {
		return false;
	}

	const NodePath &path = p_start ? attach_start_node : attach_end_node;
	const Vector3 &offset = p_start ? attach_start_offset : attach_end_offset;

	if (path.is_empty()) {
		// No target: pin to the rope's own origin, so a bare Rope3D still hangs
		// from where it was placed instead of falling out of the world.
		if (!is_inside_tree()) {
			return false;
		}
		r_position = get_global_transform().xform(offset);
		return true;
	}

	if (!is_inside_tree()) {
		return false;
	}
	Node3D *target = Object::cast_to<Node3D>(get_node_or_null(path));
	if (target == nullptr || !target->is_inside_tree()) {
		return false;
	}
	r_position = target->get_global_transform().xform(offset);
	return true;
}

void Rope3D::_refresh_attachment_cache() {
	cached_start_valid = _resolve_attachment(true, cached_start);
	cached_end_valid = _resolve_attachment(false, cached_end);
}

void Rope3D::_apply_attachments() {
	if (points.is_empty()) {
		return;
	}

	for (uint32_t i = 0; i < points.size(); i++) {
		points[i].pinned = false;
	}

	if (cached_start_valid) {
		points[0].position = cached_start;
		points[0].previous = cached_start;
		points[0].pinned = true;
	}
	if (cached_end_valid) {
		Point &p = points[points.size() - 1];
		p.position = cached_end;
		p.previous = cached_end;
		p.pinned = true;
	}
}

void Rope3D::_solve_constraints() {
	if (points.size() < 2) {
		return;
	}

	const real_t rest = MAX(rope_length, (real_t)0.001) / real_t(points.size() - 1);
	const real_t max_len = rest * MAX(stretch, (real_t)0.01);

	for (int it = 0; it < MAX(solver_iterations, 1); it++) {
		for (uint32_t i = 0; i + 1 < points.size(); i++) {
			Point &a = points[i];
			Point &b = points[i + 1];

			Vector3 delta = b.position - a.position;
			const real_t dist = delta.length();
			if (dist < CMP_EPSILON) {
				continue;
			}
			if (dist <= max_len) {
				continue;
			}

			// Move both ends halfway unless one is pinned, in which case the
			// free end takes the whole correction.
			const Vector3 correction = delta * ((dist - max_len) / dist);
			if (a.pinned && b.pinned) {
				continue;
			} else if (a.pinned) {
				b.position -= correction;
			} else if (b.pinned) {
				a.position += correction;
			} else {
				a.position += correction * 0.5;
				b.position -= correction * 0.5;
			}
		}

		_apply_contacts();

		// Re-pin after each pass; the constraints above will have dragged the
		// attached ends off their targets.
		_apply_attachments();
	}
}

void Rope3D::_gather_contacts() {
	for (uint32_t i = 0; i < points.size(); i++) {
		points[i].has_contact = false;
	}
#ifndef PHYSICS_3D_DISABLED
	if (!collision_enabled || !is_inside_tree()) {
		return;
	}
	Ref<World3D> world = get_world_3d();
	if (world.is_null()) {
		return;
	}
	PhysicsDirectSpaceState3D *space = world->get_direct_space_state();
	if (space == nullptr) {
		return;
	}

	if (collision_shape.is_null()) {
		collision_shape = PhysicsServer3D::get_singleton()->sphere_shape_create();
	}
	PhysicsServer3D::get_singleton()->shape_set_data(collision_shape, MAX(collision_radius, (real_t)0.001));

	PhysicsDirectSpaceState3D::ShapeParameters params;
	params.shape_rid = collision_shape;
	params.collision_mask = collision_mask;
	params.collide_with_bodies = true;
	params.collide_with_areas = false;

	for (uint32_t i = 0; i < points.size(); i++) {
		Point &p = points[i];
		if (p.pinned) {
			continue;
		}

		params.transform = Transform3D(Basis(), p.position);

		PhysicsDirectSpaceState3D::ShapeRestInfo rest;
		if (!space->rest_info(params, &rest)) {
			continue;
		}

		p.has_contact = true;
		p.contact_point = rest.point;
		p.contact_normal = rest.normal;
	}
#endif // PHYSICS_3D_DISABLED
}

void Rope3D::_apply_contacts() {
	const real_t radius = MAX(collision_radius, (real_t)0.001);
	for (uint32_t i = 0; i < points.size(); i++) {
		Point &p = points[i];
		if (!p.has_contact || p.pinned) {
			continue;
		}

		// How far the point sits below the contact plane, if at all.
		const real_t depth = radius - (p.position - p.contact_point).dot(p.contact_normal);
		if (depth <= 0.0) {
			continue;
		}

		// Carrying the previous position along with the correction is what
		// stops the contact from flinging the rope back off the surface.
		const Vector3 shift = p.contact_normal * depth;
		p.position += shift;
		p.previous += shift;
	}
}

void Rope3D::_apply_contact_friction() {
	if (collision_friction <= 0.0) {
		return;
	}

	const real_t keep = CLAMP((real_t)1.0 - collision_friction, (real_t)0.0, (real_t)1.0);
	for (uint32_t i = 0; i < points.size(); i++) {
		Point &p = points[i];
		if (!p.has_contact || p.pinned) {
			continue;
		}

		// Only the sliding part of the motion is damped. Without this a rope
		// draped over anything slides off it forever, because a contact that
		// only resists penetration cannot hold the rope in place.
		const Vector3 velocity = p.position - p.previous;
		const Vector3 normal_part = p.contact_normal * velocity.dot(p.contact_normal);
		const Vector3 tangent_part = velocity - normal_part;
		p.previous = p.position - (normal_part + tangent_part * keep);
	}
}

void Rope3D::_simulate(double p_delta) {
	if (points.size() < 2 || p_delta <= 0.0) {
		return;
	}

	_refresh_attachment_cache();

	const real_t dt = real_t(p_delta);
	const real_t retain = CLAMP((real_t)1.0 - damping, (real_t)0.0, (real_t)1.0);
	const Vector3 accel = (gravity + wind) * dt * dt;

	for (uint32_t i = 0; i < points.size(); i++) {
		Point &p = points[i];
		if (p.pinned) {
			continue;
		}
		// Verlet: the step is the previous step (scaled by damping) plus
		// acceleration. No explicit velocity is stored.
		const Vector3 current = p.position;
		p.position += (current - p.previous) * retain + accel;
		p.previous = current;
	}

	_apply_attachments();
	_gather_contacts();
	_solve_constraints();
	_apply_contact_friction();
}

/* -------------------------------------------------------------------------- */
/*                                  Rendering                                  */
/* -------------------------------------------------------------------------- */

void Rope3D::_clear_base() {
	set_base(RID());
}

void Rope3D::_build_tube() {
	const int rings = points.size();
	const int ring_verts = tube_sides + 1; // Duplicated seam so UVs can wrap.

	if (rings < 2 || tube_sides < 3) {
		tube_surface_ready = false;
		_clear_base();
		return;
	}

	if (!tube_surface_ready || tube_rings != rings || tube_ring_verts != ring_verts) {
		if (!_rebuild_tube_surface(rings, ring_verts)) {
			return;
		}
	}

	_update_tube_vertices();

	if (get_base() != tube_mesh->get_rid()) {
		set_base(tube_mesh->get_rid());
	}
}

bool Rope3D::_rebuild_tube_surface(int p_rings, int p_ring_verts) {
	tube_surface_ready = false;
	tube_mesh->clear_surfaces();

	const int count = p_rings * p_ring_verts;

	PackedVector3Array verts;
	PackedVector3Array normals;
	PackedVector2Array uvs;
	PackedInt32Array indices;
	verts.resize(count);
	normals.resize(count);
	uvs.resize(count);

	// Positions and normals are overwritten immediately by the in place update;
	// only the UVs and the index buffer have to be right here, and neither
	// changes while the topology holds.
	Vector3 *nw = normals.ptrw();
	Vector2 *uw = uvs.ptrw();
	for (int i = 0; i < p_rings; i++) {
		const real_t v = real_t(i) / real_t(p_rings - 1);
		for (int j = 0; j < p_ring_verts; j++) {
			const int idx = i * p_ring_verts + j;
			nw[idx] = Vector3(0, 1, 0);
			uw[idx] = Vector2(real_t(j) / real_t(tube_sides), v);
		}
	}

	for (int i = 0; i + 1 < p_rings; i++) {
		for (int j = 0; j < tube_sides; j++) {
			const int a = i * p_ring_verts + j;
			const int b = a + 1;
			const int c = a + p_ring_verts;
			const int d = c + 1;
			indices.push_back(a);
			indices.push_back(b);
			indices.push_back(c);
			indices.push_back(b);
			indices.push_back(d);
			indices.push_back(c);
		}
	}

	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = verts;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;

	tube_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	if (material.is_valid()) {
		tube_mesh->surface_set_material(0, material);
	}

	const RenderingServerTypes::SurfaceData surface = RenderingServer::get_singleton()->mesh_get_surface(tube_mesh->get_rid(), 0);
	const uint64_t format = surface.format;

	uint32_t offsets[RSE::ARRAY_MAX];
	uint32_t attrib_stride = 0;
	uint32_t skin_stride = 0;
	RenderingServer::get_singleton()->mesh_surface_make_offsets_from_format(
			format, count, indices.size(), offsets, tube_vertex_stride,
			tube_normal_stride, attrib_stride, skin_stride);
	tube_offset_vertex = offsets[RSE::ARRAY_VERTEX];
	tube_offset_normal = offsets[RSE::ARRAY_NORMAL];

	// The in place writer packs positions as three floats and normals as an
	// octahedral pair, so bail out to the slow path rather than corrupting the
	// buffer if the format ever stops matching that.
	if (tube_vertex_stride != sizeof(float) * 3 || tube_normal_stride < sizeof(uint32_t)) {
		WARN_PRINT_ONCE("Rope3D: unexpected vertex format, falling back to rebuilding the tube each frame.");
		return false;
	}

	tube_vertex_buffer = surface.vertex_data;
	if (tube_vertex_buffer.size() < int(count * tube_vertex_stride)) {
		return false;
	}

	tube_rings = p_rings;
	tube_ring_verts = p_ring_verts;
	tube_surface_ready = true;
	return true;
}

void Rope3D::_update_tube_vertices() {
	const Transform3D inv = get_global_transform().affine_inverse();
	uint8_t *write = tube_vertex_buffer.ptrw();

	AABB aabb;
	bool aabb_started = false;

	// Carried frame. Re-orthogonalising the previous up against each new
	// tangent is what keeps the tube from spinning along its length.
	Vector3 up = Vector3(0, 0, 1);

	for (int i = 0; i < tube_rings; i++) {
		const Vector3 p = inv.xform(points[i].position);

		Vector3 tangent;
		if (i == 0) {
			tangent = inv.xform(points[1].position) - p;
		} else if (i == tube_rings - 1) {
			tangent = p - inv.xform(points[i - 1].position);
		} else {
			tangent = inv.xform(points[i + 1].position) - inv.xform(points[i - 1].position);
		}
		if (tangent.length_squared() < CMP_EPSILON) {
			tangent = Vector3(0, -1, 0);
		}
		tangent.normalize();

		if (Math::abs(up.dot(tangent)) > 0.999) {
			// Degenerate: the carried up has lined up with the tangent, so pick
			// a different one rather than producing a zero-length cross product.
			up = Math::abs(tangent.x) < 0.9 ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
		}
		up = (up - tangent * up.dot(tangent)).normalized();
		const Vector3 right = tangent.cross(up).normalized();

		for (int j = 0; j < tube_ring_verts; j++) {
			const real_t a = Math::TAU * real_t(j) / real_t(tube_sides);
			const Vector3 dir = right * Math::cos(a) + up * Math::sin(a);
			const Vector3 pos = p + dir * thickness;
			const int idx = i * tube_ring_verts + j;

			float *vertex = reinterpret_cast<float *>(write + idx * tube_vertex_stride + tube_offset_vertex);
			vertex[0] = (float)pos.x;
			vertex[1] = (float)pos.y;
			vertex[2] = (float)pos.z;

			const Vector2 oct = dir.octahedron_encode();
			uint32_t packed = 0;
			packed |= (uint16_t)CLAMP(oct.x * 65535, 0, 65535);
			packed |= (uint16_t)CLAMP(oct.y * 65535, 0, 65535) << 16;
			memcpy(write + idx * tube_normal_stride + tube_offset_normal, &packed, sizeof(uint32_t));

			if (aabb_started) {
				aabb.expand_to(pos);
			} else {
				aabb = AABB(pos, Vector3());
				aabb_started = true;
			}
		}
	}

	RenderingServer::get_singleton()->mesh_surface_update_vertex_region(tube_mesh->get_rid(), 0, 0, tube_vertex_buffer);
	// The stored AABB is computed when the surface is created, so without this
	// the rope gets culled against wherever it happened to be at build time.
	tube_mesh->set_custom_aabb(aabb);
}

void Rope3D::_build_segments() {
	if (points.size() < 2 || segment_mesh.is_null()) {
		_clear_base();
		return;
	}

	if (segment_multimesh.is_null()) {
		segment_multimesh.instantiate();
		segment_multimesh->set_transform_format(MultiMesh::TRANSFORM_3D);
	}
	segment_multimesh->set_mesh(segment_mesh);

	const int count = points.size() - 1;
	if (segment_multimesh->get_instance_count() != count) {
		segment_multimesh->set_instance_count(count);
	}

	// How long the supplied mesh is along its own axis, so stretching can map
	// it onto the segment rather than guessing a scale.
	const AABB mesh_aabb = segment_mesh->get_aabb();
	const int axis = int(segment_axis);
	real_t mesh_extent = mesh_aabb.size[axis];
	if (mesh_extent < CMP_EPSILON) {
		mesh_extent = 1.0;
	}

	Vector3 axis_vec;
	axis_vec[axis] = 1.0;

	const Transform3D inv = get_global_transform().affine_inverse();

	for (int i = 0; i < count; i++) {
		const Vector3 a = inv.xform(points[i].position);
		const Vector3 b = inv.xform(points[i + 1].position);
		Vector3 dir = b - a;
		const real_t len = dir.length();
		if (len < CMP_EPSILON) {
			dir = Vector3(0, -1, 0);
		} else {
			dir /= len;
		}

		// Rotation taking the mesh's own axis onto the segment direction.
		Basis basis;
		const Vector3 cross = axis_vec.cross(dir);
		const real_t dot = CLAMP(axis_vec.dot(dir), (real_t)-1.0, (real_t)1.0);
		if (cross.length_squared() < CMP_EPSILON) {
			if (dot < 0) {
				// Exactly opposed: any perpendicular axis gives a valid flip.
				const Vector3 perp = Math::abs(axis_vec.x) < 0.9 ? Vector3(1, 0, 0) : Vector3(0, 1, 0);
				basis = Basis(perp.cross(axis_vec).normalized(), Math::PI);
			}
		} else {
			basis = Basis(cross.normalized(), Math::acos(dot));
		}

		Vector3 scale = segment_scale;
		if (segment_stretch) {
			scale[axis] *= len / mesh_extent;
		}
		basis.scale_local(scale);

		segment_multimesh->set_instance_transform(i, Transform3D(basis, (a + b) * 0.5));
	}

	set_base(segment_multimesh->get_rid());
}

void Rope3D::_update_mesh() {
	if (!is_inside_tree()) {
		return;
	}

	switch (render_mode) {
		case RENDER_MODE_TUBE:
			_build_tube();
			break;
		case RENDER_MODE_SEGMENT_MESH:
			_build_segments();
			break;
		default:
			_clear_base();
			break;
	}

	// A MultiMesh carries no material of its own, so in segment mode the rope's
	// material is applied to the instance instead of the tube surface.
	if (render_mode == RENDER_MODE_SEGMENT_MESH && material.is_valid()) {
		RenderingServer::get_singleton()->instance_geometry_set_material_override(get_instance(), material->get_rid());
	} else if (render_mode == RENDER_MODE_SEGMENT_MESH) {
		RenderingServer::get_singleton()->instance_geometry_set_material_override(get_instance(), RID());
	}
}

/* -------------------------------------------------------------------------- */
/*                                  Lifecycle                                  */
/* -------------------------------------------------------------------------- */

void Rope3D::_update_gizmo_if_moved() {
	if (!Engine::get_singleton()->is_editor_hint() || points.size() < 2) {
		return;
	}

	// Three samples catch any movement large enough to matter for picking,
	// for a fraction of the cost of comparing every point every tick.
	const Vector3 sample[3] = {
		points[0].position,
		points[points.size() / 2].position,
		points[points.size() - 1].position,
	};

	if (gizmo_sampled && sample[0].is_equal_approx(gizmo_sample[0]) &&
			sample[1].is_equal_approx(gizmo_sample[1]) &&
			sample[2].is_equal_approx(gizmo_sample[2])) {
		return;
	}

	for (int i = 0; i < 3; i++) {
		gizmo_sample[i] = sample[i];
	}
	gizmo_sampled = true;
	update_gizmos();
}

void Rope3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			points_dirty = true;
			set_physics_process_internal(true);
		} break;

		case NOTIFICATION_INTERNAL_PHYSICS_PROCESS: {
			if (points_dirty) {
				_rebuild_points();
			}

			const bool in_editor = Engine::get_singleton()->is_editor_hint();
			if (simulating && (!in_editor || simulate_in_editor)) {
				_simulate(get_physics_process_delta_time());
			} else {
				// Still follow the attachments so dragging a target in the
				// editor moves the rope with it.
				_refresh_attachment_cache();
				_apply_attachments();
				_solve_constraints();
			}
			_update_mesh();
			_update_gizmo_if_moved();
		} break;

		case NOTIFICATION_TRANSFORM_CHANGED: {
			// Points live in world space, so moving the node only changes the
			// space the mesh is built in.
			_update_mesh();
		} break;
	}
}

/* -------------------------------------------------------------------------- */
/*                                  Accessors                                  */
/* -------------------------------------------------------------------------- */

void Rope3D::set_segment_count(int p_count) {
	segment_count = CLAMP(p_count, 1, 512);
	points_dirty = true;
	update_gizmos();
}

int Rope3D::get_segment_count() const {
	return segment_count;
}

void Rope3D::set_rope_length(real_t p_length) {
	rope_length = MAX(p_length, (real_t)0.001);
	points_dirty = true;
}

real_t Rope3D::get_rope_length() const {
	return rope_length;
}

void Rope3D::set_thickness(real_t p_thickness) {
	thickness = MAX(p_thickness, (real_t)0.0001);
	_update_mesh();
	update_gizmos();
}

real_t Rope3D::get_thickness() const {
	return thickness;
}

void Rope3D::set_simulating(bool p_simulating) {
	simulating = p_simulating;
}

bool Rope3D::is_simulating() const {
	return simulating;
}

void Rope3D::set_simulate_in_editor(bool p_enabled) {
	simulate_in_editor = p_enabled;
}

bool Rope3D::is_simulating_in_editor() const {
	return simulate_in_editor;
}

void Rope3D::set_gravity(const Vector3 &p_gravity) {
	gravity = p_gravity;
}

Vector3 Rope3D::get_gravity() const {
	return gravity;
}

void Rope3D::set_damping(real_t p_damping) {
	damping = CLAMP(p_damping, (real_t)0.0, (real_t)1.0);
}

real_t Rope3D::get_damping() const {
	return damping;
}

void Rope3D::set_solver_iterations(int p_iterations) {
	solver_iterations = CLAMP(p_iterations, 1, 64);
}

int Rope3D::get_solver_iterations() const {
	return solver_iterations;
}

void Rope3D::set_wind(const Vector3 &p_wind) {
	wind = p_wind;
}

Vector3 Rope3D::get_wind() const {
	return wind;
}

void Rope3D::set_stretch(real_t p_stretch) {
	stretch = MAX(p_stretch, (real_t)0.01);
}

real_t Rope3D::get_stretch() const {
	return stretch;
}

void Rope3D::set_attach_start_enabled(bool p_enabled) {
	attach_start_enabled = p_enabled;
	update_gizmos();
	notify_property_list_changed();
	update_configuration_warnings();
}

bool Rope3D::is_attach_start_enabled() const {
	return attach_start_enabled;
}

void Rope3D::set_attach_start_node(const NodePath &p_path) {
	attach_start_node = p_path;
	update_configuration_warnings();
}

NodePath Rope3D::get_attach_start_node() const {
	return attach_start_node;
}

void Rope3D::set_attach_start_offset(const Vector3 &p_offset) {
	attach_start_offset = p_offset;
}

Vector3 Rope3D::get_attach_start_offset() const {
	return attach_start_offset;
}

void Rope3D::set_attach_end_enabled(bool p_enabled) {
	attach_end_enabled = p_enabled;
	update_gizmos();
	notify_property_list_changed();
	update_configuration_warnings();
}

bool Rope3D::is_attach_end_enabled() const {
	return attach_end_enabled;
}

void Rope3D::set_attach_end_node(const NodePath &p_path) {
	attach_end_node = p_path;
	update_configuration_warnings();
}

NodePath Rope3D::get_attach_end_node() const {
	return attach_end_node;
}

void Rope3D::set_attach_end_offset(const Vector3 &p_offset) {
	attach_end_offset = p_offset;
}

Vector3 Rope3D::get_attach_end_offset() const {
	return attach_end_offset;
}

void Rope3D::set_render_mode(RenderMode p_mode) {
	render_mode = p_mode;
	update_gizmos();
	notify_property_list_changed();
	update_configuration_warnings();
	_update_mesh();
}

Rope3D::RenderMode Rope3D::get_render_mode() const {
	return render_mode;
}

void Rope3D::set_material(const Ref<Material> &p_material) {
	material = p_material;
	tube_surface_ready = false;
	_update_mesh();
}

Ref<Material> Rope3D::get_material() const {
	return material;
}

void Rope3D::set_tube_sides(int p_sides) {
	tube_sides = CLAMP(p_sides, 3, 64);
	_update_mesh();
}

int Rope3D::get_tube_sides() const {
	return tube_sides;
}

void Rope3D::set_segment_mesh(const Ref<Mesh> &p_mesh) {
	segment_mesh = p_mesh;
	// The instance count is tied to the mesh, so make the MultiMesh rebuild
	// rather than reusing stale transforms.
	segment_multimesh.unref();
	update_configuration_warnings();
	_update_mesh();
}

Ref<Mesh> Rope3D::get_segment_mesh() const {
	return segment_mesh;
}

void Rope3D::set_segment_axis(SegmentAxis p_axis) {
	segment_axis = p_axis;
	_update_mesh();
}

Rope3D::SegmentAxis Rope3D::get_segment_axis() const {
	return segment_axis;
}

void Rope3D::set_segment_scale(const Vector3 &p_scale) {
	segment_scale = p_scale;
	_update_mesh();
}

Vector3 Rope3D::get_segment_scale() const {
	return segment_scale;
}

void Rope3D::set_segment_stretch(bool p_stretch) {
	segment_stretch = p_stretch;
	_update_mesh();
}

bool Rope3D::get_segment_stretch() const {
	return segment_stretch;
}

void Rope3D::set_collision_enabled(bool p_enabled) {
	collision_enabled = p_enabled;
	notify_property_list_changed();
}

bool Rope3D::is_collision_enabled() const {
	return collision_enabled;
}

void Rope3D::set_collision_mask(uint32_t p_mask) {
	collision_mask = p_mask;
}

uint32_t Rope3D::get_collision_mask() const {
	return collision_mask;
}

void Rope3D::set_collision_radius(real_t p_radius) {
	collision_radius = MAX(p_radius, (real_t)0.001);
}

real_t Rope3D::get_collision_radius() const {
	return collision_radius;
}

void Rope3D::set_collision_friction(real_t p_friction) {
	collision_friction = CLAMP(p_friction, (real_t)0.0, (real_t)1.0);
}

real_t Rope3D::get_collision_friction() const {
	return collision_friction;
}

int Rope3D::get_point_count() const {
	return points.size();
}

Vector3 Rope3D::get_point_position(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)points.size(), Vector3());
	return points[p_index].position;
}

void Rope3D::set_point_position(int p_index, const Vector3 &p_position) {
	ERR_FAIL_INDEX(p_index, (int)points.size());
	points[p_index].position = p_position;
	points[p_index].previous = p_position;
}

void Rope3D::reset_rope() {
	points_dirty = true;
}

/* -------------------------------------------------------------------------- */
/*                                   Editor                                    */
/* -------------------------------------------------------------------------- */

void Rope3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name.begins_with("tube_") && render_mode != RENDER_MODE_TUBE) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (p_property.name.begins_with("segment_mesh") || p_property.name.begins_with("segment_axis") ||
			p_property.name.begins_with("segment_scale") || p_property.name.begins_with("segment_stretch")) {
		if (render_mode != RENDER_MODE_SEGMENT_MESH) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}
	if ((p_property.name == "attach_start_node" || p_property.name == "attach_start_offset") && !attach_start_enabled) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if ((p_property.name == "attach_end_node" || p_property.name == "attach_end_offset") && !attach_end_enabled) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if ((p_property.name == "collision_mask" || p_property.name == "collision_radius" ||
			p_property.name == "collision_friction") && !collision_enabled) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

PackedStringArray Rope3D::get_configuration_warnings() const {
	PackedStringArray warnings = GeometryInstance3D::get_configuration_warnings();

	if (render_mode == RENDER_MODE_SEGMENT_MESH && segment_mesh.is_null()) {
		warnings.push_back(RTR("Render Mode is set to Segment Mesh but no Segment Mesh is assigned, so the rope will not be visible."));
	}
	if (!attach_start_enabled && !attach_end_enabled) {
		warnings.push_back(RTR("Neither end is attached, so the rope will simply fall. Enable Attach Start or Attach End, or pin points from a script."));
	}
	if (!attach_start_node.is_empty() && Object::cast_to<Node3D>(get_node_or_null(attach_start_node)) == nullptr) {
		warnings.push_back(RTR("Attach Start Node does not point at a Node3D."));
	}
	if (!attach_end_node.is_empty() && Object::cast_to<Node3D>(get_node_or_null(attach_end_node)) == nullptr) {
		warnings.push_back(RTR("Attach End Node does not point at a Node3D."));
	}

	return warnings;
}

AABB Rope3D::get_aabb() const {
	if (points.is_empty()) {
		return AABB();
	}

	const Transform3D inv = get_global_transform().affine_inverse();
	AABB aabb(inv.xform(points[0].position), Vector3());
	for (uint32_t i = 1; i < points.size(); i++) {
		aabb.expand_to(inv.xform(points[i].position));
	}
	return aabb.grow(MAX(thickness, collision_radius));
}

/* -------------------------------------------------------------------------- */
/*                                   Bindings                                  */
/* -------------------------------------------------------------------------- */

void Rope3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_segment_count", "count"), &Rope3D::set_segment_count);
	ClassDB::bind_method(D_METHOD("get_segment_count"), &Rope3D::get_segment_count);
	ClassDB::bind_method(D_METHOD("set_rope_length", "length"), &Rope3D::set_rope_length);
	ClassDB::bind_method(D_METHOD("get_rope_length"), &Rope3D::get_rope_length);
	ClassDB::bind_method(D_METHOD("set_thickness", "thickness"), &Rope3D::set_thickness);
	ClassDB::bind_method(D_METHOD("get_thickness"), &Rope3D::get_thickness);

	ClassDB::bind_method(D_METHOD("set_simulating", "simulating"), &Rope3D::set_simulating);
	ClassDB::bind_method(D_METHOD("is_simulating"), &Rope3D::is_simulating);
	ClassDB::bind_method(D_METHOD("set_simulate_in_editor", "enabled"), &Rope3D::set_simulate_in_editor);
	ClassDB::bind_method(D_METHOD("is_simulating_in_editor"), &Rope3D::is_simulating_in_editor);
	ClassDB::bind_method(D_METHOD("set_gravity", "gravity"), &Rope3D::set_gravity);
	ClassDB::bind_method(D_METHOD("get_gravity"), &Rope3D::get_gravity);
	ClassDB::bind_method(D_METHOD("set_damping", "damping"), &Rope3D::set_damping);
	ClassDB::bind_method(D_METHOD("get_damping"), &Rope3D::get_damping);
	ClassDB::bind_method(D_METHOD("set_solver_iterations", "iterations"), &Rope3D::set_solver_iterations);
	ClassDB::bind_method(D_METHOD("get_solver_iterations"), &Rope3D::get_solver_iterations);
	ClassDB::bind_method(D_METHOD("set_wind", "wind"), &Rope3D::set_wind);
	ClassDB::bind_method(D_METHOD("get_wind"), &Rope3D::get_wind);
	ClassDB::bind_method(D_METHOD("set_stretch", "stretch"), &Rope3D::set_stretch);
	ClassDB::bind_method(D_METHOD("get_stretch"), &Rope3D::get_stretch);

	ClassDB::bind_method(D_METHOD("set_attach_start_enabled", "enabled"), &Rope3D::set_attach_start_enabled);
	ClassDB::bind_method(D_METHOD("is_attach_start_enabled"), &Rope3D::is_attach_start_enabled);
	ClassDB::bind_method(D_METHOD("set_attach_start_node", "path"), &Rope3D::set_attach_start_node);
	ClassDB::bind_method(D_METHOD("get_attach_start_node"), &Rope3D::get_attach_start_node);
	ClassDB::bind_method(D_METHOD("set_attach_start_offset", "offset"), &Rope3D::set_attach_start_offset);
	ClassDB::bind_method(D_METHOD("get_attach_start_offset"), &Rope3D::get_attach_start_offset);
	ClassDB::bind_method(D_METHOD("set_attach_end_enabled", "enabled"), &Rope3D::set_attach_end_enabled);
	ClassDB::bind_method(D_METHOD("is_attach_end_enabled"), &Rope3D::is_attach_end_enabled);
	ClassDB::bind_method(D_METHOD("set_attach_end_node", "path"), &Rope3D::set_attach_end_node);
	ClassDB::bind_method(D_METHOD("get_attach_end_node"), &Rope3D::get_attach_end_node);
	ClassDB::bind_method(D_METHOD("set_attach_end_offset", "offset"), &Rope3D::set_attach_end_offset);
	ClassDB::bind_method(D_METHOD("get_attach_end_offset"), &Rope3D::get_attach_end_offset);

	ClassDB::bind_method(D_METHOD("set_render_mode", "mode"), &Rope3D::set_render_mode);
	ClassDB::bind_method(D_METHOD("get_render_mode"), &Rope3D::get_render_mode);
	ClassDB::bind_method(D_METHOD("set_material", "material"), &Rope3D::set_material);
	ClassDB::bind_method(D_METHOD("get_material"), &Rope3D::get_material);
	ClassDB::bind_method(D_METHOD("set_tube_sides", "sides"), &Rope3D::set_tube_sides);
	ClassDB::bind_method(D_METHOD("get_tube_sides"), &Rope3D::get_tube_sides);
	ClassDB::bind_method(D_METHOD("set_segment_mesh", "mesh"), &Rope3D::set_segment_mesh);
	ClassDB::bind_method(D_METHOD("get_segment_mesh"), &Rope3D::get_segment_mesh);
	ClassDB::bind_method(D_METHOD("set_segment_axis", "axis"), &Rope3D::set_segment_axis);
	ClassDB::bind_method(D_METHOD("get_segment_axis"), &Rope3D::get_segment_axis);
	ClassDB::bind_method(D_METHOD("set_segment_scale", "scale"), &Rope3D::set_segment_scale);
	ClassDB::bind_method(D_METHOD("get_segment_scale"), &Rope3D::get_segment_scale);
	ClassDB::bind_method(D_METHOD("set_segment_stretch", "stretch"), &Rope3D::set_segment_stretch);
	ClassDB::bind_method(D_METHOD("get_segment_stretch"), &Rope3D::get_segment_stretch);

	ClassDB::bind_method(D_METHOD("set_collision_enabled", "enabled"), &Rope3D::set_collision_enabled);
	ClassDB::bind_method(D_METHOD("is_collision_enabled"), &Rope3D::is_collision_enabled);
	ClassDB::bind_method(D_METHOD("set_collision_mask", "mask"), &Rope3D::set_collision_mask);
	ClassDB::bind_method(D_METHOD("get_collision_mask"), &Rope3D::get_collision_mask);
	ClassDB::bind_method(D_METHOD("set_collision_radius", "radius"), &Rope3D::set_collision_radius);
	ClassDB::bind_method(D_METHOD("get_collision_radius"), &Rope3D::get_collision_radius);
	ClassDB::bind_method(D_METHOD("set_collision_friction", "friction"), &Rope3D::set_collision_friction);
	ClassDB::bind_method(D_METHOD("get_collision_friction"), &Rope3D::get_collision_friction);

	ClassDB::bind_method(D_METHOD("get_point_count"), &Rope3D::get_point_count);
	ClassDB::bind_method(D_METHOD("get_point_position", "index"), &Rope3D::get_point_position);
	ClassDB::bind_method(D_METHOD("set_point_position", "index", "position"), &Rope3D::set_point_position);
	ClassDB::bind_method(D_METHOD("reset_rope"), &Rope3D::reset_rope);

	ADD_GROUP("Rope", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "segment_count", PROPERTY_HINT_RANGE, "1,512,1"), "set_segment_count", "get_segment_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rope_length", PROPERTY_HINT_RANGE, "0.01,200,0.01,or_greater,suffix:m"), "set_rope_length", "get_rope_length");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "thickness", PROPERTY_HINT_RANGE, "0.001,2,0.001,or_greater,suffix:m"), "set_thickness", "get_thickness");

	ADD_GROUP("Simulation", "");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "simulating"), "set_simulating", "is_simulating");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "simulate_in_editor"), "set_simulate_in_editor", "is_simulating_in_editor");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "gravity", PROPERTY_HINT_NONE, "suffix:m/s²"), "set_gravity", "get_gravity");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "wind", PROPERTY_HINT_NONE, "suffix:m/s²"), "set_wind", "get_wind");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "damping", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_damping", "get_damping");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stretch", PROPERTY_HINT_RANGE, "1,4,0.01"), "set_stretch", "get_stretch");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "solver_iterations", PROPERTY_HINT_RANGE, "1,64,1"), "set_solver_iterations", "get_solver_iterations");

	ADD_GROUP("Attach Start", "attach_start_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "attach_start_enabled"), "set_attach_start_enabled", "is_attach_start_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "attach_start_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_attach_start_node", "get_attach_start_node");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "attach_start_offset", PROPERTY_HINT_NONE, "suffix:m"), "set_attach_start_offset", "get_attach_start_offset");

	ADD_GROUP("Attach End", "attach_end_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "attach_end_enabled"), "set_attach_end_enabled", "is_attach_end_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "attach_end_node", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "Node3D"), "set_attach_end_node", "get_attach_end_node");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "attach_end_offset", PROPERTY_HINT_NONE, "suffix:m"), "set_attach_end_offset", "get_attach_end_offset");

	ADD_GROUP("Rendering", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_mode", PROPERTY_HINT_ENUM, "Tube,Segment Mesh,None"), "set_render_mode", "get_render_mode");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material", PROPERTY_HINT_RESOURCE_TYPE, "BaseMaterial3D,ShaderMaterial"), "set_material", "get_material");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "tube_sides", PROPERTY_HINT_RANGE, "3,64,1"), "set_tube_sides", "get_tube_sides");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "segment_mesh", PROPERTY_HINT_RESOURCE_TYPE, "Mesh"), "set_segment_mesh", "get_segment_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "segment_axis", PROPERTY_HINT_ENUM, "X,Y,Z"), "set_segment_axis", "get_segment_axis");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "segment_scale"), "set_segment_scale", "get_segment_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "segment_stretch"), "set_segment_stretch", "get_segment_stretch");

	ADD_GROUP("Collision", "collision_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "collision_enabled"), "set_collision_enabled", "is_collision_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), "set_collision_mask", "get_collision_mask");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_radius", PROPERTY_HINT_RANGE, "0.001,2,0.001,or_greater,suffix:m"), "set_collision_radius", "get_collision_radius");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "collision_friction", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_collision_friction", "get_collision_friction");

	BIND_ENUM_CONSTANT(RENDER_MODE_TUBE);
	BIND_ENUM_CONSTANT(RENDER_MODE_SEGMENT_MESH);
	BIND_ENUM_CONSTANT(RENDER_MODE_NONE);

	BIND_ENUM_CONSTANT(SEGMENT_AXIS_X);
	BIND_ENUM_CONSTANT(SEGMENT_AXIS_Y);
	BIND_ENUM_CONSTANT(SEGMENT_AXIS_Z);
}
