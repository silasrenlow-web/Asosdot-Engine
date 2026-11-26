/**************************************************************************/
/*  realtime_gi.cpp                                                       */
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

#include "realtime_gi.h"

#include "core/config/project_settings.h"
#include "core/io/resource_saver.h"
#include "core/math/geometry_2d.h"
#include "core/math/static_raycaster.h"
#include "core/object/class_db.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/os.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/sort_array.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

///////////////////////////////////////////////////////////////////////////////
// RealtimeGIData

void RealtimeGIData::add_user(const NodePath &p_path, const Rect2 &p_uv_scale, int32_t p_sub_instance, int32_t p_mesh) {
	User user;
	user.path = p_path;
	user.uv_scale = p_uv_scale;
	user.sub_instance = p_sub_instance;
	user.mesh = p_mesh;
	users.push_back(user);
}

int RealtimeGIData::add_mesh(const Ref<Mesh> &p_mesh) {
	meshes.push_back(p_mesh);
	return meshes.size() - 1;
}

Ref<Mesh> RealtimeGIData::get_user_mesh(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), Ref<Mesh>());
	int mesh = users[p_user].mesh;
	return mesh >= 0 && mesh < meshes.size() ? meshes[mesh] : Ref<Mesh>();
}

void RealtimeGIData::_set_meshes(const Array &p_meshes) {
	meshes.clear();
	for (int i = 0; i < p_meshes.size(); i++) {
		meshes.push_back(p_meshes[i]);
	}
}

Array RealtimeGIData::_get_meshes() const {
	Array ret;
	for (const Ref<Mesh> &mesh : meshes) {
		ret.push_back(mesh);
	}
	return ret;
}

int RealtimeGIData::get_user_count() const {
	return users.size();
}

NodePath RealtimeGIData::get_user_path(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), NodePath());
	return users[p_user].path;
}

int32_t RealtimeGIData::get_user_sub_instance(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), -1);
	return users[p_user].sub_instance;
}

Rect2 RealtimeGIData::get_user_lightmap_uv_scale(int p_user) const {
	ERR_FAIL_INDEX_V(p_user, users.size(), Rect2());
	return users[p_user].uv_scale;
}

void RealtimeGIData::clear_users() {
	users.clear();
	meshes.clear();
}

void RealtimeGIData::_set_user_data(const Array &p_data) {
	ERR_FAIL_COND(p_data.size() % 4 != 0);
	users.clear();
	for (int i = 0; i < p_data.size(); i += 4) {
		add_user(p_data[i], p_data[i + 1], p_data[i + 2], p_data[i + 3]);
	}
}

Array RealtimeGIData::_get_user_data() const {
	Array ret;
	for (const User &user : users) {
		ret.push_back(user.path);
		ret.push_back(user.uv_scale);
		ret.push_back(user.sub_instance);
		ret.push_back(user.mesh);
	}
	return ret;
}

void RealtimeGIData::set_surfaces(const Vector2i &p_atlas_size, float p_patch_size, const PackedFloat32Array &p_patches, const PackedInt32Array &p_cluster_offsets, const PackedInt32Array &p_texel_patches) {
	atlas_size = p_atlas_size;
	patch_size = p_patch_size;
	patches = p_patches;
	cluster_offsets = p_cluster_offsets;
	texel_patches = p_texel_patches;
	emit_changed();
}

void RealtimeGIData::set_transport(int p_ray_count, const PackedInt32Array &p_offsets, const PackedInt32Array &p_entries, const PackedFloat32Array &p_sky_visibility) {
	transport_rays = p_ray_count;
	transport_offsets = p_offsets;
	transport_entries = p_entries;
	sky_visibility = p_sky_visibility;
	emit_changed();
}

void RealtimeGIData::set_geometry(const AABB &p_bounds, const PackedInt32Array &p_bvh, const PackedFloat32Array &p_triangles) {
	bounds = p_bounds;
	bvh = p_bvh;
	triangles = p_triangles;
	emit_changed();
}

void RealtimeGIData::set_probes(const PackedVector3Array &p_points, int p_ray_count, const PackedInt32Array &p_rays, const PackedInt32Array &p_tetrahedra, const PackedInt32Array &p_bsp_tree) {
	probe_points = p_points;
	probe_rays = p_ray_count;
	probe_ray_hits = p_rays;
	probe_tetrahedra = p_tetrahedra;
	probe_bsp_tree = p_bsp_tree;
	emit_changed();
}

// Saved as four dictionaries rather than twenty properties, since they are
// only ever written whole by a bake.
void RealtimeGIData::_set_surface_data(const Dictionary &p_data) {
	atlas_size = p_data.get("atlas_size", Vector2i());
	patch_size = p_data.get("patch_size", 0.5);
	patches = p_data.get("patches", PackedFloat32Array());
	cluster_offsets = p_data.get("cluster_offsets", PackedInt32Array());
	texel_patches = p_data.get("texel_patches", PackedInt32Array());
}

Dictionary RealtimeGIData::_get_surface_data() const {
	Dictionary d;
	d["atlas_size"] = atlas_size;
	d["patch_size"] = patch_size;
	d["patches"] = patches;
	d["cluster_offsets"] = cluster_offsets;
	d["texel_patches"] = texel_patches;
	return d;
}

void RealtimeGIData::_set_transport_data(const Dictionary &p_data) {
	transport_rays = p_data.get("rays", 0);
	transport_offsets = p_data.get("offsets", PackedInt32Array());
	transport_entries = p_data.get("entries", PackedInt32Array());
	sky_visibility = p_data.get("sky_visibility", PackedFloat32Array());
}

Dictionary RealtimeGIData::_get_transport_data() const {
	Dictionary d;
	d["rays"] = transport_rays;
	d["offsets"] = transport_offsets;
	d["entries"] = transport_entries;
	d["sky_visibility"] = sky_visibility;
	return d;
}

void RealtimeGIData::_set_geometry_data(const Dictionary &p_data) {
	bounds = p_data.get("bounds", AABB());
	bvh = p_data.get("bvh", PackedInt32Array());
	triangles = p_data.get("triangles", PackedFloat32Array());
}

Dictionary RealtimeGIData::_get_geometry_data() const {
	Dictionary d;
	d["bounds"] = bounds;
	d["bvh"] = bvh;
	d["triangles"] = triangles;
	return d;
}

void RealtimeGIData::_set_probe_data(const Dictionary &p_data) {
	probe_points = p_data.get("points", PackedVector3Array());
	probe_rays = p_data.get("rays", 0);
	probe_ray_hits = p_data.get("ray_hits", PackedInt32Array());
	probe_tetrahedra = p_data.get("tetrahedra", PackedInt32Array());
	probe_bsp_tree = p_data.get("bsp_tree", PackedInt32Array());
}

Dictionary RealtimeGIData::_get_probe_data() const {
	Dictionary d;
	d["points"] = probe_points;
	d["rays"] = probe_rays;
	d["ray_hits"] = probe_ray_hits;
	d["tetrahedra"] = probe_tetrahedra;
	d["bsp_tree"] = probe_bsp_tree;
	return d;
}

void RealtimeGIData::apply(RID p_realtime_gi) const {
	RenderingServer *rs = RenderingServer::get_singleton();
	rs->realtime_gi_set_surfaces(p_realtime_gi, atlas_size, patches, cluster_offsets, texel_patches);
	rs->realtime_gi_set_transport(p_realtime_gi, transport_rays, transport_offsets, transport_entries, sky_visibility);
	rs->realtime_gi_set_geometry(p_realtime_gi, bounds, bvh, triangles);
	rs->realtime_gi_set_probes(p_realtime_gi, probe_points, probe_rays, probe_ray_hits, probe_tetrahedra, probe_bsp_tree);
}

void RealtimeGIData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_set_user_data", "data"), &RealtimeGIData::_set_user_data);
	ClassDB::bind_method(D_METHOD("_get_user_data"), &RealtimeGIData::_get_user_data);
	ClassDB::bind_method(D_METHOD("_set_meshes", "meshes"), &RealtimeGIData::_set_meshes);
	ClassDB::bind_method(D_METHOD("_get_meshes"), &RealtimeGIData::_get_meshes);
	ClassDB::bind_method(D_METHOD("get_generated_mesh_count"), &RealtimeGIData::get_generated_mesh_count);
	ClassDB::bind_method(D_METHOD("_set_surface_data", "data"), &RealtimeGIData::_set_surface_data);
	ClassDB::bind_method(D_METHOD("_get_surface_data"), &RealtimeGIData::_get_surface_data);
	ClassDB::bind_method(D_METHOD("_set_transport_data", "data"), &RealtimeGIData::_set_transport_data);
	ClassDB::bind_method(D_METHOD("_get_transport_data"), &RealtimeGIData::_get_transport_data);
	ClassDB::bind_method(D_METHOD("_set_geometry_data", "data"), &RealtimeGIData::_set_geometry_data);
	ClassDB::bind_method(D_METHOD("_get_geometry_data"), &RealtimeGIData::_get_geometry_data);
	ClassDB::bind_method(D_METHOD("_set_probe_data", "data"), &RealtimeGIData::_set_probe_data);
	ClassDB::bind_method(D_METHOD("_get_probe_data"), &RealtimeGIData::_get_probe_data);

	ClassDB::bind_method(D_METHOD("get_user_count"), &RealtimeGIData::get_user_count);
	ClassDB::bind_method(D_METHOD("get_user_path", "user_idx"), &RealtimeGIData::get_user_path);
	ClassDB::bind_method(D_METHOD("clear_users"), &RealtimeGIData::clear_users);
	ClassDB::bind_method(D_METHOD("get_atlas_size"), &RealtimeGIData::get_atlas_size);
	ClassDB::bind_method(D_METHOD("get_patch_size"), &RealtimeGIData::get_patch_size);
	ClassDB::bind_method(D_METHOD("get_patch_count"), &RealtimeGIData::get_patch_count);
	ClassDB::bind_method(D_METHOD("get_cluster_count"), &RealtimeGIData::get_cluster_count);
	ClassDB::bind_method(D_METHOD("get_transport_entry_count"), &RealtimeGIData::get_transport_entry_count);
	ClassDB::bind_method(D_METHOD("get_triangle_count"), &RealtimeGIData::get_triangle_count);
	ClassDB::bind_method(D_METHOD("get_probe_count"), &RealtimeGIData::get_probe_count);
	ClassDB::bind_method(D_METHOD("get_bounds"), &RealtimeGIData::get_bounds);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "user_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_user_data", "_get_user_data");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "meshes", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_meshes", "_get_meshes");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "surface_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_surface_data", "_get_surface_data");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "transport_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_transport_data", "_get_transport_data");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "geometry_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_geometry_data", "_get_geometry_data");
	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "probe_data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR | PROPERTY_USAGE_INTERNAL), "_set_probe_data", "_get_probe_data");
}

///////////////////////////////////////////////////////////////////////////////
// RealtimeGI

struct RealtimeGI::BakeMesh {
	NodePath path;
	int32_t sub_instance = -1;
	Transform3D xform;
	Ref<Mesh> mesh; // what the node draws
	Ref<Material> material_override;
	Vector<Ref<Material>> surface_overrides;
	TypedArray<RID> overrides;
	bool can_unwrap = false;
	Ref<ArrayMesh> generated; // a copy with UV2, when the mesh had none
	bool receiver = false;

	// Triangles in the GI's space, three entries per triangle.
	LocalVector<Vector3> positions;
	LocalVector<Vector3> normals;
	LocalVector<Vector2> uv2;
	LocalVector<Color> surface_albedo; // per triangle, used when the albedo can't be rendered

	// Shadow casting triangles, simplified where the mesh has LODs.
	LocalVector<Vector3> trace_positions;

	float area = 0.0;
	float uv_area = 0.0;
	int side = 0;
	Point2i origin; // where its chart's inner area starts in the atlas
};

static Color _surface_albedo(const Ref<Material> &p_material) {
	Ref<BaseMaterial3D> base_material = p_material;
	if (base_material.is_valid()) {
		Color albedo = base_material->get_albedo();
		return albedo * (1.0 - base_material->get_metallic());
	}
	return Color(0.7, 0.7, 0.7);
}

void RealtimeGI::_find_meshes(Node *p_at_node, Vector<BakeMesh> &r_meshes) const {
	auto add_mesh = [&](const Ref<Mesh> &p_mesh, const Transform3D &p_xform, const NodePath &p_path, int32_t p_sub_instance, const Ref<Material> &p_override, const Vector<Ref<Material>> &p_surface_overrides, bool p_can_unwrap) {
		if (p_mesh.is_null() || p_mesh->get_surface_count() == 0) {
			return;
		}
		BakeMesh bm;
		bm.path = p_path;
		bm.sub_instance = p_sub_instance;
		bm.xform = p_xform;
		bm.mesh = p_mesh;
		bm.material_override = p_override;
		bm.surface_overrides = p_surface_overrides;
		bm.can_unwrap = p_can_unwrap;
		r_meshes.push_back(bm);
	};

	MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(p_at_node);
	if (mi && mi->get_gi_mode() == GeometryInstance3D::GI_MODE_STATIC && mi->is_visible_in_tree()) {
		Ref<Mesh> mesh = mi->get_mesh();
		if (mesh.is_valid()) {
			Vector<Ref<Material>> overrides;
			for (int i = 0; i < mesh->get_surface_count(); i++) {
				overrides.push_back(mi->get_surface_override_material(i));
			}
			// Skinned and morphing meshes change shape, so a lightmap can't
			// follow them; they still cast shadows.
			bool can_unwrap = mi->get_skin().is_null() && mesh->get_blend_shape_count() == 0;
			add_mesh(mesh, get_global_transform().affine_inverse() * mi->get_global_transform(), get_path_to(mi), -1, mi->get_material_override(), overrides, can_unwrap);
		}
	}

	Node3D *s = Object::cast_to<Node3D>(p_at_node);
	if (!mi && s && s->has_method("get_bake_meshes")) {
		Array bake_meshes = p_at_node->call("get_bake_meshes");
		if (bake_meshes.size() && (bake_meshes.size() & 1) == 0) {
			Transform3D xf = get_global_transform().affine_inverse() * s->get_global_transform();
			for (int i = 0; i < bake_meshes.size(); i += 2) {
				Ref<Mesh> mesh = bake_meshes[i];
				Transform3D mesh_xf = bake_meshes[i + 1];
				add_mesh(mesh, xf * mesh_xf, get_path_to(s), i / 2, Ref<Material>(), Vector<Ref<Material>>(), false);
			}
		}
	}

	for (int i = 0; i < p_at_node->get_child_count(); i++) {
		_find_meshes(p_at_node->get_child(i), r_meshes);
	}
}

// Reads a mesh's triangles into the GI's space. p_lit is the mesh the
// lightmap is sampled through, p_mesh's own or a copy given UV2; shadows come
// from the original, which keeps its LODs.
static void _extract_bake_mesh(RealtimeGI::BakeMesh &r_bm, const Ref<Mesh> &p_lit, float p_patch_size) {
	r_bm.positions.clear();
	r_bm.normals.clear();
	r_bm.uv2.clear();
	r_bm.surface_albedo.clear();
	r_bm.trace_positions.clear();
	r_bm.overrides.clear();
	r_bm.area = 0.0;
	r_bm.uv_area = 0.0;
	r_bm.receiver = true;

	const Transform3D &xform = r_bm.xform;
	for (int i = 0; i < p_lit->get_surface_count(); i++) {
		if (p_lit->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		uint64_t format = p_lit->surface_get_format(i);
		if (!(format & Mesh::ARRAY_FORMAT_TEX_UV2) || !(format & Mesh::ARRAY_FORMAT_NORMAL)) {
			r_bm.receiver = false;
		}
	}

	const Basis normal_xform = xform.basis.inverse().transposed();
	const float scale = xform.basis.get_scale().length() / Math::sqrt(3.0f);

	for (int i = 0; i < p_lit->get_surface_count(); i++) {
		Ref<Material> material = r_bm.material_override;
		if (material.is_null() && i < r_bm.surface_overrides.size()) {
			material = r_bm.surface_overrides[i];
		}
		r_bm.overrides.push_back(material.is_valid() ? material->get_rid() : RID());
		if (p_lit->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		if (material.is_null()) {
			material = p_lit->surface_get_material(i);
		}
		const Color albedo = _surface_albedo(material);

		Array arrays = p_lit->surface_get_arrays(i);
		PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
		PackedVector3Array normals = arrays[Mesh::ARRAY_NORMAL];
		PackedVector2Array uv2 = arrays[Mesh::ARRAY_TEX_UV2];
		PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
		const bool has_attributes = r_bm.receiver && normals.size() == vertices.size() && uv2.size() == vertices.size();

		const int triangle_count = indices.size() ? indices.size() / 3 : vertices.size() / 3;
		for (int t = 0; t < triangle_count; t++) {
			int idx[3];
			for (int k = 0; k < 3; k++) {
				idx[k] = indices.size() ? indices[t * 3 + k] : t * 3 + k;
			}
			Vector3 p[3];
			for (int k = 0; k < 3; k++) {
				p[k] = xform.xform(vertices[idx[k]]);
				r_bm.positions.push_back(p[k]);
				if (has_attributes) {
					r_bm.normals.push_back(normal_xform.xform(normals[idx[k]]).normalized());
					r_bm.uv2.push_back(uv2[idx[k]]);
				}
			}
			r_bm.surface_albedo.push_back(albedo);
			if (has_attributes) {
				r_bm.area += (p[1] - p[0]).cross(p[2] - p[0]).length() * 0.5;
				Vector2 a = uv2[idx[0]], b = uv2[idx[1]], c = uv2[idx[2]];
				r_bm.uv_area += Math::abs((b - a).cross(c - a)) * 0.5;
			}
		}
	}

	// Shadows only need the shape, so a simplified LOD will do as long as it
	// strays less than a fraction of a patch from the real mesh.
	const Ref<Mesh> &shape = r_bm.mesh;
	for (int i = 0; i < shape->get_surface_count(); i++) {
		if (shape->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		Array arrays = shape->surface_get_arrays(i);
		PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
		PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];

		Vector<int> trace_indices;
		RenderingServerTypes::SurfaceData surface = RenderingServer::get_singleton()->mesh_get_surface(shape->get_rid(), i);
		for (int l = surface.lods.size() - 1; l >= 0; l--) {
			if (surface.lods[l].edge_length * scale <= p_patch_size * 0.25) {
				const Vector<uint8_t> &data = surface.lods[l].index_data;
				if (surface.vertex_count <= 65536) {
					const uint16_t *r = reinterpret_cast<const uint16_t *>(data.ptr());
					for (int j = 0; j < data.size() / 2; j++) {
						trace_indices.push_back(r[j]);
					}
				} else {
					const uint32_t *r = reinterpret_cast<const uint32_t *>(data.ptr());
					for (int j = 0; j < data.size() / 4; j++) {
						trace_indices.push_back(r[j]);
					}
				}
				break;
			}
		}
		if (trace_indices.size() < 3) {
			if (indices.size()) {
				trace_indices = indices;
			} else {
				for (int v = 0; v < vertices.size(); v++) {
					trace_indices.push_back(v);
				}
			}
		}
		for (int j = 0; j + 2 < trace_indices.size(); j += 3) {
			for (int k = 0; k < 3; k++) {
				int v = trace_indices[j + k];
				ERR_CONTINUE(v >= vertices.size());
				r_bm.trace_positions.push_back(xform.xform(vertices[v]));
			}
		}
	}

	if (r_bm.receiver && (r_bm.area <= CMP_EPSILON || r_bm.uv_area <= CMP_EPSILON || r_bm.normals.size() != r_bm.positions.size() || r_bm.uv2.size() != r_bm.positions.size())) {
		r_bm.receiver = false;
	}
	if (!r_bm.receiver) {
		r_bm.normals.clear();
		r_bm.uv2.clear();
	}
}

// A copy of p_mesh with UV2 unwrapped by xatlas, for meshes imported without
// lightmap UVs. The copy keeps the surfaces and materials, so it draws the same.
static Ref<ArrayMesh> _unwrap_copy(const Ref<Mesh> &p_mesh, const Vector3 &p_scale, float p_texel_size) {
	Ref<ArrayMesh> copy;
	copy.instantiate();
	for (int i = 0; i < p_mesh->get_surface_count(); i++) {
		if (p_mesh->surface_get_primitive_type(i) != Mesh::PRIMITIVE_TRIANGLES) {
			return Ref<ArrayMesh>();
		}
		Array arrays = p_mesh->surface_get_arrays(i);
		if (arrays[Mesh::ARRAY_NORMAL].get_type() == Variant::NIL) {
			return Ref<ArrayMesh>();
		}
		arrays[Mesh::ARRAY_TEX_UV2] = Variant();
		copy->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
		copy->surface_set_material(i, p_mesh->surface_get_material(i));
	}
	copy->set_name(p_mesh->get_name());
	if (copy->lightmap_unwrap(Transform3D(Basis::from_scale(p_scale)), p_texel_size) != OK) {
		return Ref<ArrayMesh>();
	}
	return copy;
}

// Deterministic per-patch randomness, so a rebake of the same scene comes out
// the same.
static _FORCE_INLINE_ uint32_t _pcg_hash(uint32_t p_value) {
	uint32_t state = p_value * 747796405u + 2891336453u;
	uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

static _FORCE_INLINE_ float _random_float(uint32_t &r_seed) {
	r_seed = _pcg_hash(r_seed);
	return float(r_seed >> 8) / float(1 << 24);
}

static _FORCE_INLINE_ void _sh_basis(const Vector3 &d, float *r_c) {
	r_c[0] = 0.282095;
	r_c[1] = 0.488603 * d.y;
	r_c[2] = 0.488603 * d.z;
	r_c[3] = 0.488603 * d.x;
	r_c[4] = 1.092548 * d.x * d.y;
	r_c[5] = 1.092548 * d.y * d.z;
	r_c[6] = 0.315392 * (3.0 * d.z * d.z - 1.0);
	r_c[7] = 1.092548 * d.x * d.z;
	r_c[8] = 0.546274 * (d.x * d.x - d.y * d.y);
}

// Matches fibonacci_direction() in realtime_gi.glsl.
static _FORCE_INLINE_ Vector3 _fibonacci_direction(uint32_t p_index, uint32_t p_count) {
	float z = 1.0 - (2.0 * float(p_index) + 1.0) / float(p_count);
	float r = Math::sqrt(MAX(0.0f, 1.0f - z * z));
	float phi = 2.39996322972865332f * float(p_index);
	return Vector3(Math::cos(phi) * r, Math::sin(phi) * r, z);
}

struct RealtimeGI::Baker {
	Ref<StaticRaycaster> raycaster;
	const Vector<BakeMesh> *meshes = nullptr;

	Vector2i atlas_size;
	LocalVector<int32_t> texel_patches; // dilated
	LocalVector<int32_t> patch_cluster;

	// Per patch, written by the tracing tasks.
	LocalVector<Vector3> patch_position;
	LocalVector<Vector3> patch_normal;
	LocalVector<float> patch_radius;
	LocalVector<LocalVector<uint32_t>> patch_entries;
	LocalVector<float> patch_sky; // nine per patch

	uint32_t rays = 128;

	LocalVector<Vector3> probe_candidates;
	uint32_t probe_rays = 128;
	LocalVector<int32_t> probe_hits;
	LocalVector<uint8_t> probe_valid;

	SafeFlag cancel;

	enum {
		HIT_SKY = -1,
		HIT_BLOCKED = -2,
	};

	// The cluster a ray landed on, or a HIT_ constant.
	int32_t trace(const Vector3 &p_from, const Vector3 &p_dir) const {
		StaticRaycaster::Ray ray(p_from, p_dir, 0.0, 1e6);
		if (!raycaster->intersect(ray)) {
			return HIT_SKY;
		}
		if (ray.geomID >= uint32_t(meshes->size())) {
			return HIT_BLOCKED;
		}
		const BakeMesh &mesh = (*meshes)[ray.geomID];
		if (!mesh.receiver) {
			return HIT_BLOCKED; // Casts shadows but has no lightmap to bounce from.
		}
		uint32_t first = ray.primID * 3;
		ERR_FAIL_COND_V(first + 2 >= mesh.uv2.size(), HIT_BLOCKED);
		float w = 1.0 - ray.u - ray.v;
		Vector3 normal = mesh.normals[first] * w + mesh.normals[first + 1] * ray.u + mesh.normals[first + 2] * ray.v;
		if (normal.dot(p_dir) > 0.0) {
			return HIT_BLOCKED; // The back of a surface: the ray started inside something.
		}
		Vector2 uv = mesh.uv2[first] * w + mesh.uv2[first + 1] * ray.u + mesh.uv2[first + 2] * ray.v;
		int x = CLAMP(int(Math::floor(uv.x * mesh.side)), 0, mesh.side - 1) + mesh.origin.x;
		int y = CLAMP(int(Math::floor(uv.y * mesh.side)), 0, mesh.side - 1) + mesh.origin.y;
		int32_t patch = texel_patches[y * atlas_size.x + x];
		if (patch < 0) {
			return HIT_BLOCKED;
		}
		return patch_cluster[patch];
	}

	void trace_patch(uint32_t p_index, void *) {
		if (cancel.is_set()) {
			return;
		}
		const Vector3 n = patch_normal[p_index];
		const Vector3 t = (Math::abs(n.y) < 0.99 ? n.cross(Vector3(0, 1, 0)) : n.cross(Vector3(1, 0, 0))).normalized();
		const Vector3 b = n.cross(t);
		const float radius = patch_radius[p_index];
		const Vector3 origin = patch_position[p_index] + n * (0.01 + radius * 0.05);

		uint32_t seed = _pcg_hash(p_index * 9781u + 1u);
		const float offset_u = _random_float(seed);
		const float offset_v = _random_float(seed);

		LocalVector<uint32_t> hits;
		hits.reserve(rays);
		float sky[9] = {};
		for (uint32_t r = 0; r < rays; r++) {
			// An R2 low discrepancy sequence, shifted per patch: even coverage
			// of the hemisphere without every patch sampling it alike.
			float u1 = Math::fposmod(offset_u + 0.7548776662f * float(r + 1), 1.0f);
			float u2 = Math::fposmod(offset_v + 0.5698402910f * float(r + 1), 1.0f);
			float phi = Math::TAU * u1;
			float sr = Math::sqrt(u2);
			Vector3 dir = (t * Math::cos(phi) * sr + b * Math::sin(phi) * sr + n * Math::sqrt(MAX(0.0f, 1.0f - u2))).normalized();

			// Leave from anywhere on the patch, so it sees what its whole
			// footprint sees rather than what its center does.
			float ox = (_random_float(seed) - 0.5f) * radius;
			float oy = (_random_float(seed) - 0.5f) * radius;
			int32_t hit = trace(origin + t * ox + b * oy, dir);
			if (hit == HIT_SKY) {
				float c[9];
				_sh_basis(dir, c);
				for (int k = 0; k < 9; k++) {
					sky[k] += c[k];
				}
			} else if (hit >= 0) {
				hits.push_back(uint32_t(hit));
			}
		}

		hits.sort();
		LocalVector<uint32_t> &entries = patch_entries[p_index];
		for (uint32_t i = 0; i < hits.size();) {
			uint32_t j = i;
			while (j < hits.size() && hits[j] == hits[i]) {
				j++;
			}
			entries.push_back((hits[i] << 10) | MIN(j - i, 1023u));
			i = j;
		}
		for (int k = 0; k < 9; k++) {
			patch_sky[p_index * 9 + k] = sky[k] / float(rays);
		}
	}

	void trace_probe(uint32_t p_index, void *) {
		if (cancel.is_set()) {
			return;
		}
		uint32_t blocked = 0;
		for (uint32_t r = 0; r < probe_rays; r++) {
			int32_t hit = trace(probe_candidates[p_index], _fibonacci_direction(r, probe_rays));
			probe_hits[p_index * probe_rays + r] = hit;
			if (hit == HIT_BLOCKED) {
				blocked++;
			}
		}
		// A probe that sees mostly backfaces sits inside a wall or a rock.
		probe_valid[p_index] = blocked < probe_rays / 4;
	}
};

// A binary BVH over the shadow casting triangles, in the layout
// realtime_gi.glsl walks: eight 32 bit values per node, inner nodes followed
// by their left child.
struct RealtimeGIBVHBuilder {
	static constexpr uint32_t MAX_LEAF = 4;
	static constexpr int MAX_DEPTH = 30; // The shader's traversal stack holds 32.
	static constexpr int BINS = 12;

	LocalVector<AABB> bounds;
	LocalVector<Vector3> centers;
	LocalVector<uint32_t> order;
	LocalVector<int32_t> nodes;

	struct CenterSort {
		const Vector3 *centers = nullptr;
		int axis = 0;
		bool operator()(uint32_t p_a, uint32_t p_b) const { return centers[p_a][axis] < centers[p_b][axis]; }
	};

	static _FORCE_INLINE_ float surface_area(const AABB &p_aabb) {
		const Vector3 &s = p_aabb.size;
		return 2.0f * (s.x * s.y + s.y * s.z + s.z * s.x);
	}

	_FORCE_INLINE_ int bin_of(uint32_t p_triangle, int p_axis, float p_origin, float p_extent) const {
		return MIN(int((centers[p_triangle][p_axis] - p_origin) / p_extent * BINS), BINS - 1);
	}

	void build(uint32_t p_begin, uint32_t p_end, int p_depth) {
		uint32_t node = nodes.size() / 8;
		nodes.resize(nodes.size() + 8);

		AABB aabb = bounds[order[p_begin]];
		AABB center_bounds(centers[order[p_begin]], Vector3());
		for (uint32_t i = p_begin + 1; i < p_end; i++) {
			aabb.merge_with(bounds[order[i]]);
			center_bounds.expand_to(centers[order[i]]);
		}
		const Vector3 pad(1e-4, 1e-4, 1e-4);
		const Vector3 min_b = aabb.position - pad;
		const Vector3 max_b = aabb.get_end() + pad;
		float *f = reinterpret_cast<float *>(nodes.ptr() + node * 8);
		f[0] = min_b.x;
		f[1] = min_b.y;
		f[2] = min_b.z;
		f[4] = max_b.x;
		f[5] = max_b.y;
		f[6] = max_b.z;

		const uint32_t count = p_end - p_begin;
		const int axis = center_bounds.get_longest_axis_index();
		const float extent = center_bounds.size[axis];
		if (count <= MAX_LEAF || p_depth >= MAX_DEPTH || extent <= 1e-6) {
			nodes[node * 8 + 3] = p_begin;
			nodes[node * 8 + 7] = count;
			return;
		}

		// Binned surface area heuristic along the longest axis.
		AABB bin_bounds[BINS];
		uint32_t bin_count[BINS] = {};
		const float origin = center_bounds.position[axis];
		for (uint32_t i = p_begin; i < p_end; i++) {
			int bin = bin_of(order[i], axis, origin, extent);
			bin_bounds[bin] = bin_count[bin] ? bin_bounds[bin].merge(bounds[order[i]]) : bounds[order[i]];
			bin_count[bin]++;
		}
		float best_cost = Math::INF;
		int best_split = -1;
		for (int split = 1; split < BINS; split++) {
			AABB left, right;
			uint32_t left_count = 0, right_count = 0;
			for (int i = 0; i < BINS; i++) {
				if (!bin_count[i]) {
					continue;
				}
				if (i < split) {
					left = left_count ? left.merge(bin_bounds[i]) : bin_bounds[i];
					left_count += bin_count[i];
				} else {
					right = right_count ? right.merge(bin_bounds[i]) : bin_bounds[i];
					right_count += bin_count[i];
				}
			}
			if (!left_count || !right_count) {
				continue;
			}
			float cost = surface_area(left) * left_count + surface_area(right) * right_count;
			if (cost < best_cost) {
				best_cost = cost;
				best_split = split;
			}
		}

		uint32_t mid = p_begin;
		if (best_split > 0) {
			uint32_t j = p_end;
			while (mid < j) {
				if (bin_of(order[mid], axis, origin, extent) < best_split) {
					mid++;
				} else {
					SWAP(order[mid], order[j - 1]);
					j--;
				}
			}
		}
		if (mid == p_begin || mid == p_end) {
			// No useful split: halve by position instead.
			SortArray<uint32_t, CenterSort> sorter;
			sorter.compare.centers = centers.ptr();
			sorter.compare.axis = axis;
			sorter.sort(order.ptr() + p_begin, count);
			mid = (p_begin + p_end) / 2;
		}

		build(p_begin, mid, p_depth + 1);
		uint32_t right = nodes.size() / 8;
		build(mid, p_end, p_depth + 1);
		nodes[node * 8 + 3] = right;
		nodes[node * 8 + 7] = 0;
	}
};

static void _build_bvh(const LocalVector<Vector3> &p_positions, PackedInt32Array &r_nodes, PackedFloat32Array &r_triangles) {
	const uint32_t triangle_count = p_positions.size() / 3;
	RealtimeGIBVHBuilder builder;
	builder.bounds.resize(triangle_count);
	builder.centers.resize(triangle_count);
	builder.order.resize(triangle_count);
	for (uint32_t i = 0; i < triangle_count; i++) {
		AABB aabb(p_positions[i * 3], Vector3());
		aabb.expand_to(p_positions[i * 3 + 1]);
		aabb.expand_to(p_positions[i * 3 + 2]);
		builder.bounds[i] = aabb;
		builder.centers[i] = aabb.get_center();
		builder.order[i] = i;
	}
	if (triangle_count) {
		builder.build(0, triangle_count, 0);
	}

	r_nodes.resize(builder.nodes.size());
	memcpy(r_nodes.ptrw(), builder.nodes.ptr(), builder.nodes.size() * sizeof(int32_t));

	r_triangles.resize(triangle_count * 12);
	float *w = r_triangles.ptrw();
	for (uint32_t i = 0; i < triangle_count; i++) {
		for (int k = 0; k < 3; k++) {
			const Vector3 &v = p_positions[builder.order[i] * 3 + k];
			w[i * 12 + k * 4 + 0] = v.x;
			w[i * 12 + k * 4 + 1] = v.y;
			w[i * 12 + k * 4 + 2] = v.z;
			w[i * 12 + k * 4 + 3] = 0.0;
		}
	}
}

// Tetrahedra and the BSP tree that finds them, in the layout lightmap
// captures read, built straight from the probe grid. Every cell splits into
// the six tetrahedra that share its main diagonal, which tile space with no
// gaps; the tree splits cells along grid planes, then a cell along its three
// diagonal planes. A general Delaunay tetrahedralization of a few thousand
// probes takes minutes; this takes milliseconds.
struct RealtimeGIProbeGrid {
	Vector3i counts;
	Vector3 origin;
	Vector3 step;
	LocalVector<int32_t> tetrahedra;
	LocalVector<int32_t> nodes; // six values each: plane normal and d as floats, over, under

	int32_t point_index(const Vector3i &p_point) const {
		return (p_point.z * counts.y + p_point.y) * counts.x + p_point.x;
	}

	int32_t add_node() {
		uint32_t index = nodes.size() / 6;
		nodes.resize(nodes.size() + 6);
		return int32_t(index);
	}

	void set_node(int32_t p_node, const Vector3 &p_normal, float p_d, int32_t p_over, int32_t p_under) {
		float *f = reinterpret_cast<float *>(nodes.ptr() + p_node * 6);
		f[0] = p_normal.x;
		f[1] = p_normal.y;
		f[2] = p_normal.z;
		f[3] = p_d;
		nodes[p_node * 6 + 4] = p_over;
		nodes[p_node * 6 + 5] = p_under;
	}

	// The cell's tetrahedron where the cell-local coordinates order as
	// a > b > c, returned as a BSP leaf.
	int32_t add_tetrahedron(const Vector3i &p_cell, int p_a, int p_b) {
		int32_t leaf = -int32_t(tetrahedra.size() / 4) - 1;
		Vector3i v = p_cell;
		tetrahedra.push_back(point_index(v));
		v[p_a] += 1;
		tetrahedra.push_back(point_index(v));
		v[p_b] += 1;
		tetrahedra.push_back(point_index(v));
		tetrahedra.push_back(point_index(p_cell + Vector3i(1, 1, 1)));
		return leaf;
	}

	// Over the plane where u_a > u_b, u being cell-local coordinates.
	void set_diagonal(int32_t p_node, const Vector3i &p_cell, int p_a, int p_b, int32_t p_over, int32_t p_under) {
		Vector3 corner = origin + Vector3(p_cell) * step;
		Vector3 normal;
		normal[p_a] = 1.0 / step[p_a];
		normal[p_b] = -1.0 / step[p_b];
		set_node(p_node, normal, corner[p_a] / step[p_a] - corner[p_b] / step[p_b], p_over, p_under);
	}

	int32_t build_cell(const Vector3i &p_cell) {
		const int X = 0, Y = 1, Z = 2;
		// Parents come before children, as the capture lookup expects.
		int32_t xy = add_node();
		int32_t yz = add_node();
		int32_t xz_upper = add_node();
		int32_t xz_lower = add_node();
		int32_t yz_lower = add_node();
		set_diagonal(xy, p_cell, X, Y, yz, xz_lower);
		set_diagonal(yz, p_cell, Y, Z, add_tetrahedron(p_cell, X, Y), xz_upper); // x > y > z
		set_diagonal(xz_upper, p_cell, X, Z, add_tetrahedron(p_cell, X, Z), add_tetrahedron(p_cell, Z, X)); // x > z >= y, z >= x > y
		set_diagonal(xz_lower, p_cell, X, Z, add_tetrahedron(p_cell, Y, X), yz_lower); // y >= x > z
		set_diagonal(yz_lower, p_cell, Y, Z, add_tetrahedron(p_cell, Y, Z), add_tetrahedron(p_cell, Z, Y)); // y > z >= x, z >= y >= x
		return xy;
	}

	// Halves the cell range along its longest side until one cell is left;
	// the depth is the log of the cell count.
	int32_t build(const Vector3i &p_from, const Vector3i &p_to) {
		Vector3i size = p_to - p_from;
		if (size == Vector3i(1, 1, 1)) {
			return build_cell(p_from);
		}
		int axis = size.x >= size.y && size.x >= size.z ? 0 : (size.y >= size.z ? 1 : 2);
		int mid = p_from[axis] + size[axis] / 2;
		int32_t node = add_node();
		Vector3i under_to = p_to;
		under_to[axis] = mid;
		Vector3i over_from = p_from;
		over_from[axis] = mid;
		int32_t under = build(p_from, under_to);
		int32_t over = build(over_from, p_to);
		Vector3 normal;
		normal[axis] = 1.0;
		set_node(node, normal, origin[axis] + mid * step[axis], over, under);
		return node;
	}
};

static void _build_probe_grid(const Vector3i &p_counts, const Vector3 &p_origin, const Vector3 &p_step, PackedInt32Array &r_tetrahedra, PackedInt32Array &r_bsp) {
	RealtimeGIProbeGrid grid;
	grid.counts = p_counts;
	grid.origin = p_origin;
	grid.step = p_step;
	grid.build(Vector3i(), p_counts - Vector3i(1, 1, 1));

	r_tetrahedra.resize(grid.tetrahedra.size());
	memcpy(r_tetrahedra.ptrw(), grid.tetrahedra.ptr(), grid.tetrahedra.size() * sizeof(int32_t));
	r_bsp.resize(grid.nodes.size());
	memcpy(r_bsp.ptrw(), grid.nodes.ptr(), grid.nodes.size() * sizeof(int32_t));
}

RealtimeGI::BakeError RealtimeGI::bake(Node *p_from_node, const String &p_save_path, BakeStepFunc p_step, void *p_userdata) {
	if (!p_from_node) {
		p_from_node = get_parent() ? get_parent() : this;
	}

	auto step = [&](float p_progress, const String &p_text) -> bool {
		return p_step && p_step(p_progress, p_text, p_userdata, true);
	};

	String save_path = p_save_path;
	if (save_path.is_empty()) {
		if (data.is_valid() && data->get_path().is_resource_file()) {
			save_path = data->get_path();
		} else {
			String scene_path = get_scene_file_path();
			if (scene_path.is_empty() && get_owner()) {
				scene_path = get_owner()->get_scene_file_path();
			}
			if (scene_path.is_empty()) {
				return BAKE_ERROR_NO_SAVE_PATH;
			}
			save_path = scene_path.get_basename() + "." + String(get_name()).to_snake_case() + ".rgi";
		}
	}

	Ref<StaticRaycaster> raycaster = StaticRaycaster::create();
	if (raycaster.is_null()) {
		return BAKE_ERROR_NO_RAYCASTER;
	}

	// Where the bake's time goes, for the verbose summary.
	String timings;
	uint64_t phase_start = OS::get_singleton()->get_ticks_msec();
	auto mark = [&](const char *p_phase) {
		uint64_t now = OS::get_singleton()->get_ticks_msec();
		timings += vformat(" %s %.1fs", p_phase, (now - phase_start) / 1000.0);
		phase_start = now;
	};

	step(0.0, RTR("Finding meshes"));
	Vector<BakeMesh> meshes;
	_find_meshes(p_from_node, meshes);

	// Read the meshes. Ones without UV2 get a copy unwrapped by xatlas, the
	// way Unity generates lightmap UVs, so a scene imported without them still
	// gets bounced light. Each mesh is unwrapped once however often it's used.
	HashMap<Mesh *, Ref<ArrayMesh>> unwrapped;
	for (int m = 0; m < meshes.size(); m++) {
		BakeMesh &bm = meshes.write[m];
		if ((m % 16) == 0 && step(0.01 + 0.04 * float(m) / meshes.size(), vformat(RTR("Reading meshes (%d/%d)"), m + 1, meshes.size()))) {
			return BAKE_ERROR_USER_ABORTED;
		}
		_extract_bake_mesh(bm, bm.mesh, patch_size);
		if (bm.receiver || !bm.can_unwrap || !generate_uv2) {
			continue;
		}
		Ref<ArrayMesh> copy;
		if (Ref<ArrayMesh> *found = unwrapped.getptr(bm.mesh.ptr())) {
			copy = *found;
		} else {
			copy = _unwrap_copy(bm.mesh, bm.xform.basis.get_scale(), patch_size * 0.5);
			unwrapped.insert(bm.mesh.ptr(), copy);
		}
		if (copy.is_valid()) {
			bm.generated = copy;
			_extract_bake_mesh(bm, copy, patch_size);
		}
	}

	mark("meshes");
	int receiver_count = 0;
	for (const BakeMesh &bm : meshes) {
		receiver_count += bm.receiver ? 1 : 0;
	}
	if (receiver_count == 0) {
		return BAKE_ERROR_NO_MESHES;
	}

	// Charts: a square per mesh, sized so a texel covers about one patch.
	// A one texel border around each keeps filtering from reading its
	// neighbors.
	//
	// Resolution goes where it shows. A chart is at most MAX_CHART_SIDE
	// texels across, so a terrain or a long road gets coarser patches while
	// rooms and props keep patch_size; bounced light over a large open area
	// changes slowly anyway. If the scene still has more patches than
	// max_patches, every chart coarsens until it fits, which bounds both the
	// bake and the cost per frame.
	const int MAX_CHART_SIDE = 128;
	const int MAX_ATLAS = 4096;
	float effective_patch = patch_size;
	auto chart_side = [&](const BakeMesh &p_bm, float p_patch) {
		return CLAMP(int(Math::ceil(Math::sqrt(p_bm.area / p_bm.uv_area) / p_patch)), 2, MAX_CHART_SIDE);
	};
	for (int attempt = 0; attempt < 16; attempt++) {
		double patches_estimate = 0.0;
		for (const BakeMesh &bm : meshes) {
			if (bm.receiver) {
				int side = chart_side(bm, effective_patch);
				patches_estimate += double(side) * side * MIN(bm.uv_area, 1.0f);
			}
		}
		if (patches_estimate <= max_patches) {
			break;
		}
		effective_patch *= Math::sqrt(float(patches_estimate / max_patches)) * 1.02;
	}
	Vector<Point2i> chart_positions;
	Size2i atlas;
	for (int attempt = 0; attempt < 8; attempt++) {
		Vector<Size2i> charts;
		for (BakeMesh &bm : meshes) {
			if (!bm.receiver) {
				continue;
			}
			bm.side = chart_side(bm, effective_patch);
			charts.push_back(Size2i(bm.side + 2, bm.side + 2));
		}
		Geometry2D::make_atlas(charts, chart_positions, atlas);
		if (atlas.x <= MAX_ATLAS && atlas.y <= MAX_ATLAS) {
			break;
		}
		effective_patch *= float(MAX(atlas.x, atlas.y)) / float(MAX_ATLAS) * 1.05;
	}
	if (effective_patch > patch_size * 1.01) {
		print_line(vformat("%s: %d patches at %.2f m would pass Max Patches; baked with %.2f m patches.", get_name(), max_patches, patch_size, effective_patch));
	}

	{
		int chart = 0;
		for (BakeMesh &bm : meshes) {
			if (bm.receiver) {
				bm.origin = chart_positions[chart++] + Point2i(1, 1);
			}
		}
	}

	// Albedo and emission, rendered in UV2 the way LightmapGI renders them.
	LocalVector<Color> texel_albedo;
	LocalVector<Color> texel_emission;
	LocalVector<int32_t> texel_mesh;
	LocalVector<Vector3> texel_position;
	LocalVector<Vector3> texel_normal;
	LocalVector<float> texel_radius;
	const uint32_t texel_count = atlas.x * atlas.y;
	texel_albedo.resize(texel_count);
	texel_emission.resize(texel_count);
	texel_mesh.resize(texel_count);
	texel_position.resize(texel_count);
	texel_normal.resize(texel_count);
	texel_radius.resize(texel_count);
	for (uint32_t i = 0; i < texel_count; i++) {
		texel_mesh[i] = -1;
	}

	for (int m = 0; m < meshes.size(); m++) {
		const BakeMesh &bm = meshes[m];
		if (!bm.receiver) {
			continue;
		}
		if (step(0.05 + 0.1 * float(m) / meshes.size(), vformat(RTR("Rendering surfaces (%d/%d)"), m + 1, meshes.size()))) {
			return BAKE_ERROR_USER_ABORTED;
		}

		Ref<Image> albedo_image;
		Ref<Image> orm_image;
		Ref<Image> emission_image;
		const Ref<Mesh> lit_mesh = bm.generated.is_valid() ? Ref<Mesh>(bm.generated) : bm.mesh;
		TypedArray<Image> images = RenderingServer::get_singleton()->bake_render_uv2(lit_mesh->get_rid(), bm.overrides, Size2i(bm.side, bm.side));
		if (images.size() > RSE::BAKE_CHANNEL_EMISSION) {
			albedo_image = images[RSE::BAKE_CHANNEL_ALBEDO_ALPHA];
			orm_image = images[RSE::BAKE_CHANNEL_ORM];
			emission_image = images[RSE::BAKE_CHANNEL_EMISSION];
			if (albedo_image.is_valid() && albedo_image->is_compressed()) {
				albedo_image->decompress();
			}
		}

		// Rasterize the triangles in UV2: every texel whose center a triangle
		// covers becomes a patch on that triangle.
		const int side = bm.side;
		const uint32_t triangle_count = bm.uv2.size() / 3;
		for (uint32_t t = 0; t < triangle_count; t++) {
			Vector2 a = bm.uv2[t * 3] * side, b = bm.uv2[t * 3 + 1] * side, c = bm.uv2[t * 3 + 2] * side;
			float uv_area = (b - a).cross(c - a) * 0.5;
			if (Math::abs(uv_area) < 1e-9) {
				continue;
			}
			const Vector3 &p0 = bm.positions[t * 3], &p1 = bm.positions[t * 3 + 1], &p2 = bm.positions[t * 3 + 2];
			float world_area = (p1 - p0).cross(p2 - p0).length() * 0.5;
			float radius = CLAMP(Math::sqrt(world_area / Math::abs(uv_area)) * 0.5f, 0.01f, effective_patch * 4.0f);

			int min_x = CLAMP(int(Math::floor(MIN(a.x, MIN(b.x, c.x)))), 0, side - 1);
			int max_x = CLAMP(int(Math::ceil(MAX(a.x, MAX(b.x, c.x)))), 0, side - 1);
			int min_y = CLAMP(int(Math::floor(MIN(a.y, MIN(b.y, c.y)))), 0, side - 1);
			int max_y = CLAMP(int(Math::ceil(MAX(a.y, MAX(b.y, c.y)))), 0, side - 1);

			bool covered = false;
			auto place = [&](int x, int y, float wa, float wb, float wc) {
				uint32_t texel = (bm.origin.y + y) * atlas.x + bm.origin.x + x;
				covered = true;
				if (texel_mesh[texel] >= 0) {
					return; // Overlapping UV2; the first triangle keeps it.
				}
				texel_mesh[texel] = m;
				texel_position[texel] = p0 * wa + p1 * wb + p2 * wc;
				Vector3 n = bm.normals[t * 3] * wa + bm.normals[t * 3 + 1] * wb + bm.normals[t * 3 + 2] * wc;
				texel_normal[texel] = n.length_squared() > 0.0 ? n.normalized() : (p1 - p0).cross(p2 - p0).normalized();
				texel_radius[texel] = radius;

				Color albedo = bm.surface_albedo[t];
				if (albedo_image.is_valid() && x < albedo_image->get_width() && y < albedo_image->get_height()) {
					albedo = albedo_image->get_pixel(x, y);
					if (orm_image.is_valid() && x < orm_image->get_width() && y < orm_image->get_height()) {
						albedo = albedo * (1.0 - orm_image->get_pixel(x, y).b);
					}
				}
				texel_albedo[texel] = albedo;
				if (emission_image.is_valid() && x < emission_image->get_width() && y < emission_image->get_height()) {
					texel_emission[texel] = emission_image->get_pixel(x, y);
				}
			};

			for (int y = min_y; y <= max_y; y++) {
				for (int x = min_x; x <= max_x; x++) {
					Vector2 p(x + 0.5, y + 0.5);
					float wa = (b - p).cross(c - p) * 0.5 / uv_area;
					float wb = (c - p).cross(a - p) * 0.5 / uv_area;
					float wc = 1.0 - wa - wb;
					const float eps = -1e-4;
					if (wa >= eps && wb >= eps && wc >= eps) {
						place(x, y, wa, wb, wc);
					}
				}
			}
			if (!covered) {
				// Too small to cover a texel center: put it where its centroid
				// falls, so thin things still bounce light.
				Vector2 centroid = (a + b + c) / 3.0;
				place(CLAMP(int(centroid.x), 0, side - 1), CLAMP(int(centroid.y), 0, side - 1), 1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
			}
		}
	}

	mark("surfaces");

	// Patches, cluster by cluster: a cluster is a 2x2 block of a chart's
	// texels, which is what the transport records hits against.
	Baker baker;
	baker.meshes = &meshes;
	baker.raycaster = raycaster;
	baker.atlas_size = atlas;
	baker.texel_patches.resize(texel_count);
	for (uint32_t i = 0; i < texel_count; i++) {
		baker.texel_patches[i] = -1;
	}

	PackedFloat32Array patches;
	PackedInt32Array cluster_offsets;
	LocalVector<uint32_t> patch_texel;
	for (int m = 0; m < meshes.size(); m++) {
		const BakeMesh &bm = meshes[m];
		if (!bm.receiver) {
			continue;
		}
		for (int by = 0; by < bm.side; by += 2) {
			for (int bx = 0; bx < bm.side; bx += 2) {
				bool started = false;
				for (int k = 0; k < 4; k++) {
					int x = bx + (k & 1), y = by + (k >> 1);
					if (x >= bm.side || y >= bm.side) {
						continue;
					}
					uint32_t texel = (bm.origin.y + y) * atlas.x + bm.origin.x + x;
					if (texel_mesh[texel] != m) {
						continue;
					}
					if (!started) {
						cluster_offsets.push_back(patch_texel.size());
						started = true;
					}
					baker.texel_patches[texel] = patch_texel.size();
					baker.patch_cluster.push_back(cluster_offsets.size() - 1);
					patch_texel.push_back(texel);
				}
			}
		}
	}
	cluster_offsets.push_back(patch_texel.size());
	const uint32_t patch_count = patch_texel.size();
	if (patch_count == 0) {
		return BAKE_ERROR_NO_MESHES;
	}

	patches.resize(patch_count * 16);
	{
		float *w = patches.ptrw();
		baker.patch_position.resize(patch_count);
		baker.patch_normal.resize(patch_count);
		baker.patch_radius.resize(patch_count);
		for (uint32_t i = 0; i < patch_count; i++) {
			uint32_t texel = patch_texel[i];
			const Vector3 &p = texel_position[texel];
			const Vector3 &n = texel_normal[texel];
			const Color &a = texel_albedo[texel];
			const Color &e = texel_emission[texel];
			float values[16] = { float(p.x), float(p.y), float(p.z), texel_radius[texel], float(n.x), float(n.y), float(n.z), 0.0, a.r, a.g, a.b, 1.0, e.r, e.g, e.b, 0.0 };
			memcpy(w + i * 16, values, sizeof(values));
			baker.patch_position[i] = p;
			baker.patch_normal[i] = n;
			baker.patch_radius[i] = texel_radius[texel];
		}
	}

	// Texels a chart doesn't cover show the nearest patch of the same chart,
	// so bilinear filtering at a chart's edge never reads black.
	PackedInt32Array texel_patches;
	texel_patches.resize(texel_count);
	{
		int32_t *w = texel_patches.ptrw();
		for (uint32_t i = 0; i < texel_count; i++) {
			w[i] = baker.texel_patches[i];
		}
		for (int m = 0; m < meshes.size(); m++) {
			const BakeMesh &bm = meshes[m];
			if (!bm.receiver) {
				continue;
			}
			for (int y = -1; y <= bm.side; y++) {
				for (int x = -1; x <= bm.side; x++) {
					uint32_t texel = (bm.origin.y + y) * atlas.x + bm.origin.x + x;
					if (w[texel] >= 0) {
						continue;
					}
					int32_t best = -1;
					int best_dist = INT32_MAX;
					for (int dy = -3; dy <= 3; dy++) {
						for (int dx = -3; dx <= 3; dx++) {
							int sx = x + dx, sy = y + dy;
							if (sx < 0 || sy < 0 || sx >= bm.side || sy >= bm.side) {
								continue;
							}
							int32_t patch = baker.texel_patches[(bm.origin.y + sy) * atlas.x + bm.origin.x + sx];
							int dist = dx * dx + dy * dy;
							if (patch >= 0 && dist < best_dist) {
								best = patch;
								best_dist = dist;
							}
						}
					}
					w[texel] = best;
				}
			}
		}
		// Rays land on chart texels a triangle missed too; let them find the
		// dilated patch.
		for (uint32_t i = 0; i < texel_count; i++) {
			baker.texel_patches[i] = w[i];
		}
	}

	// The scene for the bake's rays.
	mark("patches");
	if (step(0.16, RTR("Building ray tracing scene"))) {
		return BAKE_ERROR_USER_ABORTED;
	}
	for (int m = 0; m < meshes.size(); m++) {
		PackedVector3Array vertices;
		vertices.resize(meshes[m].positions.size());
		memcpy(vertices.ptrw(), meshes[m].positions.ptr(), sizeof(Vector3) * meshes[m].positions.size());
		raycaster->add_mesh(vertices, PackedInt32Array(), m);
	}
	raycaster->commit();

	static const uint32_t quality_rays[] = { 64, 128, 256, 512 };
	baker.rays = quality_rays[bake_quality];
	baker.patch_entries.resize(patch_count);
	baker.patch_sky.resize(patch_count * 9);

	auto run = [&](void (Baker::*p_method)(uint32_t, void *), uint32_t p_count, float p_from, float p_to, const String &p_text) -> bool {
		WorkerThreadPool::GroupID group = WorkerThreadPool::get_singleton()->add_template_group_task(&baker, p_method, (void *)nullptr, p_count, -1, true, SNAME("RealtimeGIBake"));
		while (!WorkerThreadPool::get_singleton()->is_group_task_completed(group)) {
			OS::get_singleton()->delay_usec(50000);
			float done = float(WorkerThreadPool::get_singleton()->get_group_processed_element_count(group)) / float(MAX(p_count, 1u));
			if (step(p_from + (p_to - p_from) * done, p_text)) {
				baker.cancel.set();
			}
		}
		WorkerThreadPool::get_singleton()->wait_for_group_task_completion(group);
		return !baker.cancel.is_set();
	};

	mark("ray scene");
	if (!run(&Baker::trace_patch, patch_count, 0.2, 0.85, vformat(RTR("Tracing light paths between %d patches"), patch_count))) {
		return BAKE_ERROR_USER_ABORTED;
	}

	mark("transport");
	PackedInt32Array transport_offsets;
	PackedInt32Array transport_entries;
	transport_offsets.resize(patch_count + 1);
	{
		uint32_t total = 0;
		for (uint32_t i = 0; i < patch_count; i++) {
			transport_offsets.write[i] = total;
			total += baker.patch_entries[i].size();
		}
		transport_offsets.write[patch_count] = total;
		transport_entries.resize(total);
		int32_t *w = transport_entries.ptrw();
		for (uint32_t i = 0; i < patch_count; i++) {
			memcpy(w + transport_offsets[i], baker.patch_entries[i].ptr(), baker.patch_entries[i].size() * sizeof(uint32_t));
		}
	}
	PackedFloat32Array sky_visibility;
	sky_visibility.resize(patch_count * 9);
	memcpy(sky_visibility.ptrw(), baker.patch_sky.ptr(), patch_count * 9 * sizeof(float));

	// Bounds of what the GI lights.
	AABB bounds;
	for (uint32_t i = 0; i < patch_count; i++) {
		if (i == 0) {
			bounds = AABB(baker.patch_position[i], Vector3());
		} else {
			bounds.expand_to(baker.patch_position[i]);
		}
	}

	// Probes on a grid through the bounds, for dynamic objects.
	PackedVector3Array probe_points;
	PackedInt32Array probe_hits;
	PackedInt32Array probe_tetrahedra;
	PackedInt32Array probe_bsp;
	if (probe_spacing > 0.0) {
		if (step(0.85, RTR("Placing probes"))) {
			return BAKE_ERROR_USER_ABORTED;
		}
		float spacing = probe_spacing;
		AABB probe_bounds = bounds.grow(spacing * 0.5);
		const int MAX_PROBES = 8192;
		Vector3i counts;
		for (int attempt = 0; attempt < 8; attempt++) {
			counts = Vector3i((probe_bounds.size / spacing).ceil()) + Vector3i(1, 1, 1);
			if (int64_t(counts.x) * counts.y * counts.z <= MAX_PROBES) {
				break;
			}
			spacing *= Math::pow(float(int64_t(counts.x) * counts.y * counts.z) / float(MAX_PROBES), 1.0f / 3.0f) * 1.02f;
		}
		counts = counts.max(Vector3i(2, 2, 2));
		const Vector3 step_size = probe_bounds.size / Vector3(counts - Vector3i(1, 1, 1));
		for (int z = 0; z < counts.z; z++) {
			for (int y = 0; y < counts.y; y++) {
				for (int x = 0; x < counts.x; x++) {
					baker.probe_candidates.push_back(probe_bounds.position + Vector3(x, y, z) * step_size);
				}
			}
		}
		baker.probe_rays = 128;
		baker.probe_hits.resize(baker.probe_candidates.size() * baker.probe_rays);
		baker.probe_valid.resize(baker.probe_candidates.size());
		if (!run(&Baker::trace_probe, baker.probe_candidates.size(), 0.85, 0.95, RTR("Tracing probes"))) {
			return BAKE_ERROR_USER_ABORTED;
		}

		// A probe inside a wall sees only backfaces. Rather than dropping it,
		// which would leave a hole in the grid, it takes the view of the
		// nearest probe that's in the open.
		const uint32_t probe_count = baker.probe_candidates.size();
		LocalVector<int32_t> source;
		source.resize(probe_count);
		LocalVector<uint32_t> queue;
		for (uint32_t i = 0; i < probe_count; i++) {
			source[i] = baker.probe_valid[i] ? int32_t(i) : -1;
			if (baker.probe_valid[i]) {
				queue.push_back(i);
			}
		}
		if (!queue.is_empty()) {
			for (uint32_t q = 0; q < queue.size(); q++) {
				const uint32_t i = queue[q];
				const int x = i % counts.x, y = (i / counts.x) % counts.y, z = i / (counts.x * counts.y);
				const Vector3i neighbors[6] = { Vector3i(x - 1, y, z), Vector3i(x + 1, y, z), Vector3i(x, y - 1, z), Vector3i(x, y + 1, z), Vector3i(x, y, z - 1), Vector3i(x, y, z + 1) };
				for (const Vector3i &n : neighbors) {
					if (n.x < 0 || n.y < 0 || n.z < 0 || n.x >= counts.x || n.y >= counts.y || n.z >= counts.z) {
						continue;
					}
					const uint32_t j = (n.z * counts.y + n.y) * counts.x + n.x;
					if (source[j] < 0) {
						source[j] = source[i];
						queue.push_back(j);
					}
				}
			}

			probe_points.resize(probe_count);
			probe_hits.resize(probe_count * baker.probe_rays);
			int32_t *hits = probe_hits.ptrw();
			for (uint32_t i = 0; i < probe_count; i++) {
				probe_points.write[i] = baker.probe_candidates[i];
				memcpy(hits + i * baker.probe_rays, baker.probe_hits.ptr() + source[i] * baker.probe_rays, baker.probe_rays * sizeof(int32_t));
			}
			_build_probe_grid(counts, probe_bounds.position, step_size, probe_tetrahedra, probe_bsp);
		}
	}

	// The volume realtime GI covers: the patches, and the space between them
	// where dynamic objects pick up its probes. The culler pairs dynamic
	// objects with it by these bounds, so a flat floor alone isn't enough.
	for (const Vector3 &p : probe_points) {
		bounds.expand_to(p);
	}
	bounds = bounds.grow(effective_patch);

	mark("probes");
	if (step(0.96, RTR("Building shadow geometry"))) {
		return BAKE_ERROR_USER_ABORTED;
	}
	LocalVector<Vector3> trace_positions;
	for (const BakeMesh &bm : meshes) {
		for (const Vector3 &v : bm.trace_positions) {
			trace_positions.push_back(v);
		}
	}
	PackedInt32Array bvh;
	PackedFloat32Array triangles;
	_build_bvh(trace_positions, bvh, triangles);
	mark("bvh");

	// Let go of the old bake's meshes first, while its user list still names them.
	Ref<RealtimeGIData> new_data;
	if (data.is_valid() && data->get_path() == save_path) {
		new_data = data; // Rebaking keeps the resource the scene points at.
	} else {
		new_data.instantiate();
	}
	set_data(Ref<RealtimeGIData>());
	new_data->clear_users();
	HashMap<ArrayMesh *, int> mesh_indices;
	for (const BakeMesh &bm : meshes) {
		if (!bm.receiver) {
			continue;
		}
		int mesh_index = -1;
		if (bm.generated.is_valid()) {
			if (int *found = mesh_indices.getptr(bm.generated.ptr())) {
				mesh_index = *found;
			} else {
				mesh_index = new_data->add_mesh(bm.generated);
				mesh_indices.insert(bm.generated.ptr(), mesh_index);
			}
		}
		new_data->add_user(bm.path, Rect2(Vector2(bm.origin) / Vector2(atlas), Vector2(bm.side, bm.side) / Vector2(atlas)), bm.sub_instance, mesh_index);
	}
	new_data->set_surfaces(atlas, effective_patch, patches, cluster_offsets, texel_patches);
	new_data->set_transport(baker.rays, transport_offsets, transport_entries, sky_visibility);
	new_data->set_geometry(bounds, bvh, triangles);
	new_data->set_probes(probe_points, probe_points.is_empty() ? 0 : baker.probe_rays, probe_hits, probe_tetrahedra, probe_bsp);

	step(0.98, RTR("Saving"));
	new_data->set_path(save_path, true);
	if (ResourceSaver::save(new_data, save_path) != OK) {
		set_data(new_data); // Still usable until the scene is closed.
		return BAKE_ERROR_CANT_SAVE;
	}

	mark("save");
	set_data(new_data);
	print_verbose("RealtimeGI bake times:" + timings);
	print_verbose(vformat("RealtimeGI: %d patches in %d clusters, %d transport entries, %d shadow triangles, %d probes, %dx%d atlas.", patch_count, cluster_offsets.size() - 1, transport_entries.size(), triangles.size() / 12, probe_points.size(), atlas.x, atlas.y));
	return BAKE_ERROR_OK;
}

int RealtimeGI::_bake_bind(Node *p_from_node, const String &p_save_path) {
	return bake(p_from_node, p_save_path);
}

void RealtimeGI::_assign_lightmaps() {
	ERR_FAIL_COND(data.is_null());
	Vector<String> missing;
	for (int i = 0; i < data->get_user_count(); i++) {
		Node *node = get_node_or_null(data->get_user_path(i));
		if (!node) {
			missing.push_back(String(data->get_user_path(i)));
			continue;
		}
		int sub_instance = data->get_user_sub_instance(i);
		RID user_instance;
		if (sub_instance >= 0) {
			user_instance = node->call("get_bake_mesh_instance", sub_instance);
		} else {
			VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(node);
			ERR_CONTINUE(!vi);
			user_instance = vi->get_instance();
		}
		if (user_instance.is_valid()) {
			// Draw with the copy that has UV2 to sample the lightmap through.
			// Swapping the base keeps the instance's materials and settings.
			Ref<Mesh> mesh = data->get_user_mesh(i);
			if (mesh.is_valid()) {
				RS::get_singleton()->instance_set_base(user_instance, mesh->get_rid());
			}
			RS::get_singleton()->instance_geometry_set_lightmap(user_instance, get_instance(), data->get_user_lightmap_uv_scale(i), 0);
		}
	}
	if (!missing.is_empty()) {
		WARN_PRINT(vformat("%s couldn't find %d baked node(s) and needs a rebake (first: %s).", get_name(), missing.size(), missing[0]));
	}
}

void RealtimeGI::_clear_lightmaps() {
	ERR_FAIL_COND(data.is_null());
	for (int i = 0; i < data->get_user_count(); i++) {
		Node *node = get_node_or_null(data->get_user_path(i));
		if (!node) {
			continue;
		}
		int sub_instance = data->get_user_sub_instance(i);
		RID user_instance;
		if (sub_instance >= 0) {
			user_instance = node->call("get_bake_mesh_instance", sub_instance);
		} else {
			VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(node);
			ERR_CONTINUE(!vi);
			user_instance = vi->get_instance();
		}
		if (user_instance.is_valid()) {
			RS::get_singleton()->instance_geometry_set_lightmap(user_instance, RID(), Rect2(), 0);
			MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(node);
			if (data->get_user_mesh(i).is_valid() && mi && mi->get_mesh().is_valid()) {
				RS::get_singleton()->instance_set_base(user_instance, mi->get_mesh()->get_rid());
			}
		}
	}
}

void RealtimeGI::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_POST_ENTER_TREE: {
			if (data.is_valid()) {
				if (last_owner && last_owner != get_owner()) {
					data->clear_users();
				}
				_assign_lightmaps();
			}
		} break;

		case NOTIFICATION_EXIT_TREE: {
			last_owner = get_owner();
			if (data.is_valid()) {
				_clear_lightmaps();
			}
		} break;
	}
}

void RealtimeGI::set_data(const Ref<RealtimeGIData> &p_data) {
	if (data.is_valid() && is_inside_tree()) {
		_clear_lightmaps();
	}
	data = p_data;
	if (data.is_valid()) {
		data->apply(realtime_gi);
		if (is_inside_tree()) {
			_assign_lightmaps();
		}
	} else {
		RS::get_singleton()->realtime_gi_set_surfaces(realtime_gi, Vector2i(), PackedFloat32Array(), PackedInt32Array(), PackedInt32Array());
		RS::get_singleton()->realtime_gi_set_geometry(realtime_gi, AABB(), PackedInt32Array(), PackedFloat32Array());
	}
	update_gizmos();
	update_configuration_warnings();
}

Ref<RealtimeGIData> RealtimeGI::get_data() const {
	return data;
}

void RealtimeGI::_update_energy() {
	RS::get_singleton()->realtime_gi_set_energy(realtime_gi, energy, environment_energy, albedo_boost, emission_energy);
}

void RealtimeGI::_update_quality() {
	RS::get_singleton()->realtime_gi_set_quality(realtime_gi, bounces_per_frame, shadow_samples, probe_update_interval);
}

void RealtimeGI::set_patch_size(float p_size) {
	patch_size = CLAMP(p_size, 0.05, 16.0);
}

float RealtimeGI::get_patch_size() const {
	return patch_size;
}

void RealtimeGI::set_bake_quality(BakeQuality p_quality) {
	// An int, since the enum can only hold 0 to 3 and clang rejects comparing it to 4.
	ERR_FAIL_INDEX(int(p_quality), 4);
	bake_quality = p_quality;
}

RealtimeGI::BakeQuality RealtimeGI::get_bake_quality() const {
	return bake_quality;
}

void RealtimeGI::set_probe_spacing(float p_spacing) {
	probe_spacing = MAX(p_spacing, 0.0);
}

float RealtimeGI::get_probe_spacing() const {
	return probe_spacing;
}

void RealtimeGI::set_max_patches(int p_patches) {
	max_patches = CLAMP(p_patches, 1024, 4194304);
}

int RealtimeGI::get_max_patches() const {
	return max_patches;
}

void RealtimeGI::set_generate_uv2(bool p_enable) {
	generate_uv2 = p_enable;
}

bool RealtimeGI::is_generating_uv2() const {
	return generate_uv2;
}

void RealtimeGI::set_energy(float p_energy) {
	energy = MAX(p_energy, 0.0);
	_update_energy();
}

float RealtimeGI::get_energy() const {
	return energy;
}

void RealtimeGI::set_environment_energy(float p_energy) {
	environment_energy = MAX(p_energy, 0.0);
	_update_energy();
}

float RealtimeGI::get_environment_energy() const {
	return environment_energy;
}

void RealtimeGI::set_albedo_boost(float p_boost) {
	albedo_boost = CLAMP(p_boost, 0.0, 10.0);
	_update_energy();
}

float RealtimeGI::get_albedo_boost() const {
	return albedo_boost;
}

void RealtimeGI::set_emission_energy(float p_energy) {
	emission_energy = MAX(p_energy, 0.0);
	_update_energy();
}

float RealtimeGI::get_emission_energy() const {
	return emission_energy;
}

void RealtimeGI::set_interior(bool p_interior) {
	interior = p_interior;
	RS::get_singleton()->realtime_gi_set_interior(realtime_gi, interior);
}

bool RealtimeGI::is_interior() const {
	return interior;
}

void RealtimeGI::set_bounces_per_frame(int p_bounces) {
	bounces_per_frame = CLAMP(p_bounces, 1, 8);
	_update_quality();
}

int RealtimeGI::get_bounces_per_frame() const {
	return bounces_per_frame;
}

void RealtimeGI::set_shadow_samples(int p_samples) {
	shadow_samples = CLAMP(p_samples, 1, 16);
	_update_quality();
}

int RealtimeGI::get_shadow_samples() const {
	return shadow_samples;
}

void RealtimeGI::set_probe_update_interval(int p_frames) {
	probe_update_interval = CLAMP(p_frames, 1, 120);
	_update_quality();
}

int RealtimeGI::get_probe_update_interval() const {
	return probe_update_interval;
}

AABB RealtimeGI::get_aabb() const {
	if (data.is_valid()) {
		return data->get_bounds();
	}
	return AABB();
}

PackedStringArray RealtimeGI::get_configuration_warnings() const {
	PackedStringArray warnings = VisualInstance3D::get_configuration_warnings();

	if (OS::get_singleton()->get_current_rendering_method() == "gl_compatibility") {
		warnings.push_back(RTR("RealtimeGI isn't supported by the Compatibility renderer, which has no compute shaders. Use Forward+ or Mobile."));
		return warnings;
	}
	if (data.is_null()) {
		warnings.push_back(RTR("No realtime GI has been baked yet. Select this node and press Bake Realtime GI at the top of the 3D editor. Static meshes need UV2 (Import dock > Meshes > Light Baking: Static Lightmaps, or Mesh > Unwrap UV2)."));
	}
	return warnings;
}

void RealtimeGI::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_data", "data"), &RealtimeGI::set_data);
	ClassDB::bind_method(D_METHOD("get_data"), &RealtimeGI::get_data);
	ClassDB::bind_method(D_METHOD("set_patch_size", "size"), &RealtimeGI::set_patch_size);
	ClassDB::bind_method(D_METHOD("get_patch_size"), &RealtimeGI::get_patch_size);
	ClassDB::bind_method(D_METHOD("set_bake_quality", "quality"), &RealtimeGI::set_bake_quality);
	ClassDB::bind_method(D_METHOD("get_bake_quality"), &RealtimeGI::get_bake_quality);
	ClassDB::bind_method(D_METHOD("set_probe_spacing", "spacing"), &RealtimeGI::set_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_probe_spacing"), &RealtimeGI::get_probe_spacing);
	ClassDB::bind_method(D_METHOD("set_max_patches", "patches"), &RealtimeGI::set_max_patches);
	ClassDB::bind_method(D_METHOD("get_max_patches"), &RealtimeGI::get_max_patches);
	ClassDB::bind_method(D_METHOD("set_generate_uv2", "enable"), &RealtimeGI::set_generate_uv2);
	ClassDB::bind_method(D_METHOD("is_generating_uv2"), &RealtimeGI::is_generating_uv2);
	ClassDB::bind_method(D_METHOD("set_energy", "energy"), &RealtimeGI::set_energy);
	ClassDB::bind_method(D_METHOD("get_energy"), &RealtimeGI::get_energy);
	ClassDB::bind_method(D_METHOD("set_environment_energy", "energy"), &RealtimeGI::set_environment_energy);
	ClassDB::bind_method(D_METHOD("get_environment_energy"), &RealtimeGI::get_environment_energy);
	ClassDB::bind_method(D_METHOD("set_albedo_boost", "boost"), &RealtimeGI::set_albedo_boost);
	ClassDB::bind_method(D_METHOD("get_albedo_boost"), &RealtimeGI::get_albedo_boost);
	ClassDB::bind_method(D_METHOD("set_emission_energy", "energy"), &RealtimeGI::set_emission_energy);
	ClassDB::bind_method(D_METHOD("get_emission_energy"), &RealtimeGI::get_emission_energy);
	ClassDB::bind_method(D_METHOD("set_interior", "enable"), &RealtimeGI::set_interior);
	ClassDB::bind_method(D_METHOD("is_interior"), &RealtimeGI::is_interior);
	ClassDB::bind_method(D_METHOD("set_bounces_per_frame", "bounces"), &RealtimeGI::set_bounces_per_frame);
	ClassDB::bind_method(D_METHOD("get_bounces_per_frame"), &RealtimeGI::get_bounces_per_frame);
	ClassDB::bind_method(D_METHOD("set_shadow_samples", "samples"), &RealtimeGI::set_shadow_samples);
	ClassDB::bind_method(D_METHOD("get_shadow_samples"), &RealtimeGI::get_shadow_samples);
	ClassDB::bind_method(D_METHOD("set_probe_update_interval", "frames"), &RealtimeGI::set_probe_update_interval);
	ClassDB::bind_method(D_METHOD("get_probe_update_interval"), &RealtimeGI::get_probe_update_interval);
	ClassDB::bind_method(D_METHOD("bake", "from_node", "save_path"), &RealtimeGI::_bake_bind, DEFVAL(Variant()), DEFVAL(String()));

	ADD_GROUP("Bake", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "patch_size", PROPERTY_HINT_RANGE, "0.05,16,0.01,or_greater,suffix:m"), "set_patch_size", "get_patch_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bake_quality", PROPERTY_HINT_ENUM, "Low,Medium,High,Ultra"), "set_bake_quality", "get_bake_quality");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_patches", PROPERTY_HINT_RANGE, "1024,1048576,1024,or_greater"), "set_max_patches", "get_max_patches");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "generate_uv2"), "set_generate_uv2", "is_generating_uv2");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_spacing", PROPERTY_HINT_RANGE, "0,16,0.1,or_greater,suffix:m"), "set_probe_spacing", "get_probe_spacing");
	ADD_GROUP("Lighting", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_energy", "get_energy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "environment_energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_environment_energy", "get_environment_energy");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "albedo_boost", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_albedo_boost", "get_albedo_boost");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission_energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_emission_energy", "get_emission_energy");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "interior"), "set_interior", "is_interior");
	ADD_GROUP("Performance", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "bounces_per_frame", PROPERTY_HINT_RANGE, "1,8,1"), "set_bounces_per_frame", "get_bounces_per_frame");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shadow_samples", PROPERTY_HINT_RANGE, "1,16,1"), "set_shadow_samples", "get_shadow_samples");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "probe_update_interval", PROPERTY_HINT_RANGE, "1,120,1,suffix:frames"), "set_probe_update_interval", "get_probe_update_interval");
	ADD_GROUP("", "");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "data", PROPERTY_HINT_RESOURCE_TYPE, "RealtimeGIData"), "set_data", "get_data");

	BIND_ENUM_CONSTANT(BAKE_QUALITY_LOW);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_MEDIUM);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_HIGH);
	BIND_ENUM_CONSTANT(BAKE_QUALITY_ULTRA);

	BIND_ENUM_CONSTANT(BAKE_ERROR_OK);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_SCENE_ROOT);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_SAVE_PATH);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_MESHES);
	BIND_ENUM_CONSTANT(BAKE_ERROR_NO_RAYCASTER);
	BIND_ENUM_CONSTANT(BAKE_ERROR_CANT_SAVE);
	BIND_ENUM_CONSTANT(BAKE_ERROR_USER_ABORTED);
}

RealtimeGI::RealtimeGI() {
	realtime_gi = RS::get_singleton()->realtime_gi_create();
	set_base(realtime_gi);
	_update_energy();
	_update_quality();
	set_disable_scale(true);
}

RealtimeGI::~RealtimeGI() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(realtime_gi);
}
