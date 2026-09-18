/**************************************************************************/
/*  rope_3d.h                                                             */
/**************************************************************************/
/*  Asosdot built-in rope node with Verlet physics.                       */
/**************************************************************************/

#pragma once

#include "scene/3d/visual_instance_3d.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/multimesh.h"

class Rope3D : public GeometryInstance3D {
	GDCLASS(Rope3D, GeometryInstance3D);

public:
	enum RenderMode {
		// A tube generated around the simulated curve.
		RENDER_MODE_TUBE,
		// One copy of a user supplied mesh per segment, drawn as a MultiMesh.
		RENDER_MODE_SEGMENT_MESH,
		// Simulate only; useful when something else follows the points.
		RENDER_MODE_NONE,
	};

	enum SegmentAxis {
		SEGMENT_AXIS_X,
		SEGMENT_AXIS_Y,
		SEGMENT_AXIS_Z,
	};

private:
	struct Point {
		// Verlet stores the previous position rather than a velocity; the
		// difference between the two IS the velocity, which makes distance
		// constraints trivial to satisfy without tracking impulses.
		Vector3 position;
		Vector3 previous;
		bool pinned = false;

		// The contact found for this point this frame. Querying the physics
		// space is far too expensive to redo inside the solver loop, so the
		// plane is cached once and projected against on every iteration --
		// which is what makes contacts hold without stretching the rope.
		bool has_contact = false;
		Vector3 contact_point;
		Vector3 contact_normal;
	};

	LocalVector<Point> points;
	bool points_dirty = true;

	// Where the attachments resolved to this step. The solver re-pins after
	// every iteration, and walking a NodePath that many times per frame adds up.
	Vector3 cached_start;
	Vector3 cached_end;
	bool cached_start_valid = false;
	bool cached_end_valid = false;

	// Rope
	int segment_count = 16;
	real_t rope_length = 4.0;
	real_t thickness = 0.05;

	// Simulation
	bool simulating = true;
	bool simulate_in_editor = false;
	Vector3 gravity = Vector3(0, -9.8, 0);
	real_t damping = 0.02;
	int solver_iterations = 8;
	Vector3 wind = Vector3();
	real_t stretch = 1.0;

	// Attachment
	bool attach_start_enabled = true;
	NodePath attach_start_node;
	Vector3 attach_start_offset;
	bool attach_end_enabled = false;
	NodePath attach_end_node;
	Vector3 attach_end_offset;

	// Rendering
	RenderMode render_mode = RENDER_MODE_TUBE;
	Ref<Material> material;
	int tube_sides = 6;
	Ref<Mesh> segment_mesh;
	SegmentAxis segment_axis = SEGMENT_AXIS_Y;
	Vector3 segment_scale = Vector3(1, 1, 1);
	bool segment_stretch = true;

	// Collision
	bool collision_enabled = false;
	uint32_t collision_mask = 1;
	real_t collision_radius = 0.05;
	real_t collision_friction = 0.5;

	Ref<ArrayMesh> tube_mesh;

	// Recreating the surface every tick means freeing and reallocating GPU
	// buffers 60 times a second. The surface is built once and its vertex
	// buffer written in place after that, which is what SoftBody3D does.
	Vector<uint8_t> tube_vertex_buffer;
	uint32_t tube_vertex_stride = 0;
	uint32_t tube_normal_stride = 0;
	uint32_t tube_offset_vertex = 0;
	uint32_t tube_offset_normal = 0;
	int tube_rings = 0;
	int tube_ring_verts = 0;
	bool tube_surface_ready = false;
	Ref<MultiMesh> segment_multimesh;
	RID collision_shape;

	void _rebuild_points();
	void _simulate(double p_delta);
	void _apply_attachments();
	void _solve_constraints();
	void _gather_contacts();
	void _apply_contacts();
	void _apply_contact_friction();
	bool _resolve_attachment(bool p_start, Vector3 &r_position) const;
	void _refresh_attachment_cache();

	void _update_mesh();
	void _build_tube();
	bool _rebuild_tube_surface(int p_rings, int p_ring_verts);
	void _update_tube_vertices();
	void _build_segments();
	void _clear_base();

protected:
	void _notification(int p_what);
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	// Rope
	void set_segment_count(int p_count);
	int get_segment_count() const;
	void set_rope_length(real_t p_length);
	real_t get_rope_length() const;
	void set_thickness(real_t p_thickness);
	real_t get_thickness() const;

	// Simulation
	void set_simulating(bool p_simulating);
	bool is_simulating() const;
	void set_simulate_in_editor(bool p_enabled);
	bool is_simulating_in_editor() const;
	void set_gravity(const Vector3 &p_gravity);
	Vector3 get_gravity() const;
	void set_damping(real_t p_damping);
	real_t get_damping() const;
	void set_solver_iterations(int p_iterations);
	int get_solver_iterations() const;
	void set_wind(const Vector3 &p_wind);
	Vector3 get_wind() const;
	void set_stretch(real_t p_stretch);
	real_t get_stretch() const;

	// Attachment
	void set_attach_start_enabled(bool p_enabled);
	bool is_attach_start_enabled() const;
	void set_attach_start_node(const NodePath &p_path);
	NodePath get_attach_start_node() const;
	void set_attach_start_offset(const Vector3 &p_offset);
	Vector3 get_attach_start_offset() const;
	void set_attach_end_enabled(bool p_enabled);
	bool is_attach_end_enabled() const;
	void set_attach_end_node(const NodePath &p_path);
	NodePath get_attach_end_node() const;
	void set_attach_end_offset(const Vector3 &p_offset);
	Vector3 get_attach_end_offset() const;

	// Rendering
	void set_render_mode(RenderMode p_mode);
	RenderMode get_render_mode() const;
	void set_material(const Ref<Material> &p_material);
	Ref<Material> get_material() const;
	void set_tube_sides(int p_sides);
	int get_tube_sides() const;
	void set_segment_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_segment_mesh() const;
	void set_segment_axis(SegmentAxis p_axis);
	SegmentAxis get_segment_axis() const;
	void set_segment_scale(const Vector3 &p_scale);
	Vector3 get_segment_scale() const;
	void set_segment_stretch(bool p_stretch);
	bool get_segment_stretch() const;

	// Collision
	void set_collision_enabled(bool p_enabled);
	bool is_collision_enabled() const;
	void set_collision_mask(uint32_t p_mask);
	uint32_t get_collision_mask() const;
	void set_collision_radius(real_t p_radius);
	real_t get_collision_radius() const;
	void set_collision_friction(real_t p_friction);
	real_t get_collision_friction() const;

	// Scripting helpers. Positions are global, which is what you want when
	// hanging something off the rope or reading where it ended up.
	int get_point_count() const;
	Vector3 get_point_position(int p_index) const;
	void set_point_position(int p_index, const Vector3 &p_position);
	void reset_rope();

	PackedStringArray get_configuration_warnings() const override;
	AABB get_aabb() const override;

	Rope3D();
	~Rope3D();
};

VARIANT_ENUM_CAST(Rope3D::RenderMode);
VARIANT_ENUM_CAST(Rope3D::SegmentAxis);
