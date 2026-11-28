/**************************************************************************/
/*  mesh_painter.cpp                                                      */
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

#include "mesh_painter.h"

#include "core/math/geometry_2d.h"
#include "core/object/class_db.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/surface_tool.h"
#include "servers/rendering/rendering_server.h"

// MeshPaintBrush.

void MeshPaintBrush::set_color(const Color &p_color) {
	color = p_color;
	emit_changed();
}

void MeshPaintBrush::set_strength(float p_strength) {
	strength = CLAMP(p_strength, 0.0f, 1.0f);
	emit_changed();
}

void MeshPaintBrush::set_hardness(float p_hardness) {
	hardness = CLAMP(p_hardness, 0.0f, 1.0f);
	emit_changed();
}

void MeshPaintBrush::set_blend_mode(BlendMode p_mode) {
	ERR_FAIL_INDEX((int)p_mode, (int)BLEND_MAX);
	blend_mode = p_mode;
	emit_changed();
}

void MeshPaintBrush::set_texture(const Ref<Texture2D> &p_texture) {
	texture = p_texture;
	texture_image.unref();
	if (texture.is_valid()) {
		Ref<Image> image = texture->get_image();
		if (image.is_valid() && !image->is_empty()) {
			image = image->duplicate();
			if (image->is_compressed()) {
				image->decompress();
			}
			image->clear_mipmaps();
			image->convert(Image::FORMAT_RGBA8);
			texture_image = image;
		}
	}
	emit_changed();
}

void MeshPaintBrush::set_texture_angle(float p_degrees) {
	texture_angle = p_degrees;
	emit_changed();
}

void MeshPaintBrush::set_paint_backfaces(bool p_enable) {
	paint_backfaces = p_enable;
	emit_changed();
}

void MeshPaintBrush::set_occlusion(bool p_enable) {
	occlusion = p_enable;
	emit_changed();
}

float MeshPaintBrush::falloff(float p_t) const {
	if (p_t >= 1.0f) {
		return 0.0f;
	}
	if (p_t <= hardness) {
		return 1.0f;
	}
	const float x = (p_t - hardness) / MAX(1.0f - hardness, 0.0001f);
	// Smooth at both ends, so dab edges never band.
	return 1.0f - x * x * (3.0f - 2.0f * x);
}

void MeshPaintBrush::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_color", "color"), &MeshPaintBrush::set_color);
	ClassDB::bind_method(D_METHOD("get_color"), &MeshPaintBrush::get_color);
	ClassDB::bind_method(D_METHOD("set_strength", "strength"), &MeshPaintBrush::set_strength);
	ClassDB::bind_method(D_METHOD("get_strength"), &MeshPaintBrush::get_strength);
	ClassDB::bind_method(D_METHOD("set_hardness", "hardness"), &MeshPaintBrush::set_hardness);
	ClassDB::bind_method(D_METHOD("get_hardness"), &MeshPaintBrush::get_hardness);
	ClassDB::bind_method(D_METHOD("set_blend_mode", "mode"), &MeshPaintBrush::set_blend_mode);
	ClassDB::bind_method(D_METHOD("get_blend_mode"), &MeshPaintBrush::get_blend_mode);
	ClassDB::bind_method(D_METHOD("set_texture", "texture"), &MeshPaintBrush::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture"), &MeshPaintBrush::get_texture);
	ClassDB::bind_method(D_METHOD("set_texture_angle", "degrees"), &MeshPaintBrush::set_texture_angle);
	ClassDB::bind_method(D_METHOD("get_texture_angle"), &MeshPaintBrush::get_texture_angle);
	ClassDB::bind_method(D_METHOD("set_paint_backfaces", "enable"), &MeshPaintBrush::set_paint_backfaces);
	ClassDB::bind_method(D_METHOD("is_painting_backfaces"), &MeshPaintBrush::is_painting_backfaces);
	ClassDB::bind_method(D_METHOD("set_occlusion", "enable"), &MeshPaintBrush::set_occlusion);
	ClassDB::bind_method(D_METHOD("has_occlusion"), &MeshPaintBrush::has_occlusion);

	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color"), "set_color", "get_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_strength", "get_strength");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "hardness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_hardness", "get_hardness");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "blend_mode", PROPERTY_HINT_ENUM, "Mix,Add,Multiply,Lighten,Darken,Erase,Blur"), "set_blend_mode", "get_blend_mode");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "texture_angle", PROPERTY_HINT_RANGE, "-180,180,0.1,degrees"), "set_texture_angle", "get_texture_angle");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paint_backfaces"), "set_paint_backfaces", "is_painting_backfaces");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "occlusion"), "set_occlusion", "has_occlusion");

	BIND_ENUM_CONSTANT(BLEND_MIX);
	BIND_ENUM_CONSTANT(BLEND_ADD);
	BIND_ENUM_CONSTANT(BLEND_MULTIPLY);
	BIND_ENUM_CONSTANT(BLEND_LIGHTEN);
	BIND_ENUM_CONSTANT(BLEND_DARKEN);
	BIND_ENUM_CONSTANT(BLEND_ERASE);
	BIND_ENUM_CONSTANT(BLEND_BLUR);
	BIND_ENUM_CONSTANT(BLEND_MAX);
}

// Helpers.

namespace {

struct VertKey {
	int surface = 0;
	int index = 0;
	int64_t u = 0;
	int64_t v = 0;

	bool operator==(const VertKey &p_other) const {
		return surface == p_other.surface && index == p_other.index && u == p_other.u && v == p_other.v;
	}
	uint32_t hash() const {
		uint32_t h = hash_murmur3_one_32(surface);
		h = hash_murmur3_one_32(index, h);
		h = hash_murmur3_one_64(u, h);
		h = hash_murmur3_one_64(v, h);
		return hash_fmix32(h);
	}
};

struct VertKeyHasher {
	static _FORCE_INLINE_ uint32_t hash(const VertKey &p_key) { return p_key.hash(); }
};

struct PairKey {
	int64_t a = 0;
	int64_t b = 0;

	bool operator==(const PairKey &p_other) const { return a == p_other.a && b == p_other.b; }
};

struct PairKeyHasher {
	static _FORCE_INLINE_ uint32_t hash(const PairKey &p_key) {
		return hash_fmix32(hash_murmur3_one_64(p_key.b, hash_murmur3_one_64(p_key.a)));
	}
};

const real_t UV_QUANTUM = 1048576.0; // 2^20 steps per UV unit.
const int CUSTOM_CHANNELS = 4; // ARRAY_CUSTOM0..3 (Mesh has no count constant).

int64_t quantize_uv(real_t p_value) {
	return (int64_t)Math::round(p_value * UV_QUANTUM);
}

int find_root(LocalVector<int> &r_parent, int p_index) {
	while (r_parent[p_index] != p_index) {
		r_parent[p_index] = r_parent[r_parent[p_index]];
		p_index = r_parent[p_index];
	}
	return p_index;
}

void unite(LocalVector<int> &r_parent, int p_a, int p_b) {
	p_a = find_root(r_parent, p_a);
	p_b = find_root(r_parent, p_b);
	if (p_a != p_b) {
		r_parent[MAX(p_a, p_b)] = MIN(p_a, p_b);
	}
}

real_t triangle_area_2d(const Vector2 &p_a, const Vector2 &p_b, const Vector2 &p_c) {
	return Math::abs((p_b - p_a).cross(p_c - p_a)) * 0.5;
}

Rect2i merge_rect(const Rect2i &p_a, const Rect2i &p_b) {
	if (p_a.size.x <= 0 || p_a.size.y <= 0) {
		return p_b;
	}
	if (p_b.size.x <= 0 || p_b.size.y <= 0) {
		return p_a;
	}
	return p_a.merge(p_b);
}

Color sample_bilinear(const Ref<Image> &p_image, float p_u, float p_v, bool p_wrap) {
	const int w = p_image->get_width();
	const int h = p_image->get_height();
	float x = p_u * w - 0.5f;
	float y = p_v * h - 0.5f;
	const int x0 = (int)Math::floor(x);
	const int y0 = (int)Math::floor(y);
	const float fx = x - x0;
	const float fy = y - y0;
	const uint8_t *data = p_image->ptr();
	auto fetch = [&](int px, int py) {
		if (p_wrap) {
			px = Math::posmod(px, w);
			py = Math::posmod(py, h);
		} else {
			px = CLAMP(px, 0, w - 1);
			py = CLAMP(py, 0, h - 1);
		}
		const uint8_t *p = data + (py * w + px) * 4;
		return Color(p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
	};
	const Color top = fetch(x0, y0).lerp(fetch(x0 + 1, y0), fx);
	const Color bottom = fetch(x0, y0 + 1).lerp(fetch(x0 + 1, y0 + 1), fx);
	return top.lerp(bottom, fy);
}

int custom_format_components(int p_format, bool &r_is_float) {
	r_is_float = false;
	switch (p_format) {
		case Mesh::ARRAY_CUSTOM_RGBA8_UNORM:
		case Mesh::ARRAY_CUSTOM_RGBA8_SNORM:
		case Mesh::ARRAY_CUSTOM_RG_HALF:
			return 4; // Bytes.
		case Mesh::ARRAY_CUSTOM_RGBA_HALF:
			return 8; // Bytes.
		case Mesh::ARRAY_CUSTOM_R_FLOAT:
			r_is_float = true;
			return 1;
		case Mesh::ARRAY_CUSTOM_RG_FLOAT:
			r_is_float = true;
			return 2;
		case Mesh::ARRAY_CUSTOM_RGB_FLOAT:
			r_is_float = true;
			return 3;
		case Mesh::ARRAY_CUSTOM_RGBA_FLOAT:
			r_is_float = true;
			return 4;
	}
	return 0;
}

} // namespace

// Loading.

void MeshPainter::_clear() {
	loaded_mesh.unref();
	committed_mesh.unref();
	sources.clear();
	verts.clear();
	faces.clear();
	slots.clear();
	selected.clear();
	positions.clear();
	face_normals.clear();
	face_bounds.clear();
	triangle_mesh.unref();
	uvs_present = false;
	uvs_changed = false;
	topology_dirty = true;
	islands_dirty = true;
	blend_shape_names.clear();
	blend_shape_mode = Mesh::BLEND_SHAPE_MODE_RELATIVE;
	shadow_mesh.unref();
	had_lods = false;
	custom_aabb = AABB();
	lightmap_size_hint = Size2i();
	stroke_images.clear();
	stroke_visibility.clear();
}

Error MeshPainter::load_mesh(const Ref<Mesh> &p_mesh, const Ref<Mesh> &p_pose_mesh, const TypedArray<Material> &p_surface_overrides) {
	_clear();
	ERR_FAIL_COND_V(p_mesh.is_null(), ERR_INVALID_PARAMETER);
	loaded_mesh = p_mesh;

	const Ref<ArrayMesh> array_mesh = p_mesh;
	if (array_mesh.is_valid()) {
		for (int i = 0; i < array_mesh->get_blend_shape_count(); i++) {
			blend_shape_names.push_back(array_mesh->get_blend_shape_name(i));
		}
		blend_shape_mode = array_mesh->get_blend_shape_mode();
		shadow_mesh = array_mesh->get_shadow_mesh();
		custom_aabb = array_mesh->get_custom_aabb();
		lightmap_size_hint = array_mesh->get_lightmap_size_hint();
	}

	bool all_uvs = true;
	for (int s = 0; s < p_mesh->get_surface_count(); s++) {
		if (p_mesh->get_rid().is_valid() && RenderingServer::get_singleton()->mesh_get_surface(p_mesh->get_rid(), s).lods.size() > 0) {
			had_lods = true;
		}
		SourceSurface source;
		source.arrays = p_mesh->surface_get_arrays(s);
		source.format = p_mesh->surface_get_format(s);
		source.blend_shapes = p_mesh->surface_get_blend_shape_arrays(s);
		if (array_mesh.is_valid()) {
			source.name = array_mesh->surface_get_name(s);
		}
		ERR_CONTINUE(source.arrays.size() != Mesh::ARRAY_MAX);
		source.vertices = source.arrays[Mesh::ARRAY_VERTEX];
		source.normals = source.arrays[Mesh::ARRAY_NORMAL];
		source.tangents = source.arrays[Mesh::ARRAY_TANGENT];
		source.colors = source.arrays[Mesh::ARRAY_COLOR];
		source.uv2s = source.arrays[Mesh::ARRAY_TEX_UV2];
		source.bones = source.arrays[Mesh::ARRAY_BONES];
		source.weights = source.arrays[Mesh::ARRAY_WEIGHTS];
		source.vertex_count = source.vertices.size();

		Ref<Material> material = p_mesh->surface_get_material(s);
		if (s < p_surface_overrides.size() && Ref<Material>(p_surface_overrides[s]).is_valid()) {
			material = p_surface_overrides[s];
		}

		const int surface_index = sources.size();
		sources.push_back(source);
		if (p_mesh->surface_get_primitive_type(s) != Mesh::PRIMITIVE_TRIANGLES || source.vertex_count == 0) {
			// Lines and points are carried through untouched (see commit_mesh()).
			sources.write[surface_index].vertex_count = -1;
			continue;
		}

		const int slot = add_slot(material, source.name.is_empty() ? vformat("Surface %d", s + 1) : source.name);

		const Vector<Vector2> uvs = source.arrays[Mesh::ARRAY_TEX_UV];
		const bool has_uv = uvs.size() == source.vertex_count;
		all_uvs = all_uvs && has_uv;

		const int base = verts.size();
		verts.resize(base + source.vertex_count);
		for (int i = 0; i < source.vertex_count; i++) {
			Vert &vert = verts[base + i];
			vert.surface = surface_index;
			vert.index = i;
			vert.uv = has_uv ? uvs[i] : Vector2();
		}

		Vector<int> indices = source.arrays[Mesh::ARRAY_INDEX];
		if (indices.is_empty()) {
			indices.resize(source.vertex_count);
			for (int i = 0; i < source.vertex_count; i++) {
				indices.write[i] = i;
			}
		}
		for (int i = 0; i + 2 < indices.size(); i += 3) {
			Face face;
			for (int c = 0; c < 3; c++) {
				ERR_FAIL_INDEX_V(indices[i + c], source.vertex_count, ERR_INVALID_DATA);
				face.v[c] = base + indices[i + c];
			}
			face.slot = slot;
			faces.push_back(face);
		}
	}
	uvs_present = all_uvs && !faces.is_empty();
	selected.resize(faces.size());
	memset(selected.ptr(), 0, selected.size());

	_update_geometry(p_pose_mesh);
	return faces.is_empty() ? ERR_INVALID_DATA : OK;
}

void MeshPainter::set_pose_mesh(const Ref<Mesh> &p_pose_mesh) {
	_update_geometry(p_pose_mesh);
}

void MeshPainter::_update_geometry(const Ref<Mesh> &p_pose_mesh) {
	// Posed vertex positions per source surface, when the pose mesh has the same layout.
	LocalVector<Vector<Vector3>> source_positions;
	source_positions.resize(sources.size());
	for (uint32_t s = 0; s < sources.size(); s++) {
		source_positions[s] = sources[s].vertices;
		if (p_pose_mesh.is_valid() && p_pose_mesh->get_surface_count() == (int)sources.size() && sources[s].vertex_count > 0) {
			const Array pose_arrays = p_pose_mesh->surface_get_arrays(s);
			if (pose_arrays.size() == Mesh::ARRAY_MAX) {
				const Vector<Vector3> posed = pose_arrays[Mesh::ARRAY_VERTEX];
				if (posed.size() == sources[s].vertex_count) {
					source_positions[s] = posed;
				}
			}
		}
	}

	positions.resize(verts.size());
	for (uint32_t i = 0; i < verts.size(); i++) {
		positions[i] = source_positions[verts[i].surface][verts[i].index];
	}
	_update_face_data();
}

void MeshPainter::_update_face_data() {
	face_normals.resize(faces.size());
	face_bounds.resize(faces.size());
	Vector<Vector3> triangles;
	triangles.resize(faces.size() * 3);
	Vector3 *tw = triangles.ptrw();
	AABB bounds;
	for (uint32_t f = 0; f < faces.size(); f++) {
		const Vector3 a = positions[faces[f].v[0]];
		const Vector3 b = positions[faces[f].v[1]];
		const Vector3 c = positions[faces[f].v[2]];
		tw[f * 3 + 0] = a;
		tw[f * 3 + 1] = b;
		tw[f * 3 + 2] = c;
		// Godot's front faces wind clockwise, so this normal points out of the front.
		Vector3 n = (c - a).cross(b - a);
		face_normals[f] = n.length_squared() > 0 ? n.normalized() : Vector3();
		AABB box(a, Vector3());
		box.expand_to(b);
		box.expand_to(c);
		face_bounds[f] = box;
		bounds = f == 0 ? box : bounds.merge(box);
	}
	weld_epsilon = MAX(bounds.get_longest_axis_size() * 0.00001, 0.000001);
	triangle_mesh.instantiate();
	if (!triangles.is_empty()) {
		triangle_mesh->create(triangles);
	}
	stroke_visibility.clear();
	topology_dirty = true;
	islands_dirty = true;
}

void MeshPainter::_update_topology() {
	if (!topology_dirty) {
		return;
	}
	topology_dirty = false;
	islands_dirty = true;

	// Weld by rest position, so the topology does not depend on the pose.
	HashMap<Vector3i, int> welds;
	vert_weld.resize(verts.size());
	const real_t inv = 1.0 / weld_epsilon;
	for (uint32_t i = 0; i < verts.size(); i++) {
		const Vector3 p = sources[verts[i].surface].vertices[verts[i].index] * inv;
		const Vector3i key(Math::round(p.x), Math::round(p.y), Math::round(p.z));
		HashMap<Vector3i, int>::Iterator E = welds.find(key);
		if (E) {
			vert_weld[i] = E->value;
		} else {
			const int id = welds.size();
			welds.insert(key, id);
			vert_weld[i] = id;
		}
	}
	weld_count = welds.size();

	weld_face_offsets.resize(weld_count + 1);
	memset(weld_face_offsets.ptr(), 0, sizeof(int) * weld_face_offsets.size());
	for (const Face &face : faces) {
		for (int c = 0; c < 3; c++) {
			weld_face_offsets[vert_weld[face.v[c]] + 1]++;
		}
	}
	for (int i = 0; i < weld_count; i++) {
		weld_face_offsets[i + 1] += weld_face_offsets[i];
	}
	weld_faces.resize(weld_face_offsets[weld_count]);
	LocalVector<int> fill;
	fill.resize(weld_count);
	memset(fill.ptr(), 0, sizeof(int) * fill.size());
	for (uint32_t f = 0; f < faces.size(); f++) {
		for (int c = 0; c < 3; c++) {
			const int w = vert_weld[faces[f].v[c]];
			weld_faces[weld_face_offsets[w] + fill[w]++] = f;
		}
	}
}

void MeshPainter::_update_islands() {
	_update_topology();
	if (!islands_dirty) {
		return;
	}
	islands_dirty = false;

	// A UV-weld id per vertex: same welded position and same UV.
	LocalVector<int> uv_weld;
	uv_weld.resize(verts.size());
	{
		HashMap<VertKey, int, VertKeyHasher> ids;
		for (uint32_t i = 0; i < verts.size(); i++) {
			VertKey key;
			key.surface = 0;
			key.index = vert_weld[i];
			key.u = quantize_uv(verts[i].uv.x);
			key.v = quantize_uv(verts[i].uv.y);
			HashMap<VertKey, int, VertKeyHasher>::Iterator E = ids.find(key);
			if (E) {
				uv_weld[i] = E->value;
			} else {
				const int id = ids.size();
				ids.insert(key, id);
				uv_weld[i] = id;
			}
		}
	}

	LocalVector<int> island_parent;
	LocalVector<int> piece_parent;
	island_parent.resize(faces.size());
	piece_parent.resize(faces.size());
	for (uint32_t f = 0; f < faces.size(); f++) {
		island_parent[f] = f;
		piece_parent[f] = f;
	}

	HashMap<PairKey, int, PairKeyHasher> uv_edges;
	HashMap<PairKey, int, PairKeyHasher> edges;
	for (uint32_t f = 0; f < faces.size(); f++) {
		for (int c = 0; c < 3; c++) {
			const int a = faces[f].v[c];
			const int b = faces[f].v[(c + 1) % 3];
			PairKey uv_key;
			uv_key.a = MIN(uv_weld[a], uv_weld[b]);
			uv_key.b = MAX(uv_weld[a], uv_weld[b]);
			HashMap<PairKey, int, PairKeyHasher>::Iterator E = uv_edges.find(uv_key);
			if (E) {
				unite(island_parent, f, E->value);
			} else {
				uv_edges.insert(uv_key, f);
			}
			PairKey key;
			key.a = MIN(vert_weld[a], vert_weld[b]);
			key.b = MAX(vert_weld[a], vert_weld[b]);
			E = edges.find(key);
			if (E) {
				unite(piece_parent, f, E->value);
			} else {
				edges.insert(key, f);
			}
		}
	}

	face_island.resize(faces.size());
	face_piece.resize(faces.size());
	HashMap<int, int> island_ids;
	HashMap<int, int> piece_ids;
	for (uint32_t f = 0; f < faces.size(); f++) {
		const int ir = find_root(island_parent, f);
		HashMap<int, int>::Iterator E = island_ids.find(ir);
		if (!E) {
			E = island_ids.insert(ir, island_ids.size());
		}
		face_island[f] = E->value;
		const int pr = find_root(piece_parent, f);
		E = piece_ids.find(pr);
		if (!E) {
			E = piece_ids.insert(pr, piece_ids.size());
		}
		face_piece[f] = E->value;
	}
	island_count = island_ids.size();
	piece_count = piece_ids.size();
}

// Output.

Ref<ArrayMesh> MeshPainter::commit_mesh(bool p_generate_lods) {
	ERR_FAIL_COND_V(faces.is_empty(), Ref<ArrayMesh>());

	// Faces in slot order, so the painter's face order is the output's (a reload of the
	// output keeps face indices and the selection).
	{
		LocalVector<Face> ordered;
		LocalVector<uint8_t> ordered_selected;
		ordered.reserve(faces.size());
		ordered_selected.reserve(faces.size());
		for (uint32_t s = 0; s < slots.size(); s++) {
			for (uint32_t f = 0; f < faces.size(); f++) {
				if (faces[f].slot == (int)s) {
					ordered.push_back(faces[f]);
					ordered_selected.push_back(selected[f]);
				}
			}
		}
		if (ordered.size() == faces.size()) {
			faces = ordered;
			selected = ordered_selected;
			_update_face_data();
		}
	}

	Ref<ArrayMesh> out;
	out.instantiate();
	for (int i = 0; i < blend_shape_names.size(); i++) {
		out->add_blend_shape(blend_shape_names[i]);
	}
	out->set_blend_shape_mode(blend_shape_mode);
	const int blend_count = blend_shape_names.size();

	// Blend shape targets as typed arrays, per source surface and shape.
	struct BlendTarget {
		Vector<Vector3> vertices;
		Vector<Vector3> normals;
		Vector<float> tangents;
	};
	LocalVector<LocalVector<BlendTarget>> blend_targets;
	blend_targets.resize(sources.size());
	for (uint32_t s = 0; s < sources.size(); s++) {
		blend_targets[s].resize(blend_count);
		for (int b = 0; b < blend_count && b < sources[s].blend_shapes.size(); b++) {
			const Array shape = sources[s].blend_shapes[b];
			if (shape.size() == Mesh::ARRAY_MAX) {
				blend_targets[s][b].vertices = shape[Mesh::ARRAY_VERTEX];
				blend_targets[s][b].normals = shape[Mesh::ARRAY_NORMAL];
				blend_targets[s][b].tangents = shape[Mesh::ARRAY_TANGENT];
			}
		}
	}

	for (uint32_t s = 0; s < slots.size(); s++) {
		// Vertices this slot uses, in first-use order.
		LocalVector<int> remap;
		remap.resize(verts.size());
		for (uint32_t i = 0; i < remap.size(); i++) {
			remap[i] = -1;
		}
		LocalVector<int> used;
		Vector<int> indices;
		HashSet<int> slot_sources;
		for (const Face &face : faces) {
			if (face.slot != (int)s) {
				continue;
			}
			for (int c = 0; c < 3; c++) {
				int &r = remap[face.v[c]];
				if (r < 0) {
					r = used.size();
					used.push_back(face.v[c]);
					slot_sources.insert(verts[face.v[c]].surface);
				}
				indices.push_back(r);
			}
		}
		if (used.is_empty()) {
			continue;
		}

		// The union of what the slot's source surfaces carry.
		uint64_t any = 0;
		uint64_t all = ~uint64_t(0);
		bool eight_bones = false;
		int custom_format[CUSTOM_CHANNELS];
		for (int c = 0; c < CUSTOM_CHANNELS; c++) {
			custom_format[c] = -2;
		}
		for (const int src : slot_sources) {
			const uint64_t format = sources[src].format;
			any |= format;
			all &= format;
			eight_bones = eight_bones || (format & Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS);
			for (int c = 0; c < CUSTOM_CHANNELS; c++) {
				const int cf = (format & (Mesh::ARRAY_FORMAT_CUSTOM0 << c)) ? int((format >> (Mesh::ARRAY_FORMAT_CUSTOM_BASE + c * Mesh::ARRAY_FORMAT_CUSTOM_BITS)) & Mesh::ARRAY_FORMAT_CUSTOM_MASK) : -1;
				if (custom_format[c] == -2) {
					custom_format[c] = cf;
				} else if (custom_format[c] != cf) {
					custom_format[c] = -1; // Mixed formats: dropped.
				}
			}
		}
		const int bones_per_vertex = eight_bones ? 8 : 4;
		const int count = used.size();

		Vector<Vector3> out_vertices;
		Vector<Vector3> out_normals;
		Vector<float> out_tangents;
		Vector<Color> out_colors;
		Vector<Vector2> out_uvs;
		Vector<Vector2> out_uv2s;
		Vector<int> out_bones;
		Vector<float> out_weights;
		Vector<uint8_t> out_custom_bytes[CUSTOM_CHANNELS];
		Vector<float> out_custom_floats[CUSTOM_CHANNELS];

		const bool has_normal = any & Mesh::ARRAY_FORMAT_NORMAL;
		bool has_tangent = (any & Mesh::ARRAY_FORMAT_TANGENT) && has_normal;
		const bool has_color = any & Mesh::ARRAY_FORMAT_COLOR;
		const bool has_uv2 = any & Mesh::ARRAY_FORMAT_TEX_UV2;
		const bool has_bones = (any & Mesh::ARRAY_FORMAT_BONES) && (any & Mesh::ARRAY_FORMAT_WEIGHTS);

		out_vertices.resize(count);
		out_uvs.resize(count);
		if (has_normal) {
			out_normals.resize(count);
		}
		if (has_tangent) {
			out_tangents.resize(count * 4);
		}
		if (has_color) {
			out_colors.resize(count);
		}
		if (has_uv2) {
			out_uv2s.resize(count);
		}
		if (has_bones) {
			out_bones.resize(count * bones_per_vertex);
			out_weights.resize(count * bones_per_vertex);
		}
		for (int c = 0; c < CUSTOM_CHANNELS; c++) {
			if (custom_format[c] < 0) {
				continue;
			}
			bool is_float = false;
			const int n = custom_format_components(custom_format[c], is_float);
			if (is_float) {
				out_custom_floats[c].resize(count * n);
			} else {
				out_custom_bytes[c].resize(count * n);
			}
		}

		// Blend shape targets, per shape.
		LocalVector<Vector<Vector3>> blend_vertices;
		LocalVector<Vector<Vector3>> blend_normals;
		LocalVector<Vector<float>> blend_tangents;
		blend_vertices.resize(blend_count);
		blend_normals.resize(blend_count);
		blend_tangents.resize(blend_count);
		for (int b = 0; b < blend_count; b++) {
			blend_vertices[b].resize(count);
			if (has_normal) {
				blend_normals[b].resize(count);
			}
			if (has_tangent) {
				blend_tangents[b].resize(count * 4);
			}
		}

		for (int i = 0; i < count; i++) {
			const Vert &vert = verts[used[i]];
			const SourceSurface &src = sources[vert.surface];
			const int vi = vert.index;
			out_vertices.write[i] = src.vertices[vi];
			out_uvs.write[i] = vert.uv;
			if (has_normal) {
				out_normals.write[i] = src.normals.size() == src.vertex_count ? src.normals[vi] : Vector3(0, 1, 0);
			}
			if (has_tangent) {
				for (int k = 0; k < 4; k++) {
					out_tangents.write[i * 4 + k] = src.tangents.size() == src.vertex_count * 4 ? src.tangents[vi * 4 + k] : (k == 0 || k == 3 ? 1.0f : 0.0f);
				}
			}
			if (has_color) {
				out_colors.write[i] = src.colors.size() == src.vertex_count ? src.colors[vi] : Color(1, 1, 1, 1);
			}
			if (has_uv2) {
				out_uv2s.write[i] = src.uv2s.size() == src.vertex_count ? src.uv2s[vi] : Vector2();
			}
			if (has_bones) {
				const Vector<int> &sb = src.bones;
				const Vector<float> &sw = src.weights;
				const int src_bpv = src.vertex_count > 0 && sb.size() > 0 ? sb.size() / src.vertex_count : 0;
				for (int k = 0; k < bones_per_vertex; k++) {
					const bool from_source = k < src_bpv && sw.size() == sb.size();
					out_bones.write[i * bones_per_vertex + k] = from_source ? sb[vi * src_bpv + k] : 0;
					// A vertex from an unskinned surface follows bone 0 instead of collapsing.
					out_weights.write[i * bones_per_vertex + k] = from_source ? sw[vi * src_bpv + k] : (src_bpv == 0 && k == 0 ? 1.0f : 0.0f);
				}
			}
			for (int c = 0; c < CUSTOM_CHANNELS; c++) {
				if (custom_format[c] < 0) {
					continue;
				}
				bool is_float = false;
				const int n = custom_format_components(custom_format[c], is_float);
				if (is_float) {
					const Vector<float> data = src.arrays[Mesh::ARRAY_CUSTOM0 + c];
					for (int k = 0; k < n; k++) {
						out_custom_floats[c].write[i * n + k] = data.size() == src.vertex_count * n ? data[vi * n + k] : 0.0f;
					}
				} else {
					const Vector<uint8_t> data = src.arrays[Mesh::ARRAY_CUSTOM0 + c];
					for (int k = 0; k < n; k++) {
						out_custom_bytes[c].write[i * n + k] = data.size() == src.vertex_count * n ? data[vi * n + k] : 0;
					}
				}
			}
			for (int b = 0; b < blend_count; b++) {
				const BlendTarget &target = blend_targets[vert.surface][b];
				blend_vertices[b].write[i] = target.vertices.size() == src.vertex_count ? target.vertices[vi] : out_vertices[i];
				if (has_normal) {
					blend_normals[b].write[i] = target.normals.size() == src.vertex_count ? target.normals[vi] : out_normals[i];
				}
				if (has_tangent) {
					for (int k = 0; k < 4; k++) {
						blend_tangents[b].write[i * 4 + k] = target.tangents.size() == src.vertex_count * 4 ? target.tangents[vi * 4 + k] : out_tangents[i * 4 + k];
					}
				}
			}
		}

		// New UVs make the old tangents wrong for normal maps: generate them again (MikkTSpace).
		if (has_tangent && uvs_changed) {
			Array minimal;
			minimal.resize(Mesh::ARRAY_MAX);
			minimal[Mesh::ARRAY_VERTEX] = out_vertices;
			minimal[Mesh::ARRAY_NORMAL] = out_normals;
			minimal[Mesh::ARRAY_TEX_UV] = out_uvs;
			minimal[Mesh::ARRAY_INDEX] = indices;
			Ref<SurfaceTool> st;
			st.instantiate();
			st->create_from_arrays(minimal);
			st->generate_tangents();
			const Array generated = st->commit_to_arrays();
			const Vector<float> tangents = generated[Mesh::ARRAY_TANGENT];
			if (tangents.size() == out_tangents.size()) {
				out_tangents = tangents;
			}
		}

		Array arrays;
		arrays.resize(Mesh::ARRAY_MAX);
		arrays[Mesh::ARRAY_VERTEX] = out_vertices;
		arrays[Mesh::ARRAY_TEX_UV] = out_uvs;
		arrays[Mesh::ARRAY_INDEX] = indices;
		if (has_normal) {
			arrays[Mesh::ARRAY_NORMAL] = out_normals;
		}
		if (has_tangent) {
			arrays[Mesh::ARRAY_TANGENT] = out_tangents;
		}
		if (has_color) {
			arrays[Mesh::ARRAY_COLOR] = out_colors;
		}
		if (has_uv2) {
			arrays[Mesh::ARRAY_TEX_UV2] = out_uv2s;
		}
		if (has_bones) {
			arrays[Mesh::ARRAY_BONES] = out_bones;
			arrays[Mesh::ARRAY_WEIGHTS] = out_weights;
		}
		uint64_t flags = eight_bones && has_bones ? uint64_t(Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS) : 0;
		for (int c = 0; c < CUSTOM_CHANNELS; c++) {
			if (custom_format[c] < 0) {
				continue;
			}
			bool is_float = false;
			custom_format_components(custom_format[c], is_float);
			if (is_float) {
				arrays[Mesh::ARRAY_CUSTOM0 + c] = out_custom_floats[c];
			} else {
				arrays[Mesh::ARRAY_CUSTOM0 + c] = out_custom_bytes[c];
			}
			flags |= uint64_t(custom_format[c]) << (Mesh::ARRAY_FORMAT_CUSTOM_BASE + c * Mesh::ARRAY_FORMAT_CUSTOM_BITS);
		}

		TypedArray<Array> blend_arrays;
		for (int b = 0; b < blend_count; b++) {
			Array shape;
			shape.resize(Mesh::ARRAY_MAX);
			shape[Mesh::ARRAY_VERTEX] = blend_vertices[b];
			if (has_normal) {
				shape[Mesh::ARRAY_NORMAL] = blend_normals[b];
			}
			if (has_tangent) {
				shape[Mesh::ARRAY_TANGENT] = blend_tangents[b];
			}
			blend_arrays.push_back(shape);
		}

		// Index LODs, like the scene importer makes, when the mesh had them.
		Dictionary lods;
		if (had_lods && p_generate_lods && SurfaceTool::simplify_scale_func && SurfaceTool::simplify_with_attrib_func) {
			Ref<ImporterMesh> lod_maker;
			lod_maker.instantiate();
			lod_maker->add_surface(Mesh::PRIMITIVE_TRIANGLES, arrays);
			lod_maker->generate_lods(60.0, Array());
			for (int l = 0; l < lod_maker->get_surface_lod_count(0); l++) {
				lods[lod_maker->get_surface_lod_size(0, l)] = lod_maker->get_surface_lod_indices(0, l);
			}
		}

		const int surface = out->get_surface_count();
		out->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, blend_arrays, lods, flags);
		ERR_CONTINUE(out->get_surface_count() == surface);
		out->surface_set_material(surface, slots[s].material);
		out->surface_set_name(surface, slots[s].name);
	}

	// Line and point surfaces, as they were.
	if (loaded_mesh.is_valid()) {
		for (int s = 0; s < loaded_mesh->get_surface_count() && s < (int)sources.size(); s++) {
			if (sources[s].vertex_count >= 0) {
				continue;
			}
			const int surface = out->get_surface_count();
			out->add_surface_from_arrays(loaded_mesh->surface_get_primitive_type(s), sources[s].arrays, sources[s].blend_shapes.size() == blend_count ? TypedArray<Array>(sources[s].blend_shapes) : TypedArray<Array>(), Dictionary(), sources[s].format);
			if (out->get_surface_count() > surface) {
				out->surface_set_material(surface, loaded_mesh->surface_get_material(s));
				out->surface_set_name(surface, sources[s].name);
			}
		}
	}

	out->set_custom_aabb(custom_aabb);
	out->set_lightmap_size_hint(lightmap_size_hint);
	if (shadow_mesh.is_valid()) {
		// The shadow mesh only holds positions, which painting never moves.
		out->set_shadow_mesh(shadow_mesh);
	}
	committed_mesh = out;
	return out;
}

// Slots.

int MeshPainter::add_slot(const Ref<Material> &p_material, const String &p_name) {
	Slot slot;
	slot.material = p_material;
	slot.name = p_name.is_empty() ? vformat("Material %d", slots.size() + 1) : p_name;
	slots.push_back(slot);
	return slots.size() - 1;
}

void MeshPainter::remove_slot(int p_slot) {
	ERR_FAIL_INDEX(p_slot, (int)slots.size());
	ERR_FAIL_COND_MSG(slots.size() <= 1, "A mesh keeps at least one material slot.");
	const int fallback = p_slot == 0 ? 1 : 0;
	for (Face &face : faces) {
		if (face.slot == p_slot) {
			face.slot = fallback;
		}
	}
	for (Face &face : faces) {
		if (face.slot > p_slot) {
			face.slot--;
		}
	}
	slots.remove_at(p_slot);
}

void MeshPainter::set_slot_material(int p_slot, const Ref<Material> &p_material) {
	ERR_FAIL_INDEX(p_slot, (int)slots.size());
	slots[p_slot].material = p_material;
}

Ref<Material> MeshPainter::get_slot_material(int p_slot) const {
	ERR_FAIL_INDEX_V(p_slot, (int)slots.size(), Ref<Material>());
	return slots[p_slot].material;
}

void MeshPainter::set_slot_name(int p_slot, const String &p_name) {
	ERR_FAIL_INDEX(p_slot, (int)slots.size());
	slots[p_slot].name = p_name;
}

String MeshPainter::get_slot_name(int p_slot) const {
	ERR_FAIL_INDEX_V(p_slot, (int)slots.size(), String());
	return slots[p_slot].name;
}

int MeshPainter::get_slot_face_count(int p_slot) const {
	int count = 0;
	for (const Face &face : faces) {
		count += face.slot == p_slot ? 1 : 0;
	}
	return count;
}

PackedInt32Array MeshPainter::get_slot_faces(int p_slot) const {
	PackedInt32Array result;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (faces[f].slot == p_slot) {
			result.push_back(f);
		}
	}
	return result;
}

void MeshPainter::set_faces_slot(const PackedInt32Array &p_faces, int p_slot) {
	ERR_FAIL_INDEX(p_slot, (int)slots.size());
	for (const int f : p_faces) {
		ERR_CONTINUE(f < 0 || f >= (int)faces.size());
		faces[f].slot = p_slot;
	}
}

int MeshPainter::get_face_slot(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, (int)faces.size(), -1);
	return faces[p_face].slot;
}

// Selection.

void MeshPainter::set_face_selected(int p_face, bool p_selected) {
	ERR_FAIL_INDEX(p_face, (int)faces.size());
	selected[p_face] = p_selected ? 1 : 0;
}

bool MeshPainter::is_face_selected(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, (int)faces.size(), false);
	return selected[p_face];
}

PackedInt32Array MeshPainter::get_selected_faces() const {
	PackedInt32Array result;
	for (uint32_t f = 0; f < selected.size(); f++) {
		if (selected[f]) {
			result.push_back(f);
		}
	}
	return result;
}

void MeshPainter::set_selected_faces(const PackedInt32Array &p_faces) {
	memset(selected.ptr(), 0, selected.size());
	for (const int f : p_faces) {
		if (f >= 0 && f < (int)selected.size()) {
			selected[f] = 1;
		}
	}
}

int MeshPainter::get_selected_face_count() const {
	int count = 0;
	for (const uint8_t s : selected) {
		count += s;
	}
	return count;
}

void MeshPainter::select_all(bool p_select) {
	memset(selected.ptr(), p_select ? 1 : 0, selected.size());
}

void MeshPainter::invert_selection() {
	for (uint8_t &s : selected) {
		s = s ? 0 : 1;
	}
}

void MeshPainter::grow_selection() {
	_update_topology();
	LocalVector<uint8_t> grown(selected);
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (!selected[f]) {
			continue;
		}
		for (int c = 0; c < 3; c++) {
			const int w = vert_weld[faces[f].v[c]];
			for (int i = weld_face_offsets[w]; i < weld_face_offsets[w + 1]; i++) {
				grown[weld_faces[i]] = 1;
			}
		}
	}
	selected = grown;
}

void MeshPainter::shrink_selection() {
	_update_topology();
	LocalVector<uint8_t> shrunk(selected);
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (!selected[f]) {
			continue;
		}
		for (int c = 0; c < 3 && shrunk[f]; c++) {
			const int w = vert_weld[faces[f].v[c]];
			for (int i = weld_face_offsets[w]; i < weld_face_offsets[w + 1]; i++) {
				if (!selected[weld_faces[i]]) {
					shrunk[f] = 0;
					break;
				}
			}
		}
	}
	selected = shrunk;
}

void MeshPainter::select_islands_of_selection() {
	_update_islands();
	LocalVector<uint8_t> islands;
	islands.resize(island_count);
	memset(islands.ptr(), 0, islands.size());
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (selected[f]) {
			islands[face_island[f]] = 1;
		}
	}
	for (uint32_t f = 0; f < faces.size(); f++) {
		selected[f] = islands[face_island[f]];
	}
}

void MeshPainter::select_pieces_of_selection() {
	_update_islands();
	LocalVector<uint8_t> pieces;
	pieces.resize(piece_count);
	memset(pieces.ptr(), 0, pieces.size());
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (selected[f]) {
			pieces[face_piece[f]] = 1;
		}
	}
	for (uint32_t f = 0; f < faces.size(); f++) {
		selected[f] = pieces[face_piece[f]];
	}
}

// Islands.

int MeshPainter::get_island_count() {
	_update_islands();
	return island_count;
}

int MeshPainter::get_face_island(int p_face) {
	_update_islands();
	ERR_FAIL_INDEX_V(p_face, (int)faces.size(), -1);
	return face_island[p_face];
}

PackedInt32Array MeshPainter::get_island_faces(int p_island) {
	_update_islands();
	PackedInt32Array result;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (face_island[f] == p_island) {
			result.push_back(f);
		}
	}
	return result;
}

PackedInt32Array MeshPainter::get_islands_of_faces(const PackedInt32Array &p_faces) {
	_update_islands();
	HashSet<int> islands;
	PackedInt32Array result;
	for (const int f : p_faces) {
		ERR_CONTINUE(f < 0 || f >= (int)faces.size());
		if (!islands.has(face_island[f])) {
			islands.insert(face_island[f]);
			result.push_back(face_island[f]);
		}
	}
	return result;
}

Rect2 MeshPainter::get_island_uv_rect(int p_island) {
	_update_islands();
	Rect2 rect;
	bool first = true;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (face_island[f] != p_island) {
			continue;
		}
		for (int c = 0; c < 3; c++) {
			const Vector2 uv = verts[faces[f].v[c]].uv;
			if (first) {
				rect = Rect2(uv, Vector2());
				first = false;
			} else {
				rect.expand_to(uv);
			}
		}
	}
	return rect;
}

int MeshPainter::get_face_piece(int p_face) {
	_update_islands();
	ERR_FAIL_INDEX_V(p_face, (int)faces.size(), -1);
	return face_piece[p_face];
}

PackedInt32Array MeshPainter::get_piece_faces(int p_piece) {
	_update_islands();
	PackedInt32Array result;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (face_piece[f] == p_piece) {
			result.push_back(f);
		}
	}
	return result;
}

// UVs.

Vector2 MeshPainter::get_face_uv(int p_face, int p_corner) const {
	ERR_FAIL_INDEX_V(p_face, (int)faces.size(), Vector2());
	ERR_FAIL_INDEX_V(p_corner, 3, Vector2());
	return verts[faces[p_face].v[p_corner]].uv;
}

PackedVector2Array MeshPainter::get_face_uvs() const {
	PackedVector2Array result;
	result.resize(faces.size() * 3);
	Vector2 *w = result.ptrw();
	for (uint32_t f = 0; f < faces.size(); f++) {
		for (int c = 0; c < 3; c++) {
			w[f * 3 + c] = verts[faces[f].v[c]].uv;
		}
	}
	return result;
}

void MeshPainter::_set_corner_uvs(const LocalVector<int> &p_faces, const Vector<Vector2> &p_corner_uvs) {
	ERR_FAIL_COND(p_corner_uvs.size() != (int)p_faces.size() * 3);
	// Every face corner as (source vertex, UV), then the vertices rebuilt from those so a
	// vertex shared by faces that now disagree on its UV is split, and unused ones go away.
	LocalVector<Vector2> corner_uv;
	corner_uv.resize(faces.size() * 3);
	for (uint32_t f = 0; f < faces.size(); f++) {
		for (int c = 0; c < 3; c++) {
			corner_uv[f * 3 + c] = verts[faces[f].v[c]].uv;
		}
	}
	for (uint32_t i = 0; i < p_faces.size(); i++) {
		for (int c = 0; c < 3; c++) {
			corner_uv[p_faces[i] * 3 + c] = p_corner_uvs[i * 3 + c];
		}
	}

	LocalVector<Vert> new_verts;
	HashMap<VertKey, int, VertKeyHasher> ids;
	for (uint32_t f = 0; f < faces.size(); f++) {
		for (int c = 0; c < 3; c++) {
			const Vert &old = verts[faces[f].v[c]];
			const Vector2 uv = corner_uv[f * 3 + c];
			VertKey key;
			key.surface = old.surface;
			key.index = old.index;
			key.u = quantize_uv(uv.x);
			key.v = quantize_uv(uv.y);
			HashMap<VertKey, int, VertKeyHasher>::Iterator E = ids.find(key);
			if (!E) {
				Vert vert;
				vert.surface = old.surface;
				vert.index = old.index;
				vert.uv = uv;
				E = ids.insert(key, new_verts.size());
				new_verts.push_back(vert);
			}
			faces[f].v[c] = E->value;
		}
	}
	// Positions follow the vertices (posed positions are kept through the old vertex).
	LocalVector<Vector3> new_positions;
	new_positions.resize(new_verts.size());
	{
		HashMap<int64_t, Vector3> by_source;
		for (uint32_t i = 0; i < verts.size(); i++) {
			by_source[(int64_t(verts[i].surface) << 32) | int64_t(verts[i].index)] = positions[i];
		}
		for (uint32_t i = 0; i < new_verts.size(); i++) {
			new_positions[i] = by_source[(int64_t(new_verts[i].surface) << 32) | int64_t(new_verts[i].index)];
		}
	}
	verts = new_verts;
	positions = new_positions;
	uvs_present = true;
	uvs_changed = true;
	topology_dirty = true;
	islands_dirty = true;
}

void MeshPainter::transform_faces_uv(const PackedInt32Array &p_faces, const Transform2D &p_transform) {
	HashSet<int> moved;
	for (const int f : p_faces) {
		ERR_CONTINUE(f < 0 || f >= (int)faces.size());
		for (int c = 0; c < 3; c++) {
			moved.insert(faces[f].v[c]);
		}
	}
	for (const int v : moved) {
		verts[v].uv = p_transform.xform(verts[v].uv);
	}
	uvs_present = true;
	uvs_changed = true;
	islands_dirty = true;
}

Vector2 MeshPainter::_uv_density_scale(const LocalVector<int> &p_faces) const {
	// x: UV area, y: 3D area.
	real_t uv_area = 0.0;
	real_t area = 0.0;
	for (const int f : p_faces) {
		const Face &face = faces[f];
		uv_area += triangle_area_2d(verts[face.v[0]].uv, verts[face.v[1]].uv, verts[face.v[2]].uv);
		area += Face3(positions[face.v[0]], positions[face.v[1]], positions[face.v[2]]).get_area();
	}
	return Vector2(uv_area, area);
}

Error MeshPainter::smart_unwrap(const PackedInt32Array &p_faces, float p_angle_limit, int p_padding, int p_resolution) {
	ERR_FAIL_NULL_V_MSG(unwrap_func, ERR_UNAVAILABLE, "UV unwrapping needs the xatlas_unwrap module.");
	ERR_FAIL_COND_V(faces.is_empty(), ERR_UNCONFIGURED);

	LocalVector<int> subset;
	if (p_faces.is_empty()) {
		subset.resize(faces.size());
		for (uint32_t f = 0; f < faces.size(); f++) {
			subset[f] = f;
		}
	} else {
		for (const int f : p_faces) {
			ERR_CONTINUE(f < 0 || f >= (int)faces.size());
			subset.push_back(f);
		}
	}
	ERR_FAIL_COND_V(subset.is_empty(), ERR_INVALID_PARAMETER);
	const bool partial = subset.size() < faces.size();

	// One input vertex per source vertex: old UV seams do not matter to the new unwrap.
	HashMap<int64_t, int> input_ids;
	Vector<Vector3> input_positions;
	Vector<Vector3> input_normals;
	Vector<int> indices;
	Vector<uint32_t> groups;
	for (const int f : subset) {
		for (int c = 0; c < 3; c++) {
			const Vert &vert = verts[faces[f].v[c]];
			const int64_t key = (int64_t(vert.surface) << 32) | int64_t(vert.index);
			HashMap<int64_t, int>::Iterator E = input_ids.find(key);
			if (!E) {
				E = input_ids.insert(key, input_positions.size());
				const Vector<Vector3> &sv = sources[vert.surface].vertices;
				const Vector<Vector3> &sn = sources[vert.surface].normals;
				input_positions.push_back(sv[vert.index]);
				input_normals.push_back(sn.size() == sv.size() ? sn[vert.index] : face_normals[f]);
			}
			indices.push_back(E->value);
		}
		// Islands never cross material slots.
		groups.push_back(faces[f].slot);
	}

	UnwrapOptions options;
	options.angle_limit = p_angle_limit;
	options.padding = p_padding;
	options.resolution = p_resolution;
	Vector<Vector2> corner_uvs;
	Vector<int> corner_xref;
	ERR_FAIL_COND_V_MSG(!unwrap_func(options, input_positions, input_normals, Vector<Vector2>(), indices, groups, corner_uvs, corner_xref), ERR_CANT_CREATE, "UV unwrap failed.");
	ERR_FAIL_COND_V(corner_uvs.size() != indices.size(), ERR_BUG);

	if (partial) {
		// New islands get the texel density of the rest, then everything is packed again.
		LocalVector<int> rest;
		{
			LocalVector<uint8_t> in_subset;
			in_subset.resize(faces.size());
			memset(in_subset.ptr(), 0, in_subset.size());
			for (const int f : subset) {
				in_subset[f] = 1;
			}
			for (uint32_t f = 0; f < faces.size(); f++) {
				if (!in_subset[f]) {
					rest.push_back(f);
				}
			}
		}
		const Vector2 rest_density = _uv_density_scale(rest);
		real_t new_uv_area = 0.0;
		real_t new_area = 0.0;
		for (uint32_t i = 0; i < subset.size(); i++) {
			const Face &face = faces[subset[i]];
			new_uv_area += triangle_area_2d(corner_uvs[i * 3], corner_uvs[i * 3 + 1], corner_uvs[i * 3 + 2]);
			new_area += Face3(positions[face.v[0]], positions[face.v[1]], positions[face.v[2]]).get_area();
		}
		if (rest_density.x > 0 && rest_density.y > 0 && new_uv_area > 0 && new_area > 0) {
			const real_t scale = Math::sqrt((rest_density.x / rest_density.y) / (new_uv_area / new_area));
			for (int i = 0; i < corner_uvs.size(); i++) {
				corner_uvs.write[i] *= scale;
			}
		}
		_set_corner_uvs(subset, corner_uvs);
		return pack_islands(p_padding, p_resolution, true, false);
	}

	_set_corner_uvs(subset, corner_uvs);
	return OK;
}

Error MeshPainter::pack_islands(int p_padding, int p_resolution, bool p_rotate, bool p_average_scale) {
	ERR_FAIL_NULL_V_MSG(unwrap_func, ERR_UNAVAILABLE, "UV packing needs the xatlas_unwrap module.");
	ERR_FAIL_COND_V(faces.is_empty(), ERR_UNCONFIGURED);
	_update_islands();

	LocalVector<int> all;
	all.resize(faces.size());
	for (uint32_t f = 0; f < faces.size(); f++) {
		all[f] = f;
	}
	Vector<Vector2> corner_uvs;
	corner_uvs.resize(faces.size() * 3);
	for (uint32_t f = 0; f < faces.size(); f++) {
		for (int c = 0; c < 3; c++) {
			corner_uvs.write[f * 3 + c] = verts[faces[f].v[c]].uv;
		}
	}

	if (p_average_scale) {
		// Every island gets the same texels per meter.
		LocalVector<real_t> uv_area;
		LocalVector<real_t> area;
		LocalVector<Vector2> center;
		LocalVector<int> corner_count;
		uv_area.resize(island_count);
		area.resize(island_count);
		center.resize(island_count);
		corner_count.resize(island_count);
		for (int i = 0; i < island_count; i++) {
			uv_area[i] = 0;
			area[i] = 0;
			center[i] = Vector2();
			corner_count[i] = 0;
		}
		real_t total_uv = 0.0;
		real_t total_area = 0.0;
		for (uint32_t f = 0; f < faces.size(); f++) {
			const int island = face_island[f];
			const real_t a2 = triangle_area_2d(corner_uvs[f * 3], corner_uvs[f * 3 + 1], corner_uvs[f * 3 + 2]);
			const real_t a3 = Face3(positions[faces[f].v[0]], positions[faces[f].v[1]], positions[faces[f].v[2]]).get_area();
			uv_area[island] += a2;
			area[island] += a3;
			total_uv += a2;
			total_area += a3;
			for (int c = 0; c < 3; c++) {
				center[island] += corner_uvs[f * 3 + c];
				corner_count[island]++;
			}
		}
		if (total_uv > 0 && total_area > 0) {
			const real_t target = total_uv / total_area;
			for (uint32_t f = 0; f < faces.size(); f++) {
				const int island = face_island[f];
				if (uv_area[island] <= 0 || area[island] <= 0) {
					continue;
				}
				const real_t scale = Math::sqrt(target / (uv_area[island] / area[island]));
				const Vector2 pivot = center[island] / MAX(corner_count[island], 1);
				for (int c = 0; c < 3; c++) {
					corner_uvs.write[f * 3 + c] = pivot + (corner_uvs[f * 3 + c] - pivot) * scale;
				}
			}
		}
	}

	// One input vertex per corner, welded by UV within an island, so each island is one chart.
	Vector<Vector2> input_uvs;
	Vector<int> indices;
	Vector<uint32_t> groups;
	{
		_update_topology();
		HashMap<VertKey, int, VertKeyHasher> ids;
		for (uint32_t f = 0; f < faces.size(); f++) {
			for (int c = 0; c < 3; c++) {
				const Vector2 uv = corner_uvs[f * 3 + c];
				VertKey key;
				key.surface = face_island[f];
				key.index = vert_weld[faces[f].v[c]];
				key.u = quantize_uv(uv.x);
				key.v = quantize_uv(uv.y);
				HashMap<VertKey, int, VertKeyHasher>::Iterator E = ids.find(key);
				if (!E) {
					E = ids.insert(key, input_uvs.size());
					input_uvs.push_back(uv);
				}
				indices.push_back(E->value);
			}
			groups.push_back(face_island[f]);
		}
	}

	UnwrapOptions options;
	options.padding = p_padding;
	options.resolution = p_resolution;
	options.rotate = p_rotate;
	options.pack_only = true;
	Vector<Vector2> packed;
	Vector<int> xref;
	ERR_FAIL_COND_V_MSG(!unwrap_func(options, Vector<Vector3>(), Vector<Vector3>(), input_uvs, indices, groups, packed, xref), ERR_CANT_CREATE, "UV packing failed.");
	ERR_FAIL_COND_V(packed.size() != indices.size(), ERR_BUG);
	_set_corner_uvs(all, packed);
	return OK;
}

// Geometry queries.

bool MeshPainter::intersect_ray_face(const Vector3 &p_from, const Vector3 &p_dir, int &r_face, Vector3 &r_position, Vector3 &r_normal) const {
	if (triangle_mesh.is_null() || !triangle_mesh->is_valid()) {
		return false;
	}
	int32_t face = -1;
	if (!triangle_mesh->intersect_ray(p_from, p_dir, r_position, r_normal, nullptr, &face) || face < 0 || face >= (int)faces.size()) {
		return false;
	}
	r_face = face;
	r_normal = face_normals[face];
	return true;
}

Dictionary MeshPainter::intersect_ray(const Vector3 &p_from, const Vector3 &p_dir) const {
	Dictionary result;
	int face = -1;
	Vector3 position;
	Vector3 normal;
	if (intersect_ray_face(p_from, p_dir, face, position, normal)) {
		result["face"] = face;
		result["position"] = position;
		result["normal"] = normal;
		result["barycentric"] = get_barycentric(face, position);
	}
	return result;
}

bool MeshPainter::_face_visible(int p_face, const Vector3 &p_eye) {
	if (stroke_eye != p_eye) {
		stroke_visibility.clear();
		stroke_eye = p_eye;
	}
	HashMap<int, bool>::Iterator E = stroke_visibility.find(p_face);
	if (E) {
		return E->value;
	}
	const Face &face = faces[p_face];
	const Vector3 a = positions[face.v[0]];
	const Vector3 b = positions[face.v[1]];
	const Vector3 c = positions[face.v[2]];
	const Vector3 centroid = (a + b + c) / 3.0;
	const Vector3 samples[4] = { centroid, a.lerp(centroid, 0.1), b.lerp(centroid, 0.1), c.lerp(centroid, 0.1) };
	const real_t epsilon = weld_epsilon * 50.0;
	bool visible = false;
	for (const Vector3 &sample : samples) {
		const Vector3 to_eye = p_eye - sample;
		const real_t distance = to_eye.length();
		if (distance <= epsilon) {
			visible = true;
			break;
		}
		const Vector3 end = sample + to_eye / distance * epsilon;
		Vector3 hit;
		Vector3 hit_normal;
		int32_t hit_face = -1;
		if (!triangle_mesh->intersect_segment(p_eye, end, hit, hit_normal, nullptr, &hit_face) || hit_face == p_face || hit.distance_to(sample) <= epsilon * 2.0) {
			visible = true;
			break;
		}
	}
	stroke_visibility.insert(p_face, visible);
	return visible;
}

PackedInt32Array MeshPainter::get_faces_in_sphere(const Vector3 &p_center, real_t p_radius, const Vector3 &p_eye, bool p_front_only) {
	PackedInt32Array result;
	const AABB sphere(p_center - Vector3(p_radius, p_radius, p_radius), Vector3(p_radius, p_radius, p_radius) * 2.0);
	const Vector3 view = p_center - p_eye;
	for (uint32_t f = 0; f < faces.size(); f++) {
		if (!face_bounds[f].intersects(sphere)) {
			continue;
		}
		const Face &face = faces[f];
		const Face3 tri(positions[face.v[0]], positions[face.v[1]], positions[face.v[2]]);
		if (tri.get_closest_point_to(p_center).distance_to(p_center) > p_radius) {
			continue;
		}
		if (p_front_only) {
			if (face_normals[f].dot(view) > 0) {
				continue;
			}
			if (!_face_visible(f, p_eye)) {
				continue;
			}
		}
		result.push_back(f);
	}
	return result;
}

Vector3 MeshPainter::get_face_normal(int p_face) const {
	ERR_FAIL_INDEX_V(p_face, (int)faces.size(), Vector3());
	return face_normals[p_face];
}

Vector3 MeshPainter::get_face_position(int p_face, int p_corner) const {
	ERR_FAIL_INDEX_V(p_face, (int)faces.size(), Vector3());
	ERR_FAIL_INDEX_V(p_corner, 3, Vector3());
	return positions[faces[p_face].v[p_corner]];
}

Vector3 MeshPainter::get_barycentric(int p_face, const Vector3 &p_point) const {
	ERR_FAIL_INDEX_V(p_face, (int)faces.size(), Vector3());
	const Face &face = faces[p_face];
	const Vector3 a = positions[face.v[0]];
	const Vector3 v0 = positions[face.v[1]] - a;
	const Vector3 v1 = positions[face.v[2]] - a;
	const Vector3 v2 = p_point - a;
	const real_t d00 = v0.dot(v0);
	const real_t d01 = v0.dot(v1);
	const real_t d11 = v1.dot(v1);
	const real_t d20 = v2.dot(v0);
	const real_t d21 = v2.dot(v1);
	const real_t denom = d00 * d11 - d01 * d01;
	if (Math::abs(denom) < CMP_EPSILON2) {
		return Vector3(1, 0, 0);
	}
	const real_t v = (d11 * d20 - d01 * d21) / denom;
	const real_t w = (d00 * d21 - d01 * d20) / denom;
	return Vector3(1.0 - v - w, v, w);
}

PackedVector3Array MeshPainter::get_faces_triangles(const PackedInt32Array &p_faces, real_t p_offset) const {
	PackedVector3Array result;
	result.resize(p_faces.size() * 3);
	Vector3 *w = result.ptrw();
	int count = 0;
	for (const int f : p_faces) {
		if (f < 0 || f >= (int)faces.size()) {
			continue;
		}
		const Vector3 offset = face_normals[f] * p_offset;
		for (int c = 0; c < 3; c++) {
			w[count * 3 + c] = positions[faces[f].v[c]] + offset;
		}
		count++;
	}
	result.resize(count * 3);
	return result;
}

AABB MeshPainter::get_aabb() const {
	AABB bounds;
	for (uint32_t f = 0; f < face_bounds.size(); f++) {
		bounds = f == 0 ? face_bounds[f] : bounds.merge(face_bounds[f]);
	}
	return bounds;
}

// Painting.

void MeshPainter::begin_stroke() {
	stroke_images.clear();
	stroke_visibility.clear();
}

MeshPainter::StrokeImage &MeshPainter::_stroke_image(const Ref<Image> &p_image) {
	HashMap<Image *, StrokeImage>::Iterator E = stroke_images.find(p_image.ptr());
	if (E) {
		return E->value;
	}
	StrokeImage stroke;
	stroke.original.instantiate();
	stroke.original->copy_internals_from(p_image);
	stroke.mask.resize(p_image->get_width() * p_image->get_height());
	memset(stroke.mask.ptr(), 0, sizeof(float) * stroke.mask.size());
	return stroke_images.insert(p_image.ptr(), stroke)->value;
}

void MeshPainter::_blend_texel(StrokeImage &p_stroke, const Ref<Image> &p_image, int p_x, int p_y, float p_alpha, const Color &p_color, MeshPaintBrush::BlendMode p_mode) {
	const int w = p_image->get_width();
	const int index = p_y * w + p_x;
	uint8_t *data = p_image->ptrw();
	uint8_t *texel = data + index * 4;

	if (p_mode == MeshPaintBrush::BLEND_BLUR) {
		const int h = p_image->get_height();
		float sum[4] = { 0, 0, 0, 0 };
		int count = 0;
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				const int x = p_x + dx;
				const int y = p_y + dy;
				if (x < 0 || y < 0 || x >= w || y >= h) {
					continue;
				}
				const uint8_t *n = data + (y * w + x) * 4;
				for (int k = 0; k < 4; k++) {
					sum[k] += n[k];
				}
				count++;
			}
		}
		const float amount = p_alpha * 0.5f;
		for (int k = 0; k < 4; k++) {
			texel[k] = (uint8_t)CLAMP(Math::round(Math::lerp((float)texel[k], sum[k] / count, amount)), 0.0f, 255.0f);
		}
		return;
	}

	// One stroke never paints a texel more than its strongest dab did.
	float &mask = p_stroke.mask[index];
	if (p_alpha <= mask) {
		return;
	}
	mask = p_alpha;
	const uint8_t *o = p_stroke.original->ptr() + index * 4;
	const Color base(o[0] / 255.0f, o[1] / 255.0f, o[2] / 255.0f, o[3] / 255.0f);
	const float m = mask * p_color.a;
	Color result = base;
	switch (p_mode) {
		case MeshPaintBrush::BLEND_MIX: {
			result.r = Math::lerp(base.r, p_color.r, m);
			result.g = Math::lerp(base.g, p_color.g, m);
			result.b = Math::lerp(base.b, p_color.b, m);
			result.a = base.a + (1.0f - base.a) * m;
		} break;
		case MeshPaintBrush::BLEND_ADD: {
			result.r = base.r + p_color.r * m;
			result.g = base.g + p_color.g * m;
			result.b = base.b + p_color.b * m;
		} break;
		case MeshPaintBrush::BLEND_MULTIPLY: {
			result.r = Math::lerp(base.r, base.r * p_color.r, m);
			result.g = Math::lerp(base.g, base.g * p_color.g, m);
			result.b = Math::lerp(base.b, base.b * p_color.b, m);
		} break;
		case MeshPaintBrush::BLEND_LIGHTEN: {
			result.r = Math::lerp(base.r, MAX(base.r, p_color.r), m);
			result.g = Math::lerp(base.g, MAX(base.g, p_color.g), m);
			result.b = Math::lerp(base.b, MAX(base.b, p_color.b), m);
		} break;
		case MeshPaintBrush::BLEND_DARKEN: {
			result.r = Math::lerp(base.r, MIN(base.r, p_color.r), m);
			result.g = Math::lerp(base.g, MIN(base.g, p_color.g), m);
			result.b = Math::lerp(base.b, MIN(base.b, p_color.b), m);
		} break;
		case MeshPaintBrush::BLEND_ERASE: {
			result.a = base.a * (1.0f - mask);
		} break;
		default:
			break;
	}
	texel[0] = (uint8_t)CLAMP(Math::round(result.r * 255.0f), 0.0f, 255.0f);
	texel[1] = (uint8_t)CLAMP(Math::round(result.g * 255.0f), 0.0f, 255.0f);
	texel[2] = (uint8_t)CLAMP(Math::round(result.b * 255.0f), 0.0f, 255.0f);
	texel[3] = (uint8_t)CLAMP(Math::round(result.a * 255.0f), 0.0f, 255.0f);
}

Color MeshPainter::_sample_brush(const Ref<MeshPaintBrush> &p_brush, float p_u, float p_v) const {
	const Ref<Image> &image = p_brush->get_texture_image();
	if (image.is_null()) {
		return Color(1, 1, 1, 1);
	}
	return sample_bilinear(image, p_u, p_v, false);
}

template <typename F>
void MeshPainter::_rasterize_face(int p_face, const Size2i &p_size, float p_bleed, F &&p_texel) const {
	const Face &face = faces[p_face];
	Vector2 p[3];
	for (int c = 0; c < 3; c++) {
		const Vector2 uv = verts[face.v[c]].uv;
		p[c] = Vector2(uv.x * p_size.x, uv.y * p_size.y);
	}
	const Vector2 v0 = p[1] - p[0];
	const Vector2 v1 = p[2] - p[0];
	const real_t d = v0.x * v1.y - v1.x * v0.y;
	if (Math::abs(d) < 1e-9) {
		return;
	}
	Vector2 lo = p[0];
	Vector2 hi = p[0];
	for (int c = 1; c < 3; c++) {
		lo = lo.min(p[c]);
		hi = hi.max(p[c]);
	}
	const int x0 = MAX(0, (int)Math::floor(lo.x - p_bleed));
	const int y0 = MAX(0, (int)Math::floor(lo.y - p_bleed));
	const int x1 = MIN(p_size.x - 1, (int)Math::ceil(hi.x + p_bleed));
	const int y1 = MIN(p_size.y - 1, (int)Math::ceil(hi.y + p_bleed));
	const real_t inv_d = 1.0 / d;
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			const Vector2 q(x + 0.5, y + 0.5);
			const Vector2 v2 = q - p[0];
			real_t b1 = (v2.x * v1.y - v1.x * v2.y) * inv_d;
			real_t b2 = (v0.x * v2.y - v2.x * v0.y) * inv_d;
			real_t b0 = 1.0 - b1 - b2;
			if (b0 < 0 || b1 < 0 || b2 < 0) {
				if (p_bleed <= 0) {
					continue;
				}
				// Just outside: paint the edge's colour a little past it, so filtering and
				// mipmaps never pull in the background along UV seams.
				Vector2 closest;
				real_t best = 1e20;
				for (int e = 0; e < 3; e++) {
					const Vector2 c = Geometry2D::get_closest_point_to_segment(q, p[e], p[(e + 1) % 3]);
					const real_t dist = c.distance_squared_to(q);
					if (dist < best) {
						best = dist;
						closest = c;
					}
				}
				if (best > p_bleed * p_bleed) {
					continue;
				}
				const Vector2 cv = closest - p[0];
				b1 = CLAMP((cv.x * v1.y - v1.x * cv.y) * inv_d, (real_t)0.0, (real_t)1.0);
				b2 = CLAMP((v0.x * cv.y - cv.x * v0.y) * inv_d, (real_t)0.0, (real_t)1.0);
				if (b1 + b2 > 1.0) {
					const real_t s = b1 + b2;
					b1 /= s;
					b2 /= s;
				}
				b0 = 1.0 - b1 - b2;
			}
			p_texel(x, y, Vector3(b0, b1, b2));
		}
	}
}

Rect2i MeshPainter::paint_dab(const Ref<Image> &p_image, const Ref<MeshPaintBrush> &p_brush, const Vector3 &p_center, real_t p_radius, const Vector3 &p_eye, const Vector3 &p_up, int p_slot) {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), Rect2i());
	ERR_FAIL_COND_V_MSG(p_image->get_format() != Image::FORMAT_RGBA8, Rect2i(), "Paint images must be RGBA8.");
	ERR_FAIL_COND_V(p_brush.is_null(), Rect2i());
	ERR_FAIL_COND_V(!uvs_present, Rect2i());
	if (p_radius <= 0) {
		return Rect2i();
	}
	StrokeImage &stroke = _stroke_image(p_image);
	const Size2i size = p_image->get_size();

	Vector3 view = p_center - p_eye;
	const bool has_eye = view.length_squared() > CMP_EPSILON2;
	view = has_eye ? view.normalized() : Vector3(0, 0, -1);
	Vector3 up = p_up - view * p_up.dot(view);
	if (up.length_squared() < CMP_EPSILON2) {
		up = Math::abs(view.y) < 0.9 ? Vector3(0, 1, 0) - view * view.y : Vector3(1, 0, 0) - view * view.x;
	}
	up.normalize();
	Vector3 right = view.cross(up).normalized();
	const real_t angle = Math::deg_to_rad(p_brush->get_texture_angle());
	const Vector3 rotated_right = right * Math::cos(angle) + up * Math::sin(angle);
	const Vector3 rotated_up = up * Math::cos(angle) - right * Math::sin(angle);
	right = rotated_right;
	up = rotated_up;

	const bool textured = p_brush->get_texture_image().is_valid();
	const real_t cull_radius = textured ? p_radius * Math::SQRT2 : p_radius;
	const AABB sphere(p_center - Vector3(cull_radius, cull_radius, cull_radius), Vector3(cull_radius, cull_radius, cull_radius) * 2.0);
	const MeshPaintBrush::BlendMode mode = p_brush->get_blend_mode();
	const Color color = p_brush->get_color();
	const float strength = p_brush->get_strength();
	Rect2i dab_rect;

	for (uint32_t f = 0; f < faces.size(); f++) {
		if (p_slot >= 0 && faces[f].slot != p_slot) {
			continue;
		}
		if (!face_bounds[f].intersects(sphere)) {
			continue;
		}
		if (has_eye && !p_brush->is_painting_backfaces() && face_normals[f].dot(view) > 0) {
			continue;
		}
		if (has_eye && p_brush->has_occlusion() && !_face_visible(f, p_eye)) {
			continue;
		}
		const Face &face = faces[f];
		const Vector3 a = positions[face.v[0]];
		const Vector3 b = positions[face.v[1]];
		const Vector3 c = positions[face.v[2]];
		_rasterize_face(f, size, 1.5, [&](int x, int y, const Vector3 &bary) {
			const Vector3 point = a * bary.x + b * bary.y + c * bary.z;
			const Vector3 offset = point - p_center;
			float alpha;
			Color paint = color;
			if (textured) {
				const real_t su = offset.dot(right) / p_radius;
				const real_t sv = offset.dot(up) / p_radius;
				if (Math::abs(su) >= 1.0 || Math::abs(sv) >= 1.0) {
					return;
				}
				const Color t = _sample_brush(p_brush, (su + 1.0) * 0.5, (1.0 - sv) * 0.5);
				paint = Color(color.r * t.r, color.g * t.g, color.b * t.b, color.a);
				alpha = t.a * p_brush->falloff(MAX(Math::abs(su), Math::abs(sv)));
			} else {
				const real_t distance = offset.length();
				if (distance >= p_radius) {
					return;
				}
				alpha = p_brush->falloff(distance / p_radius);
			}
			alpha *= strength;
			if (alpha <= 0.0f) {
				return;
			}
			_blend_texel(stroke, p_image, x, y, alpha, paint, mode);
			dab_rect = merge_rect(dab_rect, Rect2i(x, y, 1, 1));
		});
	}
	stroke.dirty = merge_rect(stroke.dirty, dab_rect);
	return dab_rect;
}

Rect2i MeshPainter::paint_dab_uv(const Ref<Image> &p_image, const Ref<MeshPaintBrush> &p_brush, const Vector2 &p_uv, real_t p_radius_uv) {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), Rect2i());
	ERR_FAIL_COND_V_MSG(p_image->get_format() != Image::FORMAT_RGBA8, Rect2i(), "Paint images must be RGBA8.");
	ERR_FAIL_COND_V(p_brush.is_null(), Rect2i());
	if (p_radius_uv <= 0) {
		return Rect2i();
	}
	StrokeImage &stroke = _stroke_image(p_image);
	const Size2i size = p_image->get_size();
	const Vector2 center(p_uv.x * size.x, p_uv.y * size.y);
	const Vector2 radius(p_radius_uv * size.x, p_radius_uv * size.y);
	const bool textured = p_brush->get_texture_image().is_valid();
	const real_t reach = textured ? Math::SQRT2 : 1.0;
	const int x0 = MAX(0, (int)Math::floor(center.x - radius.x * reach));
	const int y0 = MAX(0, (int)Math::floor(center.y - radius.y * reach));
	const int x1 = MIN(size.x - 1, (int)Math::ceil(center.x + radius.x * reach));
	const int y1 = MIN(size.y - 1, (int)Math::ceil(center.y + radius.y * reach));
	const real_t angle = Math::deg_to_rad(p_brush->get_texture_angle());
	const real_t ca = Math::cos(angle);
	const real_t sa = Math::sin(angle);
	const Color color = p_brush->get_color();
	Rect2i dab_rect;
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			const real_t du = (x + 0.5 - center.x) / radius.x;
			const real_t dv = (y + 0.5 - center.y) / radius.y;
			float alpha;
			Color paint = color;
			if (textured) {
				// Brush axes: right and up on screen, turned by the brush angle.
				const real_t su = du * ca - dv * sa;
				const real_t sv = -(du * sa + dv * ca);
				if (Math::abs(su) >= 1.0 || Math::abs(sv) >= 1.0) {
					continue;
				}
				const Color t = _sample_brush(p_brush, (su + 1.0) * 0.5, (1.0 - sv) * 0.5);
				paint = Color(color.r * t.r, color.g * t.g, color.b * t.b, color.a);
				alpha = t.a * p_brush->falloff(MAX(Math::abs(su), Math::abs(sv)));
			} else {
				const real_t distance = Math::sqrt(du * du + dv * dv);
				if (distance >= 1.0) {
					continue;
				}
				alpha = p_brush->falloff(distance);
			}
			alpha *= p_brush->get_strength();
			if (alpha <= 0.0f) {
				continue;
			}
			_blend_texel(stroke, p_image, x, y, alpha, paint, p_brush->get_blend_mode());
			dab_rect = merge_rect(dab_rect, Rect2i(x, y, 1, 1));
		}
	}
	stroke.dirty = merge_rect(stroke.dirty, dab_rect);
	return dab_rect;
}

Rect2i MeshPainter::fill_faces(const Ref<Image> &p_image, const PackedInt32Array &p_faces, const Ref<MeshPaintBrush> &p_brush) {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), Rect2i());
	ERR_FAIL_COND_V_MSG(p_image->get_format() != Image::FORMAT_RGBA8, Rect2i(), "Paint images must be RGBA8.");
	ERR_FAIL_COND_V(p_brush.is_null(), Rect2i());
	ERR_FAIL_COND_V(!uvs_present, Rect2i());
	StrokeImage &stroke = _stroke_image(p_image);
	const Size2i size = p_image->get_size();
	const float alpha = p_brush->get_strength();
	MeshPaintBrush::BlendMode mode = p_brush->get_blend_mode();
	if (mode == MeshPaintBrush::BLEND_BLUR) {
		mode = MeshPaintBrush::BLEND_MIX;
	}
	const Color color = p_brush->get_color();
	Rect2i rect;
	for (const int f : p_faces) {
		ERR_CONTINUE(f < 0 || f >= (int)faces.size());
		_rasterize_face(f, size, 1.5, [&](int x, int y, const Vector3 &bary) {
			_blend_texel(stroke, p_image, x, y, alpha, color, mode);
			rect = merge_rect(rect, Rect2i(x, y, 1, 1));
		});
	}
	stroke.dirty = merge_rect(stroke.dirty, rect);
	return rect;
}

Rect2i MeshPainter::get_stroke_rect(const Ref<Image> &p_image) const {
	HashMap<Image *, StrokeImage>::ConstIterator E = stroke_images.find(p_image.ptr());
	return E ? E->value.dirty : Rect2i();
}

Ref<Image> MeshPainter::get_stroke_original(const Ref<Image> &p_image) const {
	HashMap<Image *, StrokeImage>::ConstIterator E = stroke_images.find(p_image.ptr());
	return E ? E->value.original : Ref<Image>();
}

TypedArray<Image> MeshPainter::get_stroke_images() const {
	TypedArray<Image> result;
	for (const KeyValue<Image *, StrokeImage> &E : stroke_images) {
		result.push_back(Ref<Image>(E.key));
	}
	return result;
}

void MeshPainter::end_stroke() {
	stroke_images.clear();
}

Vector2 MeshPainter::get_uv_at(int p_face, const Vector3 &p_barycentric) const {
	ERR_FAIL_INDEX_V(p_face, (int)faces.size(), Vector2());
	const Face &face = faces[p_face];
	return verts[face.v[0]].uv * p_barycentric.x + verts[face.v[1]].uv * p_barycentric.y + verts[face.v[2]].uv * p_barycentric.z;
}

Color MeshPainter::sample_image(const Ref<Image> &p_image, int p_face, const Vector3 &p_barycentric) const {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), Color());
	const Vector2 uv = get_uv_at(p_face, p_barycentric);
	const int x = CLAMP((int)(uv.x * p_image->get_width()), 0, p_image->get_width() - 1);
	const int y = CLAMP((int)(uv.y * p_image->get_height()), 0, p_image->get_height() - 1);
	return p_image->get_pixel(x, y);
}

// Material channels.

static BaseMaterial3D::TextureParam channel_param(MeshPainter::Channel p_channel) {
	switch (p_channel) {
		case MeshPainter::CHANNEL_ALBEDO:
			return BaseMaterial3D::TEXTURE_ALBEDO;
		case MeshPainter::CHANNEL_ROUGHNESS:
			return BaseMaterial3D::TEXTURE_ROUGHNESS;
		case MeshPainter::CHANNEL_METALLIC:
			return BaseMaterial3D::TEXTURE_METALLIC;
		case MeshPainter::CHANNEL_EMISSION:
			return BaseMaterial3D::TEXTURE_EMISSION;
		default:
			return BaseMaterial3D::TEXTURE_ALBEDO;
	}
}

String MeshPainter::get_channel_name(Channel p_channel) {
	switch (p_channel) {
		case CHANNEL_ALBEDO:
			return "albedo";
		case CHANNEL_ROUGHNESS:
			return "roughness";
		case CHANNEL_METALLIC:
			return "metallic";
		case CHANNEL_EMISSION:
			return "emission";
		default:
			return String();
	}
}

Ref<Texture2D> MeshPainter::get_channel_texture(const Ref<BaseMaterial3D> &p_material, Channel p_channel) {
	ERR_FAIL_COND_V(p_material.is_null(), Ref<Texture2D>());
	ERR_FAIL_INDEX_V((int)p_channel, (int)CHANNEL_MAX, Ref<Texture2D>());
	return p_material->get_texture(channel_param(p_channel));
}

Ref<Image> MeshPainter::make_channel_image(const Ref<BaseMaterial3D> &p_material, Channel p_channel, const Vector2i &p_size) {
	ERR_FAIL_COND_V(p_material.is_null(), Ref<Image>());
	ERR_FAIL_INDEX_V((int)p_channel, (int)CHANNEL_MAX, Ref<Image>());
	ERR_FAIL_COND_V(p_size.x <= 0 || p_size.y <= 0, Ref<Image>());

	// What the material shows today, as one colour per texel: texture times its value.
	Color tint;
	BaseMaterial3D::TextureChannel source_channel = BaseMaterial3D::TEXTURE_CHANNEL_RED;
	switch (p_channel) {
		case CHANNEL_ALBEDO:
			tint = p_material->get_albedo();
			break;
		case CHANNEL_ROUGHNESS: {
			const float r = p_material->get_roughness();
			tint = Color(r, r, r, 1);
			source_channel = p_material->get_roughness_texture_channel();
		} break;
		case CHANNEL_METALLIC: {
			const float m = p_material->get_metallic();
			tint = Color(m, m, m, 1);
			source_channel = p_material->get_metallic_texture_channel();
		} break;
		case CHANNEL_EMISSION:
			tint = p_material->get_feature(BaseMaterial3D::FEATURE_EMISSION) ? p_material->get_emission() : Color(0, 0, 0, 1);
			tint.a = 1.0;
			break;
		default:
			break;
	}

	Ref<Image> source;
	const Ref<Texture2D> texture = get_channel_texture(p_material, p_channel);
	if (texture.is_valid() && !(p_channel == CHANNEL_EMISSION && !p_material->get_feature(BaseMaterial3D::FEATURE_EMISSION))) {
		// The original file is lossless; the imported texture may be VRAM compressed.
		const String path = texture->get_path();
		if (!path.is_empty() && !path.contains("::")) {
			source = Image::load_from_file(path);
		}
		if (source.is_null() || source->is_empty()) {
			source = texture->get_image();
			if (source.is_valid()) {
				source = source->duplicate();
			}
		}
		if (source.is_valid() && !source->is_empty()) {
			if (source->is_compressed()) {
				source->decompress();
			}
			source->clear_mipmaps();
			source->convert(Image::FORMAT_RGBA8);
		} else {
			source.unref();
		}
	}

	const Vector3 scale = p_material->get_uv1_scale();
	const Vector3 offset = p_material->get_uv1_offset();
	const bool tiled = !scale.is_equal_approx(Vector3(1, 1, 1)) || !offset.is_zero_approx();
	// Untiled textures are painted at their own size; new or re-tiled ones at the size asked.
	const Vector2i size = source.is_valid() && !tiled ? source->get_size() : p_size;

	Ref<Image> image = Image::create_empty(size.x, size.y, false, Image::FORMAT_RGBA8);
	uint8_t *w = image->ptrw();
	for (int y = 0; y < size.y; y++) {
		for (int x = 0; x < size.x; x++) {
			Color c = tint;
			if (source.is_valid()) {
				Color t;
				if (tiled) {
					// Baked with the material's UV tiling, which the painted texture no longer uses.
					const float u = (x + 0.5f) / size.x * scale.x + offset.x;
					const float v = (y + 0.5f) / size.y * scale.y + offset.y;
					t = sample_bilinear(source, u, v, true);
				} else {
					const uint8_t *s = source->ptr() + (y * size.x + x) * 4;
					t = Color(s[0] / 255.0f, s[1] / 255.0f, s[2] / 255.0f, s[3] / 255.0f);
				}
				if (p_channel == CHANNEL_ROUGHNESS || p_channel == CHANNEL_METALLIC) {
					float value = t.r;
					switch (source_channel) {
						case BaseMaterial3D::TEXTURE_CHANNEL_GREEN:
							value = t.g;
							break;
						case BaseMaterial3D::TEXTURE_CHANNEL_BLUE:
							value = t.b;
							break;
						case BaseMaterial3D::TEXTURE_CHANNEL_ALPHA:
							value = t.a;
							break;
						case BaseMaterial3D::TEXTURE_CHANNEL_GRAYSCALE:
							value = (t.r + t.g + t.b) / 3.0f;
							break;
						default:
							break;
					}
					const float v = value * tint.r;
					c = Color(v, v, v, 1);
				} else {
					c = Color(t.r * tint.r, t.g * tint.g, t.b * tint.b, t.a * tint.a);
				}
			}
			uint8_t *d = w + (y * size.x + x) * 4;
			d[0] = (uint8_t)CLAMP(Math::round(c.r * 255.0f), 0.0f, 255.0f);
			d[1] = (uint8_t)CLAMP(Math::round(c.g * 255.0f), 0.0f, 255.0f);
			d[2] = (uint8_t)CLAMP(Math::round(c.b * 255.0f), 0.0f, 255.0f);
			d[3] = (uint8_t)CLAMP(Math::round(c.a * 255.0f), 0.0f, 255.0f);
		}
	}
	return image;
}

void MeshPainter::set_channel_texture(const Ref<BaseMaterial3D> &p_material, Channel p_channel, const Ref<Texture2D> &p_texture) {
	ERR_FAIL_COND(p_material.is_null());
	ERR_FAIL_INDEX((int)p_channel, (int)CHANNEL_MAX);
	p_material->set_texture(channel_param(p_channel), p_texture);
	if (p_texture.is_null()) {
		return;
	}
	// The painted texture holds the final value, so the material's own value becomes neutral.
	switch (p_channel) {
		case CHANNEL_ALBEDO:
			p_material->set_albedo(Color(1, 1, 1, 1));
			break;
		case CHANNEL_ROUGHNESS:
			p_material->set_roughness(1.0);
			p_material->set_roughness_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_RED);
			break;
		case CHANNEL_METALLIC:
			p_material->set_metallic(1.0);
			p_material->set_metallic_texture_channel(BaseMaterial3D::TEXTURE_CHANNEL_RED);
			break;
		case CHANNEL_EMISSION:
			p_material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
			p_material->set_emission(Color(1, 1, 1, 1));
			p_material->set_emission_operator(BaseMaterial3D::EMISSION_OP_MULTIPLY);
			break;
		default:
			break;
	}
	const Vector3 scale = p_material->get_uv1_scale();
	const Vector3 offset = p_material->get_uv1_offset();
	if (!scale.is_equal_approx(Vector3(1, 1, 1)) || !offset.is_zero_approx()) {
		// make_channel_image() baked the tiling in.
		p_material->set_uv1_scale(Vector3(1, 1, 1));
		p_material->set_uv1_offset(Vector3());
	}
}

void MeshPainter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_mesh", "mesh", "pose_mesh", "surface_overrides"), &MeshPainter::load_mesh, DEFVAL(Ref<Mesh>()), DEFVAL(TypedArray<Material>()));
	ClassDB::bind_method(D_METHOD("set_pose_mesh", "pose_mesh"), &MeshPainter::set_pose_mesh);
	ClassDB::bind_method(D_METHOD("get_loaded_mesh"), &MeshPainter::get_loaded_mesh);
	ClassDB::bind_method(D_METHOD("commit_mesh", "generate_lods"), &MeshPainter::commit_mesh, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("get_face_count"), &MeshPainter::get_face_count);
	ClassDB::bind_method(D_METHOD("has_uvs"), &MeshPainter::has_uvs);

	ClassDB::bind_method(D_METHOD("get_slot_count"), &MeshPainter::get_slot_count);
	ClassDB::bind_method(D_METHOD("add_slot", "material", "name"), &MeshPainter::add_slot, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("remove_slot", "slot"), &MeshPainter::remove_slot);
	ClassDB::bind_method(D_METHOD("set_slot_material", "slot", "material"), &MeshPainter::set_slot_material);
	ClassDB::bind_method(D_METHOD("get_slot_material", "slot"), &MeshPainter::get_slot_material);
	ClassDB::bind_method(D_METHOD("set_slot_name", "slot", "name"), &MeshPainter::set_slot_name);
	ClassDB::bind_method(D_METHOD("get_slot_name", "slot"), &MeshPainter::get_slot_name);
	ClassDB::bind_method(D_METHOD("get_slot_face_count", "slot"), &MeshPainter::get_slot_face_count);
	ClassDB::bind_method(D_METHOD("get_slot_faces", "slot"), &MeshPainter::get_slot_faces);
	ClassDB::bind_method(D_METHOD("set_faces_slot", "faces", "slot"), &MeshPainter::set_faces_slot);
	ClassDB::bind_method(D_METHOD("get_face_slot", "face"), &MeshPainter::get_face_slot);

	ClassDB::bind_method(D_METHOD("set_face_selected", "face", "selected"), &MeshPainter::set_face_selected);
	ClassDB::bind_method(D_METHOD("is_face_selected", "face"), &MeshPainter::is_face_selected);
	ClassDB::bind_method(D_METHOD("get_selected_faces"), &MeshPainter::get_selected_faces);
	ClassDB::bind_method(D_METHOD("set_selected_faces", "faces"), &MeshPainter::set_selected_faces);
	ClassDB::bind_method(D_METHOD("get_selected_face_count"), &MeshPainter::get_selected_face_count);
	ClassDB::bind_method(D_METHOD("select_all", "select"), &MeshPainter::select_all, DEFVAL(true));
	ClassDB::bind_method(D_METHOD("invert_selection"), &MeshPainter::invert_selection);
	ClassDB::bind_method(D_METHOD("grow_selection"), &MeshPainter::grow_selection);
	ClassDB::bind_method(D_METHOD("shrink_selection"), &MeshPainter::shrink_selection);
	ClassDB::bind_method(D_METHOD("select_islands_of_selection"), &MeshPainter::select_islands_of_selection);
	ClassDB::bind_method(D_METHOD("select_pieces_of_selection"), &MeshPainter::select_pieces_of_selection);

	ClassDB::bind_method(D_METHOD("get_island_count"), &MeshPainter::get_island_count);
	ClassDB::bind_method(D_METHOD("get_face_island", "face"), &MeshPainter::get_face_island);
	ClassDB::bind_method(D_METHOD("get_island_faces", "island"), &MeshPainter::get_island_faces);
	ClassDB::bind_method(D_METHOD("get_islands_of_faces", "faces"), &MeshPainter::get_islands_of_faces);
	ClassDB::bind_method(D_METHOD("get_island_uv_rect", "island"), &MeshPainter::get_island_uv_rect);
	ClassDB::bind_method(D_METHOD("get_face_piece", "face"), &MeshPainter::get_face_piece);
	ClassDB::bind_method(D_METHOD("get_piece_faces", "piece"), &MeshPainter::get_piece_faces);

	ClassDB::bind_method(D_METHOD("get_face_uv", "face", "corner"), &MeshPainter::get_face_uv);
	ClassDB::bind_method(D_METHOD("get_face_uvs"), &MeshPainter::get_face_uvs);
	ClassDB::bind_method(D_METHOD("transform_faces_uv", "faces", "transform"), &MeshPainter::transform_faces_uv);
	ClassDB::bind_method(D_METHOD("smart_unwrap", "faces", "angle_limit", "padding", "resolution"), &MeshPainter::smart_unwrap, DEFVAL(PackedInt32Array()), DEFVAL(66.0), DEFVAL(4), DEFVAL(1024));
	ClassDB::bind_method(D_METHOD("pack_islands", "padding", "resolution", "rotate", "average_scale"), &MeshPainter::pack_islands, DEFVAL(4), DEFVAL(1024), DEFVAL(true), DEFVAL(true));

	ClassDB::bind_method(D_METHOD("intersect_ray", "from", "direction"), &MeshPainter::intersect_ray);
	ClassDB::bind_method(D_METHOD("get_faces_in_sphere", "center", "radius", "eye", "front_only"), &MeshPainter::get_faces_in_sphere, DEFVAL(Vector3()), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_face_normal", "face"), &MeshPainter::get_face_normal);
	ClassDB::bind_method(D_METHOD("get_face_position", "face", "corner"), &MeshPainter::get_face_position);
	ClassDB::bind_method(D_METHOD("get_barycentric", "face", "point"), &MeshPainter::get_barycentric);
	ClassDB::bind_method(D_METHOD("get_faces_triangles", "faces", "offset"), &MeshPainter::get_faces_triangles, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("get_aabb"), &MeshPainter::get_aabb);

	ClassDB::bind_method(D_METHOD("begin_stroke"), &MeshPainter::begin_stroke);
	ClassDB::bind_method(D_METHOD("paint_dab", "image", "brush", "center", "radius", "eye", "up", "slot"), &MeshPainter::paint_dab, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("paint_dab_uv", "image", "brush", "uv", "radius"), &MeshPainter::paint_dab_uv);
	ClassDB::bind_method(D_METHOD("fill_faces", "image", "faces", "brush"), &MeshPainter::fill_faces);
	ClassDB::bind_method(D_METHOD("get_stroke_rect", "image"), &MeshPainter::get_stroke_rect);
	ClassDB::bind_method(D_METHOD("get_stroke_original", "image"), &MeshPainter::get_stroke_original);
	ClassDB::bind_method(D_METHOD("get_stroke_images"), &MeshPainter::get_stroke_images);
	ClassDB::bind_method(D_METHOD("end_stroke"), &MeshPainter::end_stroke);
	ClassDB::bind_method(D_METHOD("sample_image", "image", "face", "barycentric"), &MeshPainter::sample_image);
	ClassDB::bind_method(D_METHOD("get_uv_at", "face", "barycentric"), &MeshPainter::get_uv_at);

	ClassDB::bind_static_method("MeshPainter", D_METHOD("get_channel_texture", "material", "channel"), &MeshPainter::get_channel_texture);
	ClassDB::bind_static_method("MeshPainter", D_METHOD("make_channel_image", "material", "channel", "size"), &MeshPainter::make_channel_image);
	ClassDB::bind_static_method("MeshPainter", D_METHOD("set_channel_texture", "material", "channel", "texture"), &MeshPainter::set_channel_texture);

	BIND_ENUM_CONSTANT(CHANNEL_ALBEDO);
	BIND_ENUM_CONSTANT(CHANNEL_ROUGHNESS);
	BIND_ENUM_CONSTANT(CHANNEL_METALLIC);
	BIND_ENUM_CONSTANT(CHANNEL_EMISSION);
	BIND_ENUM_CONSTANT(CHANNEL_MAX);
}

MeshPainter::MeshPainter() {
}
