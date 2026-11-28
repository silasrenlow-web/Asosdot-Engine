/**************************************************************************/
/*  quick_mesh_brush_3d.h                                                 */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/templates/local_vector.h"
#include "scene/3d/mesh_instance_3d.h"

class ArrayMesh;

// Asosdot: Quick Mesh Brush. A MeshInstance3D whose mesh is bent by brush points placed on
// its surface. Each point is moved, rotated or scaled with the normal 3D gizmos; the surface
// around it follows with a smooth falloff, and the mesh gets extra triangles only around the
// points so the bend stays smooth. The result is an ordinary mesh saved with the scene, so a
// game pays nothing for it at runtime unless update_at_runtime is turned on.
//
// Points apply in order: each one bends the surface as the points before it left it, so a
// point placed on an already bent area sits on the surface the user sees.
class QuickMeshBrush3D : public MeshInstance3D {
	GDCLASS(QuickMeshBrush3D, MeshInstance3D);

public:
	static constexpr int CUSTOM_CHANNELS = 4; // ARRAY_CUSTOM0 to ARRAY_CUSTOM3.

	struct BrushPoint {
		Transform3D rest; // Where the point was placed, Y along the surface normal.
		Transform3D transform; // Where it is now. rest == transform bends nothing.
		Vector3 surface_position; // The point on the unbent source mesh, used for detail.
		real_t radius = 1.0;
		real_t hardness = 0.0; // 0 = smooth all the way from the centre, 1 = a hard edge.
	};

private:
	// A refined copy of one source surface. Everything here depends only on the source
	// mesh, the points' surface positions and radii, and the detail settings, so moving a
	// point only reruns the bend.
	struct Surface {
		Mesh::PrimitiveType primitive = Mesh::PRIMITIVE_TRIANGLES;
		Array passthrough; // Surfaces that are not triangles are kept as they are.
		uint64_t format = 0;
		Ref<Material> material;
		String name;

		LocalVector<Vector3> positions;
		LocalVector<int> indices;
		// Per vertex attributes that blend: normal(3) tangent(4) color(4) uv(2) uv2(2) custom.
		LocalVector<float> blend;
		int blend_stride = 0;
		bool has_normal = false, has_tangent = false, has_color = false, has_uv = false, has_uv2 = false;
		int custom_float_count[QuickMeshBrush3D::CUSTOM_CHANNELS] = {};
		// Attributes that are copied from an original vertex (bones, weights, packed custom).
		LocalVector<int> copy_from;
		Array copy_source; // The source arrays for those attributes.
		int bone_stride = 0;
		int custom_byte_size[QuickMeshBrush3D::CUSTOM_CHANNELS] = {};

		LocalVector<int> weld; // Vertex -> first vertex at the same position.
		LocalVector<Vector3> rest_normals; // Welded face normals of the unbent surface.

		// Outputs of the last bend, kept for ray picks.
		LocalVector<Vector3> bent;
	};

	Ref<Mesh> source_mesh;
	LocalVector<BrushPoint> points;
	real_t detail = 10.0;
	int max_triangles = 250000;
	bool update_at_runtime = false;
	NodePath collision_shape;

	LocalVector<Surface> surfaces;
	Ref<ArrayMesh> result;
	bool refine_dirty = true;
	bool collision_dirty = false;
	bool bend_queued = false;
	bool budget_reached = false;
	bool dropped_animation_data = false;
	int refined_triangles = 0;

	bool _can_rebuild() const;
	void _queue_refine();
	void _queue_bend();
	void _queue_collision();
	void _deferred_rebuild();
	void _refine();
	void _refine_surface(Surface &r_surface, const Array &p_arrays, int &r_budget);
	void _bend();
	void _update_collision();

protected:
	void _notification(int p_what);
	static void _bind_methods();
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_source_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_source_mesh() const;

	void set_detail(real_t p_detail);
	real_t get_detail() const;

	void set_max_triangles(int p_count);
	int get_max_triangles() const;

	void set_update_at_runtime(bool p_enable);
	bool is_updating_at_runtime() const;

	void set_collision_shape(const NodePath &p_path);
	NodePath get_collision_shape() const;

	void set_point_count(int p_count);
	int get_point_count() const;

	int add_point(const Vector3 &p_position, const Vector3 &p_normal, real_t p_radius = 1.0, real_t p_hardness = 0.0);
	void remove_point(int p_index);
	void clear_points();

	void set_point_transform(int p_index, const Transform3D &p_transform);
	Transform3D get_point_transform(int p_index) const;
	void set_point_rest(int p_index, const Transform3D &p_rest);
	Transform3D get_point_rest(int p_index) const;
	void set_point_surface_position(int p_index, const Vector3 &p_position);
	Vector3 get_point_surface_position(int p_index) const;
	void set_point_radius(int p_index, real_t p_radius);
	real_t get_point_radius(int p_index) const;
	void set_point_hardness(int p_index, real_t p_hardness);
	real_t get_point_hardness(int p_index) const;
	void reset_point(int p_index);

	// All points as an Array of Dictionaries: what the editor stores for undo.
	Array get_points_data() const;
	void set_points_data(const Array &p_data);

	// Local space ray pick against the bent surface: {position, normal, surface_position}.
	Dictionary intersect_ray(const Vector3 &p_from, const Vector3 &p_dir) const;

	void rebuild();
	bool has_edits() const;
	int get_triangle_count() const;
	void apply_edits();

	PackedStringArray get_configuration_warnings() const override;

	QuickMeshBrush3D();
};
