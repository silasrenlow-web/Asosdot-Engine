/**************************************************************************/
/*  quick_mesh_brush_3d.cpp                                               */
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

#include "quick_mesh_brush_3d.h"

#include "core/config/engine.h"
#include "core/math/face3.h"
#include "core/math/geometry_3d.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "scene/resources/mesh.h"

#ifndef PHYSICS_3D_DISABLED
#include "scene/3d/physics/collision_shape_3d.h"
#include "scene/resources/3d/concave_polygon_shape_3d.h"
#endif // PHYSICS_3D_DISABLED

namespace {

// A brush point with everything the bend needs per vertex worked out once.
struct PreparedPoint {
	Transform3D rest_inverse;
	Vector3 rest_origin;
	Quaternion rest_rotation;
	Vector3 rest_scale;
	Vector3 target_origin;
	Quaternion target_rotation;
	Vector3 target_scale;
	Transform3D target;
	real_t radius = 1.0;
	real_t radius_squared = 1.0;
	real_t hardness = 0.0;
};

// 1 at the centre, 0 at the radius. Smootherstep, so the bent surface has no crease at
// either end of the falloff.
real_t brush_weight(real_t p_t, real_t p_hardness) {
	if (p_t >= 1.0) {
		return 0.0;
	}
	if (p_t <= p_hardness) {
		return 1.0;
	}
	const real_t u = (p_t - p_hardness) / (1.0 - p_hardness);
	return 1.0 - u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
}

Vector3 bend_position(const LocalVector<PreparedPoint> &p_points, const Vector3 &p_position) {
	Vector3 p = p_position;
	for (const PreparedPoint &point : p_points) {
		const real_t distance_squared = (p - point.rest_origin).length_squared();
		if (distance_squared >= point.radius_squared) {
			continue;
		}
		const real_t w = brush_weight(Math::sqrt(distance_squared) / point.radius, point.hardness);
		if (w <= 0.0) {
			continue;
		}
		const Vector3 local = point.rest_inverse.xform(p);
		if (w >= 1.0) {
			p = point.target.xform(local);
		} else {
			const Basis basis(point.rest_rotation.slerp(point.target_rotation, w), point.rest_scale.lerp(point.target_scale, w));
			p = point.rest_origin.lerp(point.target_origin, w) + basis.xform(local);
		}
	}
	return p;
}

LocalVector<PreparedPoint> prepare_points(const LocalVector<QuickMeshBrush3D::BrushPoint> &p_points) {
	LocalVector<PreparedPoint> prepared;
	for (const QuickMeshBrush3D::BrushPoint &point : p_points) {
		if (point.radius <= CMP_EPSILON || point.rest.is_equal_approx(point.transform)) {
			continue;
		}
		PreparedPoint p;
		const Transform3D rest(point.rest.basis.orthonormalized(), point.rest.origin);
		p.rest_inverse = rest.affine_inverse();
		p.rest_origin = rest.origin;
		p.rest_rotation = rest.basis.get_rotation_quaternion();
		p.rest_scale = Vector3(1, 1, 1);
		p.target = point.transform;
		p.target_origin = point.transform.origin;
		p.target_rotation = point.transform.basis.get_rotation_quaternion();
		p.target_scale = point.transform.basis.get_scale();
		p.radius = point.radius;
		p.radius_squared = point.radius * point.radius;
		p.hardness = CLAMP(point.hardness, (real_t)0.0, (real_t)0.999);
		prepared.push_back(p);
	}
	return prepared;
}

// The flags add_surface_from_arrays needs to read a surface's arrays back the same way.
uint64_t surface_flags(uint64_t p_format) {
	uint64_t flags = p_format & Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS;
	for (int c = 0; c < QuickMeshBrush3D::CUSTOM_CHANNELS; c++) {
		flags |= p_format & ((uint64_t)Mesh::ARRAY_FORMAT_CUSTOM_MASK << (Mesh::ARRAY_FORMAT_CUSTOM_BASE + c * Mesh::ARRAY_FORMAT_CUSTOM_BITS));
	}
	return flags;
}

uint64_t edge_key(int p_a, int p_b) {
	return p_a < p_b ? ((uint64_t)p_a << 32) | (uint32_t)p_b : ((uint64_t)p_b << 32) | (uint32_t)p_a;
}

Basis surface_basis(const Vector3 &p_normal) {
	Vector3 y = p_normal.normalized();
	if (y.is_zero_approx()) {
		y = Vector3(0, 1, 0);
	}
	// X follows the node's X where it can, so points on a floor line up with the node.
	Vector3 x = Vector3(1, 0, 0) - y * y.x;
	if (x.length_squared() < 0.01) {
		x = Vector3(0, 0, -1) - y * -y.z;
	}
	x.normalize();
	const Vector3 z = x.cross(y).normalized();
	return Basis(x, y, z);
}

// Welded, area weighted face normals: one per position, so seams in UVs don't show.
void welded_normals(const LocalVector<Vector3> &p_positions, const LocalVector<int> &p_indices, const LocalVector<int> &p_weld, LocalVector<Vector3> &r_normals) {
	r_normals.resize(p_positions.size());
	for (Vector3 &n : r_normals) {
		n = Vector3();
	}
	for (uint32_t i = 0; i + 2 < p_indices.size(); i += 3) {
		const int a = p_indices[i], b = p_indices[i + 1], c = p_indices[i + 2];
		const Vector3 face = (p_positions[b] - p_positions[a]).cross(p_positions[c] - p_positions[a]);
		r_normals[p_weld[a]] += face;
		r_normals[p_weld[b]] += face;
		r_normals[p_weld[c]] += face;
	}
	for (uint32_t i = 0; i < r_normals.size(); i++) {
		r_normals[i] = r_normals[p_weld[i]];
	}
}

} // namespace

// Rebuilds happen in the editor, or in a game that asked for it.
bool QuickMeshBrush3D::_can_rebuild() const {
	return is_inside_tree() && (Engine::get_singleton()->is_editor_hint() || update_at_runtime);
}

void QuickMeshBrush3D::_queue_refine() {
	refine_dirty = true;
	collision_dirty = true;
	_queue_bend();
}

void QuickMeshBrush3D::_queue_bend() {
	if (bend_queued || !_can_rebuild()) {
		return;
	}
	bend_queued = true;
	callable_mp(this, &QuickMeshBrush3D::_deferred_rebuild).call_deferred();
}

void QuickMeshBrush3D::_deferred_rebuild() {
	if (!bend_queued) {
		return;
	}
	bend_queued = false;
	if (!_can_rebuild()) {
		return;
	}
	if (refine_dirty) {
		_refine();
	}
	_bend();
	if (collision_dirty) {
		_update_collision();
	}
}

// The collision shape follows finished edits, not every frame of a gizmo drag.
void QuickMeshBrush3D::_queue_collision() {
	collision_dirty = true;
	_queue_bend();
}

void QuickMeshBrush3D::_refine() {
	refine_dirty = false;
	const bool was_reached = budget_reached;
	const bool was_dropping = dropped_animation_data;
	budget_reached = false;
	dropped_animation_data = false;
	surfaces.clear();
	refined_triangles = 0;

	if (source_mesh.is_valid()) {
		int source_triangles = 0;
		for (int i = 0; i < source_mesh->get_surface_count(); i++) {
			source_triangles += source_mesh->surface_get_array_index_len(i) > 0 ? source_mesh->surface_get_array_index_len(i) / 3 : source_mesh->surface_get_array_len(i) / 3;
		}
		// The budget only limits the triangles the brush adds.
		int budget = MAX(max_triangles - source_triangles, 0);
		dropped_animation_data = source_mesh->get_blend_shape_count() > 0;
		surfaces.resize(source_mesh->get_surface_count());
		for (int i = 0; i < source_mesh->get_surface_count(); i++) {
			Surface &surface = surfaces[i];
			surface.primitive = source_mesh->surface_get_primitive_type(i);
			surface.format = source_mesh->surface_get_format(i);
			surface.material = source_mesh->surface_get_material(i);
			Ref<ArrayMesh> array_mesh = source_mesh;
			if (array_mesh.is_valid()) {
				surface.name = array_mesh->surface_get_name(i);
			}
			const Array arrays = source_mesh->surface_get_arrays(i);
			if (surface.primitive != Mesh::PRIMITIVE_TRIANGLES) {
				surface.passthrough = arrays;
				continue;
			}
			_refine_surface(surface, arrays, budget);
			refined_triangles += surface.indices.size() / 3;
		}
	}
	if (was_reached != budget_reached || was_dropping != dropped_animation_data) {
		update_configuration_warnings();
	}
}

void QuickMeshBrush3D::_refine_surface(Surface &r_surface, const Array &p_arrays, int &r_budget) {
	const PackedVector3Array vertices = p_arrays[Mesh::ARRAY_VERTEX];
	const int vertex_count = vertices.size();
	if (vertex_count == 0) {
		return;
	}

	r_surface.positions.resize(vertex_count);
	for (int i = 0; i < vertex_count; i++) {
		r_surface.positions[i] = vertices[i];
	}
	const PackedInt32Array source_indices = p_arrays[Mesh::ARRAY_INDEX];
	if (source_indices.is_empty()) {
		r_surface.indices.resize(vertex_count - vertex_count % 3);
		for (uint32_t i = 0; i < r_surface.indices.size(); i++) {
			r_surface.indices[i] = i;
		}
	} else {
		r_surface.indices.resize(source_indices.size() - source_indices.size() % 3);
		for (uint32_t i = 0; i < r_surface.indices.size(); i++) {
			r_surface.indices[i] = source_indices[i];
		}
	}

	// Lay out the attributes that blend between the two ends of a split edge.
	const PackedVector3Array normals = p_arrays[Mesh::ARRAY_NORMAL];
	const PackedFloat32Array tangents = p_arrays[Mesh::ARRAY_TANGENT];
	const PackedColorArray colors = p_arrays[Mesh::ARRAY_COLOR];
	const PackedVector2Array uvs = p_arrays[Mesh::ARRAY_TEX_UV];
	const PackedVector2Array uv2s = p_arrays[Mesh::ARRAY_TEX_UV2];
	r_surface.has_normal = normals.size() == vertex_count;
	r_surface.has_tangent = tangents.size() == vertex_count * 4;
	r_surface.has_color = colors.size() == vertex_count;
	r_surface.has_uv = uvs.size() == vertex_count;
	r_surface.has_uv2 = uv2s.size() == vertex_count;
	int stride = (r_surface.has_normal ? 3 : 0) + (r_surface.has_tangent ? 4 : 0) + (r_surface.has_color ? 4 : 0) + (r_surface.has_uv ? 2 : 0) + (r_surface.has_uv2 ? 2 : 0);

	// Float custom channels blend; packed ones (bytes, halves) are copied from a vertex.
	r_surface.copy_source.resize(Mesh::ARRAY_MAX);
	bool needs_copy = false;
	for (int c = 0; c < QuickMeshBrush3D::CUSTOM_CHANNELS; c++) {
		r_surface.custom_float_count[c] = 0;
		r_surface.custom_byte_size[c] = 0;
		const Variant custom = p_arrays[Mesh::ARRAY_CUSTOM0 + c];
		if (custom.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
			const PackedFloat32Array floats = custom;
			if (floats.size() > 0 && floats.size() % vertex_count == 0) {
				r_surface.custom_float_count[c] = floats.size() / vertex_count;
				stride += r_surface.custom_float_count[c];
			}
		} else if (custom.get_type() == Variant::PACKED_BYTE_ARRAY) {
			const PackedByteArray bytes = custom;
			if (bytes.size() > 0 && bytes.size() % vertex_count == 0) {
				r_surface.custom_byte_size[c] = bytes.size() / vertex_count;
				r_surface.copy_source[Mesh::ARRAY_CUSTOM0 + c] = bytes;
				needs_copy = true;
			}
		}
	}
	const PackedInt32Array bones = p_arrays[Mesh::ARRAY_BONES];
	const PackedFloat32Array weights = p_arrays[Mesh::ARRAY_WEIGHTS];
	r_surface.bone_stride = 0;
	if (bones.size() > 0 && bones.size() % vertex_count == 0 && weights.size() == bones.size()) {
		r_surface.bone_stride = bones.size() / vertex_count;
		r_surface.copy_source[Mesh::ARRAY_BONES] = bones;
		r_surface.copy_source[Mesh::ARRAY_WEIGHTS] = weights;
		needs_copy = true;
	}
	r_surface.blend_stride = stride;
	r_surface.blend.resize(vertex_count * stride);
	for (int i = 0; i < vertex_count; i++) {
		float *w = r_surface.blend.ptr() + i * stride;
		if (r_surface.has_normal) {
			*w++ = normals[i].x;
			*w++ = normals[i].y;
			*w++ = normals[i].z;
		}
		if (r_surface.has_tangent) {
			for (int k = 0; k < 4; k++) {
				*w++ = tangents[i * 4 + k];
			}
		}
		if (r_surface.has_color) {
			*w++ = colors[i].r;
			*w++ = colors[i].g;
			*w++ = colors[i].b;
			*w++ = colors[i].a;
		}
		if (r_surface.has_uv) {
			*w++ = uvs[i].x;
			*w++ = uvs[i].y;
		}
		if (r_surface.has_uv2) {
			*w++ = uv2s[i].x;
			*w++ = uv2s[i].y;
		}
		for (int c = 0; c < QuickMeshBrush3D::CUSTOM_CHANNELS; c++) {
			const int count = r_surface.custom_float_count[c];
			if (count > 0) {
				const PackedFloat32Array floats = p_arrays[Mesh::ARRAY_CUSTOM0 + c];
				for (int k = 0; k < count; k++) {
					*w++ = floats[i * count + k];
				}
			}
		}
	}
	if (needs_copy) {
		r_surface.copy_from.resize(vertex_count);
		for (int i = 0; i < vertex_count; i++) {
			r_surface.copy_from[i] = i;
		}
	}

	// Where to add detail: around each point on the unbent surface, edges shorter than the
	// point's radius divided by detail.
	struct DetailArea {
		Vector3 center;
		real_t reach = 0.0;
		real_t target_squared = 0.0;
	};
	LocalVector<DetailArea> areas;
	for (const BrushPoint &point : points) {
		if (point.radius <= CMP_EPSILON || detail <= 0.0) {
			continue;
		}
		const real_t target = MAX(point.radius / detail, (real_t)0.0005);
		DetailArea area;
		area.center = point.surface_position;
		area.reach = point.radius + target * 0.5;
		area.target_squared = target * target;
		areas.push_back(area);
	}

	// Split marked edges at their midpoints, red-green style, so the mesh stays watertight
	// (the same edge on both sides of a UV seam is split at the same position).
	for (int iteration = 0; iteration < 24 && !areas.is_empty(); iteration++) {
		// Decide by position, not by vertex index: the two sides of a UV seam are different
		// vertices at the same spot, and must split the same edges or the seam cracks.
		HashMap<Vector3, int> first_at_position;
		LocalVector<int> weld;
		weld.resize(r_surface.positions.size());
		for (uint32_t i = 0; i < r_surface.positions.size(); i++) {
			HashMap<Vector3, int>::Iterator it = first_at_position.find(r_surface.positions[i]);
			if (it) {
				weld[i] = it->value;
			} else {
				first_at_position.insert(r_surface.positions[i], i);
				weld[i] = i;
			}
		}
		const uint32_t triangle_count = r_surface.indices.size() / 3;
		HashSet<uint64_t> marked;
		for (uint32_t t = 0; t < triangle_count; t++) {
			const int *v = r_surface.indices.ptr() + t * 3;
			const Face3 face(r_surface.positions[v[0]], r_surface.positions[v[1]], r_surface.positions[v[2]]);
			for (const DetailArea &area : areas) {
				// The whole triangle, not just its edges: a small area can sit inside a big one.
				if (face.get_closest_point_to(area.center).distance_to(area.center) >= area.reach) {
					continue;
				}
				for (int e = 0; e < 3; e++) {
					if (face.vertex[e].distance_squared_to(face.vertex[(e + 1) % 3]) > area.target_squared) {
						marked.insert(edge_key(weld[v[e]], weld[v[(e + 1) % 3]]));
					}
				}
			}
		}
		HashMap<uint64_t, int> midpoints; // Per vertex index edge.
		if (!marked.is_empty()) {
			for (uint32_t t = 0; t < triangle_count; t++) {
				for (int e = 0; e < 3; e++) {
					const int a = r_surface.indices[t * 3 + e];
					const int b = r_surface.indices[t * 3 + (e + 1) % 3];
					if (marked.has(edge_key(weld[a], weld[b]))) {
						midpoints.insert(edge_key(a, b), -1);
					}
				}
			}
		}
		if (midpoints.is_empty()) {
			break;
		}
		int added = 0;
		for (uint32_t t = 0; t < triangle_count; t++) {
			for (int e = 0; e < 3; e++) {
				if (midpoints.has(edge_key(r_surface.indices[t * 3 + e], r_surface.indices[t * 3 + (e + 1) % 3]))) {
					added++;
				}
			}
		}
		if (added > r_budget) {
			budget_reached = true;
			break;
		}
		r_budget -= added;

		// New vertices at the midpoints. (a + b) * 0.5 is the same whichever end comes first.
		for (KeyValue<uint64_t, int> &E : midpoints) {
			const int a = int(E.key >> 32);
			const int b = int(E.key & 0xffffffff);
			E.value = r_surface.positions.size();
			r_surface.positions.push_back((r_surface.positions[a] + r_surface.positions[b]) * 0.5);
			const uint32_t first = r_surface.blend.size();
			r_surface.blend.resize(first + stride);
			for (int k = 0; k < stride; k++) {
				r_surface.blend[first + k] = (r_surface.blend[a * stride + k] + r_surface.blend[b * stride + k]) * 0.5f;
			}
			if (needs_copy) {
				const int from = r_surface.copy_from[a]; // Not a reference: push_back may reallocate.
				r_surface.copy_from.push_back(from);
			}
		}

		LocalVector<int> split;
		split.reserve((triangle_count + added) * 3);
		for (uint32_t t = 0; t < triangle_count; t++) {
			int v[3] = { r_surface.indices[t * 3], r_surface.indices[t * 3 + 1], r_surface.indices[t * 3 + 2] };
			int m[3]; // Midpoint of edge v[i] -> v[i + 1], or -1.
			int marked = 0;
			for (int e = 0; e < 3; e++) {
				const HashMap<uint64_t, int>::Iterator it = midpoints.find(edge_key(v[e], v[(e + 1) % 3]));
				m[e] = it ? it->value : -1;
				marked += m[e] >= 0 ? 1 : 0;
			}
			if (marked == 0) {
				split.push_back(v[0]);
				split.push_back(v[1]);
				split.push_back(v[2]);
			} else if (marked == 3) {
				const int tris[4][3] = { { v[0], m[0], m[2] }, { m[0], v[1], m[1] }, { m[2], m[1], v[2] }, { m[0], m[1], m[2] } };
				for (const auto &tri : tris) {
					split.push_back(tri[0]);
					split.push_back(tri[1]);
					split.push_back(tri[2]);
				}
			} else if (marked == 1) {
				// Rotate so edge 0 is the marked one.
				int r = m[0] >= 0 ? 0 : (m[1] >= 0 ? 1 : 2);
				const int a = v[r], b = v[(r + 1) % 3], c = v[(r + 2) % 3], mab = m[r];
				split.push_back(a);
				split.push_back(mab);
				split.push_back(c);
				split.push_back(mab);
				split.push_back(b);
				split.push_back(c);
			} else {
				// Rotate so edge 2 (c -> a) is the one left whole.
				int r = m[0] < 0 ? 1 : (m[1] < 0 ? 2 : 0);
				const int a = v[r], b = v[(r + 1) % 3], c = v[(r + 2) % 3];
				const int mab = m[r], mbc = m[(r + 1) % 3];
				split.push_back(mab);
				split.push_back(b);
				split.push_back(mbc);
				// Split the quad a, mab, mbc, c along its shorter diagonal.
				if (r_surface.positions[a].distance_squared_to(r_surface.positions[mbc]) < r_surface.positions[mab].distance_squared_to(r_surface.positions[c])) {
					split.push_back(a);
					split.push_back(mab);
					split.push_back(mbc);
					split.push_back(a);
					split.push_back(mbc);
					split.push_back(c);
				} else {
					split.push_back(a);
					split.push_back(mab);
					split.push_back(c);
					split.push_back(mab);
					split.push_back(mbc);
					split.push_back(c);
				}
			}
		}
		r_surface.indices = split;
	}

	// Weld by exact position: split edges on both sides of a seam land on the same spot.
	HashMap<Vector3, int> first_at;
	r_surface.weld.resize(r_surface.positions.size());
	for (uint32_t i = 0; i < r_surface.positions.size(); i++) {
		HashMap<Vector3, int>::Iterator it = first_at.find(r_surface.positions[i]);
		if (it) {
			r_surface.weld[i] = it->value;
		} else {
			first_at.insert(r_surface.positions[i], i);
			r_surface.weld[i] = i;
		}
	}
	welded_normals(r_surface.positions, r_surface.indices, r_surface.weld, r_surface.rest_normals);
}

void QuickMeshBrush3D::_bend() {
	if (source_mesh.is_null() || points.is_empty()) {
		for (Surface &surface : surfaces) {
			surface.bent = surface.positions;
		}
		MeshInstance3D::set_mesh(source_mesh);
		return;
	}
	if (result.is_null()) {
		result.instantiate();
	}

	const LocalVector<PreparedPoint> prepared = prepare_points(points);
	result->clear_surfaces();
	for (Surface &surface : surfaces) {
		if (surface.primitive != Mesh::PRIMITIVE_TRIANGLES) {
			if (!surface.passthrough.is_empty()) {
				result->add_surface_from_arrays(surface.primitive, surface.passthrough);
				result->surface_set_material(result->get_surface_count() - 1, surface.material);
				result->surface_set_name(result->get_surface_count() - 1, surface.name);
			}
			continue;
		}
		const int vertex_count = surface.positions.size();
		if (vertex_count == 0 || surface.indices.is_empty()) {
			continue;
		}

		surface.bent.resize(vertex_count);
		for (int i = 0; i < vertex_count; i++) {
			const int root = surface.weld[i];
			surface.bent[i] = root < i ? surface.bent[root] : bend_position(prepared, surface.positions[i]);
		}
		LocalVector<Vector3> bent_normals;
		welded_normals(surface.bent, surface.indices, surface.weld, bent_normals);

		const int stride = surface.blend_stride;
		PackedVector3Array vertices;
		vertices.resize(vertex_count);
		PackedVector3Array normals;
		PackedFloat32Array tangents;
		PackedColorArray colors;
		PackedVector2Array uvs, uv2s;
		if (surface.has_normal) {
			normals.resize(vertex_count);
		}
		if (surface.has_tangent) {
			tangents.resize(vertex_count * 4);
		}
		if (surface.has_color) {
			colors.resize(vertex_count);
		}
		if (surface.has_uv) {
			uvs.resize(vertex_count);
		}
		if (surface.has_uv2) {
			uv2s.resize(vertex_count);
		}
		PackedFloat32Array custom_floats[QuickMeshBrush3D::CUSTOM_CHANNELS];
		for (int c = 0; c < QuickMeshBrush3D::CUSTOM_CHANNELS; c++) {
			custom_floats[c].resize(vertex_count * surface.custom_float_count[c]);
		}

		for (int i = 0; i < vertex_count; i++) {
			vertices.set(i, surface.bent[i]);
			const float *r = surface.blend.ptr() + i * stride;
			// The authored normal turns with the surface; where nothing moved it stays exact.
			Quaternion turn;
			bool turned = false;
			const Vector3 &rest_n = surface.rest_normals[i];
			const Vector3 &bent_n = bent_normals[i];
			if (!rest_n.is_equal_approx(bent_n) && rest_n.length_squared() > 0.0 && bent_n.length_squared() > 0.0) {
				turn = Quaternion(rest_n.normalized(), bent_n.normalized());
				turned = true;
			}
			if (surface.has_normal) {
				Vector3 n(r[0], r[1], r[2]);
				n = turned ? turn.xform(n) : n;
				normals.set(i, n.normalized());
				r += 3;
			}
			if (surface.has_tangent) {
				Vector3 t(r[0], r[1], r[2]);
				t = (turned ? turn.xform(t) : t).normalized();
				tangents.set(i * 4 + 0, t.x);
				tangents.set(i * 4 + 1, t.y);
				tangents.set(i * 4 + 2, t.z);
				tangents.set(i * 4 + 3, r[3] < 0.0f ? -1.0f : 1.0f);
				r += 4;
			}
			if (surface.has_color) {
				colors.set(i, Color(r[0], r[1], r[2], r[3]));
				r += 4;
			}
			if (surface.has_uv) {
				uvs.set(i, Vector2(r[0], r[1]));
				r += 2;
			}
			if (surface.has_uv2) {
				uv2s.set(i, Vector2(r[0], r[1]));
				r += 2;
			}
			for (int c = 0; c < QuickMeshBrush3D::CUSTOM_CHANNELS; c++) {
				const int count = surface.custom_float_count[c];
				for (int k = 0; k < count; k++) {
					custom_floats[c].set(i * count + k, *r++);
				}
			}
		}

		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = vertices;
		if (surface.has_normal) {
			arrays[Mesh::ARRAY_NORMAL] = normals;
		}
		if (surface.has_tangent) {
			arrays[Mesh::ARRAY_TANGENT] = tangents;
		}
		if (surface.has_color) {
			arrays[Mesh::ARRAY_COLOR] = colors;
		}
		if (surface.has_uv) {
			arrays[Mesh::ARRAY_TEX_UV] = uvs;
		}
		if (surface.has_uv2) {
			arrays[Mesh::ARRAY_TEX_UV2] = uv2s;
		}
		const uint64_t flags = surface_flags(surface.format);
		for (int c = 0; c < QuickMeshBrush3D::CUSTOM_CHANNELS; c++) {
			if (surface.custom_float_count[c] > 0) {
				arrays[Mesh::ARRAY_CUSTOM0 + c] = custom_floats[c];
			} else if (surface.custom_byte_size[c] > 0) {
				const PackedByteArray from = surface.copy_source[Mesh::ARRAY_CUSTOM0 + c];
				const int size = surface.custom_byte_size[c];
				PackedByteArray bytes;
				bytes.resize(vertex_count * size);
				for (int i = 0; i < vertex_count; i++) {
					memcpy(bytes.ptrw() + i * size, from.ptr() + surface.copy_from[i] * size, size);
				}
				arrays[Mesh::ARRAY_CUSTOM0 + c] = bytes;
			}
		}
		if (surface.bone_stride > 0) {
			const PackedInt32Array from_bones = surface.copy_source[Mesh::ARRAY_BONES];
			const PackedFloat32Array from_weights = surface.copy_source[Mesh::ARRAY_WEIGHTS];
			const int size = surface.bone_stride;
			PackedInt32Array bones;
			PackedFloat32Array weights;
			bones.resize(vertex_count * size);
			weights.resize(vertex_count * size);
			for (int i = 0; i < vertex_count; i++) {
				for (int k = 0; k < size; k++) {
					bones.set(i * size + k, from_bones[surface.copy_from[i] * size + k]);
					weights.set(i * size + k, from_weights[surface.copy_from[i] * size + k]);
				}
			}
			arrays[Mesh::ARRAY_BONES] = bones;
			arrays[Mesh::ARRAY_WEIGHTS] = weights;
		}
		PackedInt32Array indices;
		indices.resize(surface.indices.size());
		memcpy(indices.ptrw(), surface.indices.ptr(), surface.indices.size() * sizeof(int));
		arrays[Mesh::ARRAY_INDEX] = indices;

		result->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), Dictionary(), flags);
		result->surface_set_material(result->get_surface_count() - 1, surface.material);
		result->surface_set_name(result->get_surface_count() - 1, surface.name);
	}
	if (get_mesh() != result) {
		MeshInstance3D::set_mesh(result);
	}
}

void QuickMeshBrush3D::_update_collision() {
	collision_dirty = false;
#ifndef PHYSICS_3D_DISABLED
	if (collision_shape.is_empty() || !is_inside_tree()) {
		return;
	}
	CollisionShape3D *shape_node = Object::cast_to<CollisionShape3D>(get_node_or_null(collision_shape));
	if (!shape_node || get_mesh().is_null()) {
		return;
	}
	// The faces go into the shape node's space.
	const Transform3D to_shape = shape_node->get_global_transform().affine_inverse() * get_global_transform();
	const Vector<Face3> faces = get_mesh()->get_faces();
	Vector<Vector3> vertices;
	vertices.resize(faces.size() * 3);
	Vector3 *w = vertices.ptrw();
	for (int i = 0; i < faces.size(); i++) {
		for (int k = 0; k < 3; k++) {
			w[i * 3 + k] = to_shape.xform(faces[i].vertex[k]);
		}
	}
	Ref<ConcavePolygonShape3D> shape = shape_node->get_shape();
	if (shape.is_null()) {
		shape.instantiate();
		shape_node->set_shape(shape);
	}
	shape->set_faces(vertices);
#endif // PHYSICS_3D_DISABLED
}

void QuickMeshBrush3D::rebuild() {
	if (source_mesh.is_null()) {
		return;
	}
	bend_queued = false;
	if (refine_dirty || surfaces.is_empty()) {
		_refine();
	}
	_bend();
	_update_collision();
}

bool QuickMeshBrush3D::has_edits() const {
	for (const BrushPoint &point : points) {
		if (!point.rest.is_equal_approx(point.transform)) {
			return true;
		}
	}
	return false;
}

int QuickMeshBrush3D::get_triangle_count() const {
	const Ref<Mesh> shown = get_mesh();
	if (shown.is_null()) {
		return 0;
	}
	int count = 0;
	for (int i = 0; i < shown->get_surface_count(); i++) {
		const int index_len = shown->surface_get_array_index_len(i);
		count += (index_len > 0 ? index_len : shown->surface_get_array_len(i)) / 3;
	}
	return count;
}

// Bakes the bend into a new source mesh and starts again with no points.
void QuickMeshBrush3D::apply_edits() {
	rebuild();
	if (points.is_empty() || result.is_null()) {
		return;
	}
	Ref<ArrayMesh> baked;
	baked.instantiate();
	for (int i = 0; i < result->get_surface_count(); i++) {
		baked->add_surface_from_arrays(result->surface_get_primitive_type(i), result->surface_get_arrays(i), TypedArray<Array>(), Dictionary(), surface_flags(result->surface_get_format(i)));
		baked->surface_set_material(i, result->surface_get_material(i));
		baked->surface_set_name(i, result->surface_get_name(i));
	}
	points.clear();
	set_source_mesh(baked);
	rebuild();
	notify_property_list_changed();
}

Dictionary QuickMeshBrush3D::intersect_ray(const Vector3 &p_from, const Vector3 &p_dir) const {
	Dictionary hit;
	real_t best = Math::INF;
	for (const Surface &surface : surfaces) {
		if (surface.primitive != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		const LocalVector<Vector3> &shown = surface.bent.size() == surface.positions.size() ? surface.bent : surface.positions;
		for (uint32_t i = 0; i + 2 < surface.indices.size(); i += 3) {
			const int a = surface.indices[i], b = surface.indices[i + 1], c = surface.indices[i + 2];
			Vector3 point;
			if (!Geometry3D::ray_intersects_triangle(p_from, p_dir, shown[a], shown[b], shown[c], &point)) {
				continue;
			}
			const real_t distance = p_from.distance_squared_to(point);
			if (distance >= best) {
				continue;
			}
			best = distance;
			const Vector3 bary = Geometry3D::triangle_get_barycentric_coords(shown[a], shown[b], shown[c], point);
			Vector3 normal = (shown[b] - shown[a]).cross(shown[c] - shown[a]).normalized();
			if (normal.dot(p_dir) > 0.0) {
				normal = -normal;
			}
			hit["position"] = point;
			hit["normal"] = normal;
			hit["surface_position"] = surface.positions[a] * bary.x + surface.positions[b] * bary.y + surface.positions[c] * bary.z;
		}
	}
	return hit;
}

// Points.

int QuickMeshBrush3D::add_point(const Vector3 &p_position, const Vector3 &p_normal, real_t p_radius, real_t p_hardness) {
	BrushPoint point;
	point.rest = Transform3D(surface_basis(p_normal), p_position);
	point.transform = point.rest;
	point.radius = MAX(p_radius, (real_t)0.001);
	point.hardness = CLAMP(p_hardness, (real_t)0.0, (real_t)1.0);
	point.surface_position = p_position;
	// On a bent area, detail goes where this spot was on the unbent mesh.
	const Vector3 normal = p_normal.normalized();
	if (!normal.is_zero_approx()) {
		const real_t lift = point.radius * 0.05 + 0.001;
		const Dictionary hit = intersect_ray(p_position + normal * lift, -normal);
		if (!hit.is_empty() && Vector3(hit["position"]).distance_to(p_position) < lift * 2.0) {
			point.surface_position = hit["surface_position"];
		}
	}
	points.push_back(point);
	notify_property_list_changed();
	_queue_refine();
	return points.size() - 1;
}

void QuickMeshBrush3D::remove_point(int p_index) {
	ERR_FAIL_INDEX(p_index, (int)points.size());
	points.remove_at(p_index);
	notify_property_list_changed();
	_queue_refine();
}

void QuickMeshBrush3D::clear_points() {
	points.clear();
	notify_property_list_changed();
	_queue_refine();
}

void QuickMeshBrush3D::set_point_count(int p_count) {
	ERR_FAIL_COND(p_count < 0);
	points.resize(p_count);
	notify_property_list_changed();
	_queue_refine();
}

int QuickMeshBrush3D::get_point_count() const {
	return points.size();
}

void QuickMeshBrush3D::set_point_transform(int p_index, const Transform3D &p_transform) {
	ERR_FAIL_INDEX(p_index, (int)points.size());
	points[p_index].transform = p_transform;
	_queue_bend();
}

Transform3D QuickMeshBrush3D::get_point_transform(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)points.size(), Transform3D());
	return points[p_index].transform;
}

void QuickMeshBrush3D::set_point_rest(int p_index, const Transform3D &p_rest) {
	ERR_FAIL_INDEX(p_index, (int)points.size());
	points[p_index].rest = p_rest;
	_queue_bend();
}

Transform3D QuickMeshBrush3D::get_point_rest(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)points.size(), Transform3D());
	return points[p_index].rest;
}

void QuickMeshBrush3D::set_point_surface_position(int p_index, const Vector3 &p_position) {
	ERR_FAIL_INDEX(p_index, (int)points.size());
	points[p_index].surface_position = p_position;
	_queue_refine();
}

Vector3 QuickMeshBrush3D::get_point_surface_position(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)points.size(), Vector3());
	return points[p_index].surface_position;
}

void QuickMeshBrush3D::set_point_radius(int p_index, real_t p_radius) {
	ERR_FAIL_INDEX(p_index, (int)points.size());
	points[p_index].radius = MAX(p_radius, (real_t)0.001);
	_queue_refine();
}

real_t QuickMeshBrush3D::get_point_radius(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)points.size(), 0.0);
	return points[p_index].radius;
}

void QuickMeshBrush3D::set_point_hardness(int p_index, real_t p_hardness) {
	ERR_FAIL_INDEX(p_index, (int)points.size());
	points[p_index].hardness = CLAMP(p_hardness, (real_t)0.0, (real_t)1.0);
	_queue_bend();
}

real_t QuickMeshBrush3D::get_point_hardness(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)points.size(), 0.0);
	return points[p_index].hardness;
}

void QuickMeshBrush3D::reset_point(int p_index) {
	ERR_FAIL_INDEX(p_index, (int)points.size());
	points[p_index].transform = points[p_index].rest;
	_queue_bend();
}

Array QuickMeshBrush3D::get_points_data() const {
	Array entries;
	for (const BrushPoint &point : points) {
		Dictionary d;
		d["rest"] = point.rest;
		d["transform"] = point.transform;
		d["surface_position"] = point.surface_position;
		d["radius"] = point.radius;
		d["hardness"] = point.hardness;
		entries.push_back(d);
	}
	return entries;
}

void QuickMeshBrush3D::set_points_data(const Array &p_data) {
	points.clear();
	for (const Variant &v : p_data) {
		const Dictionary d = v;
		BrushPoint point;
		point.rest = d.get("rest", Transform3D());
		point.transform = d.get("transform", point.rest);
		point.surface_position = d.get("surface_position", point.rest.origin);
		point.radius = d.get("radius", 1.0);
		point.hardness = d.get("hardness", 0.0);
		points.push_back(point);
	}
	notify_property_list_changed();
	_queue_refine();
}

// Settings.

void QuickMeshBrush3D::set_source_mesh(const Ref<Mesh> &p_mesh) {
	if (source_mesh == p_mesh) {
		return;
	}
	if (source_mesh.is_valid()) {
		source_mesh->disconnect_changed(callable_mp(this, &QuickMeshBrush3D::_queue_refine));
	}
	const Ref<Mesh> previous = source_mesh;
	source_mesh = p_mesh;
	if (source_mesh.is_valid()) {
		source_mesh->connect_changed(callable_mp(this, &QuickMeshBrush3D::_queue_refine));
	}
	if (_can_rebuild()) {
		_queue_refine();
	} else if (points.is_empty() && (get_mesh().is_null() || get_mesh() == previous)) {
		// Shows a new source when nothing bends it. A scene being loaded sets the saved,
		// bent mesh first, and that one stays.
		MeshInstance3D::set_mesh(source_mesh);
	}
	update_configuration_warnings();
}

Ref<Mesh> QuickMeshBrush3D::get_source_mesh() const {
	return source_mesh;
}

void QuickMeshBrush3D::set_detail(real_t p_detail) {
	detail = MAX(p_detail, (real_t)0.0);
	_queue_refine();
}

real_t QuickMeshBrush3D::get_detail() const {
	return detail;
}

void QuickMeshBrush3D::set_max_triangles(int p_count) {
	max_triangles = MAX(p_count, 0);
	_queue_refine();
}

int QuickMeshBrush3D::get_max_triangles() const {
	return max_triangles;
}

void QuickMeshBrush3D::set_update_at_runtime(bool p_enable) {
	update_at_runtime = p_enable;
	_queue_refine();
}

bool QuickMeshBrush3D::is_updating_at_runtime() const {
	return update_at_runtime;
}

void QuickMeshBrush3D::set_collision_shape(const NodePath &p_path) {
	collision_shape = p_path;
	update_configuration_warnings();
	_queue_collision();
}

NodePath QuickMeshBrush3D::get_collision_shape() const {
	return collision_shape;
}

// Inspector: points/<n>/... entries.

bool QuickMeshBrush3D::_set(const StringName &p_name, const Variant &p_value) {
	const String name = p_name;
	if (!name.begins_with("points/")) {
		return false;
	}
	const int index = name.get_slicec('/', 1).to_int();
	const String what = name.get_slicec('/', 2);
	if (index < 0) {
		return false;
	}
	if (index >= (int)points.size()) {
		// Scenes load entries before the count in some orders; grow to fit.
		points.resize(index + 1);
	}
	if (what == "transform") {
		set_point_transform(index, p_value);
	} else if (what == "rest") {
		set_point_rest(index, p_value);
	} else if (what == "surface_position") {
		set_point_surface_position(index, p_value);
	} else if (what == "radius") {
		set_point_radius(index, p_value);
	} else if (what == "hardness") {
		set_point_hardness(index, p_value);
	} else {
		return false;
	}
	return true;
}

bool QuickMeshBrush3D::_get(const StringName &p_name, Variant &r_ret) const {
	const String name = p_name;
	if (!name.begins_with("points/")) {
		return false;
	}
	const int index = name.get_slicec('/', 1).to_int();
	const String what = name.get_slicec('/', 2);
	if (index < 0 || index >= (int)points.size()) {
		return false;
	}
	const BrushPoint &point = points[index];
	if (what == "transform") {
		r_ret = point.transform;
	} else if (what == "rest") {
		r_ret = point.rest;
	} else if (what == "surface_position") {
		r_ret = point.surface_position;
	} else if (what == "radius") {
		r_ret = point.radius;
	} else if (what == "hardness") {
		r_ret = point.hardness;
	} else {
		return false;
	}
	return true;
}

void QuickMeshBrush3D::_get_property_list(List<PropertyInfo> *p_list) const {
	for (uint32_t i = 0; i < points.size(); i++) {
		const String path = "points/" + itos(i) + "/";
		p_list->push_back(PropertyInfo(Variant::TRANSFORM3D, path + "transform"));
		p_list->push_back(PropertyInfo(Variant::FLOAT, path + "radius", PROPERTY_HINT_RANGE, "0.001,100,0.001,or_greater,suffix:m"));
		p_list->push_back(PropertyInfo(Variant::FLOAT, path + "hardness", PROPERTY_HINT_RANGE, "0,1,0.01"));
		p_list->push_back(PropertyInfo(Variant::TRANSFORM3D, path + "rest", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE));
		p_list->push_back(PropertyInfo(Variant::VECTOR3, path + "surface_position", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE));
	}
}

void QuickMeshBrush3D::_validate_property(PropertyInfo &p_property) const {
	// The mesh is the result. Edit the source mesh, or the points.
	if (p_property.name == "mesh") {
		p_property.usage |= PROPERTY_USAGE_READ_ONLY;
	}
}

void QuickMeshBrush3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			// A MeshInstance3D changed to this type brings its mesh along: that is what gets bent.
			if (source_mesh.is_null() && get_mesh().is_valid()) {
				set_source_mesh(get_mesh());
			}
			if (_can_rebuild()) {
				// A fresh result each time, so a duplicated node never shares it with the original.
				result.unref();
				_queue_refine();
			}
		} break;
	}
}

PackedStringArray QuickMeshBrush3D::get_configuration_warnings() const {
	PackedStringArray warnings = MeshInstance3D::get_configuration_warnings();
	if (source_mesh.is_null()) {
		warnings.push_back(RTR("Set a source mesh to bend, or change a MeshInstance3D with a mesh to this type."));
	}
	if (budget_reached) {
		warnings.push_back(RTR("The triangle budget (max_triangles) was reached, so some areas got less detail. Raise max_triangles or lower detail."));
	}
	if (dropped_animation_data) {
		warnings.push_back(RTR("The source mesh has blend shapes, which the bent mesh doesn't keep."));
	}
#ifndef PHYSICS_3D_DISABLED
	if (!collision_shape.is_empty() && is_inside_tree() && !Object::cast_to<CollisionShape3D>(get_node_or_null(collision_shape))) {
		warnings.push_back(RTR("collision_shape must point to a CollisionShape3D."));
	}
#endif // PHYSICS_3D_DISABLED
	return warnings;
}

void QuickMeshBrush3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source_mesh", "mesh"), &QuickMeshBrush3D::set_source_mesh);
	ClassDB::bind_method(D_METHOD("get_source_mesh"), &QuickMeshBrush3D::get_source_mesh);
	ClassDB::bind_method(D_METHOD("set_detail", "detail"), &QuickMeshBrush3D::set_detail);
	ClassDB::bind_method(D_METHOD("get_detail"), &QuickMeshBrush3D::get_detail);
	ClassDB::bind_method(D_METHOD("set_max_triangles", "count"), &QuickMeshBrush3D::set_max_triangles);
	ClassDB::bind_method(D_METHOD("get_max_triangles"), &QuickMeshBrush3D::get_max_triangles);
	ClassDB::bind_method(D_METHOD("set_update_at_runtime", "enable"), &QuickMeshBrush3D::set_update_at_runtime);
	ClassDB::bind_method(D_METHOD("is_updating_at_runtime"), &QuickMeshBrush3D::is_updating_at_runtime);
	ClassDB::bind_method(D_METHOD("set_collision_shape", "path"), &QuickMeshBrush3D::set_collision_shape);
	ClassDB::bind_method(D_METHOD("get_collision_shape"), &QuickMeshBrush3D::get_collision_shape);

	ClassDB::bind_method(D_METHOD("set_point_count", "count"), &QuickMeshBrush3D::set_point_count);
	ClassDB::bind_method(D_METHOD("get_point_count"), &QuickMeshBrush3D::get_point_count);
	ClassDB::bind_method(D_METHOD("add_point", "position", "normal", "radius", "hardness"), &QuickMeshBrush3D::add_point, DEFVAL(1.0), DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("remove_point", "index"), &QuickMeshBrush3D::remove_point);
	ClassDB::bind_method(D_METHOD("clear_points"), &QuickMeshBrush3D::clear_points);
	ClassDB::bind_method(D_METHOD("set_point_transform", "index", "transform"), &QuickMeshBrush3D::set_point_transform);
	ClassDB::bind_method(D_METHOD("get_point_transform", "index"), &QuickMeshBrush3D::get_point_transform);
	ClassDB::bind_method(D_METHOD("set_point_rest", "index", "rest"), &QuickMeshBrush3D::set_point_rest);
	ClassDB::bind_method(D_METHOD("get_point_rest", "index"), &QuickMeshBrush3D::get_point_rest);
	ClassDB::bind_method(D_METHOD("set_point_surface_position", "index", "position"), &QuickMeshBrush3D::set_point_surface_position);
	ClassDB::bind_method(D_METHOD("get_point_surface_position", "index"), &QuickMeshBrush3D::get_point_surface_position);
	ClassDB::bind_method(D_METHOD("set_point_radius", "index", "radius"), &QuickMeshBrush3D::set_point_radius);
	ClassDB::bind_method(D_METHOD("get_point_radius", "index"), &QuickMeshBrush3D::get_point_radius);
	ClassDB::bind_method(D_METHOD("set_point_hardness", "index", "hardness"), &QuickMeshBrush3D::set_point_hardness);
	ClassDB::bind_method(D_METHOD("get_point_hardness", "index"), &QuickMeshBrush3D::get_point_hardness);
	ClassDB::bind_method(D_METHOD("reset_point", "index"), &QuickMeshBrush3D::reset_point);
	ClassDB::bind_method(D_METHOD("get_points_data"), &QuickMeshBrush3D::get_points_data);
	ClassDB::bind_method(D_METHOD("set_points_data", "data"), &QuickMeshBrush3D::set_points_data);

	ClassDB::bind_method(D_METHOD("intersect_ray", "from", "direction"), &QuickMeshBrush3D::intersect_ray);
	ClassDB::bind_method(D_METHOD("rebuild"), &QuickMeshBrush3D::rebuild);
	ClassDB::bind_method(D_METHOD("has_edits"), &QuickMeshBrush3D::has_edits);
	ClassDB::bind_method(D_METHOD("get_triangle_count"), &QuickMeshBrush3D::get_triangle_count);
	ClassDB::bind_method(D_METHOD("apply_edits"), &QuickMeshBrush3D::apply_edits);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "source_mesh", PROPERTY_HINT_RESOURCE_TYPE, Mesh::get_class_static()), "set_source_mesh", "get_source_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "detail", PROPERTY_HINT_RANGE, "0,64,0.1,or_greater"), "set_detail", "get_detail");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_triangles", PROPERTY_HINT_RANGE, "0,2000000,1,or_greater"), "set_max_triangles", "get_max_triangles");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "update_at_runtime"), "set_update_at_runtime", "is_updating_at_runtime");
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "collision_shape", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "CollisionShape3D"), "set_collision_shape", "get_collision_shape");
	ADD_ARRAY_COUNT("Points", "point_count", "set_point_count", "get_point_count", "points/");
}

QuickMeshBrush3D::QuickMeshBrush3D() {
}
