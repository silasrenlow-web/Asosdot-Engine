/**************************************************************************/
/*  editable_mesh_instance_3d.cpp                                         */
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
/* the following conditions:                                             */
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

#include "editable_mesh_instance_3d.h"

#include "csg_shape.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/resources/surface_tool.h"

#include <manifold/manifold.h>

namespace {

// What travels through the boolean with every vertex. Manifold interpolates all of it
// where a cut crosses a face, so the source mesh's own attributes survive on the parts of
// it that a shape doesn't replace.
enum EditableProperty {
	EDITABLE_POSITION_X,
	EDITABLE_POSITION_Y,
	EDITABLE_POSITION_Z,
	EDITABLE_INVERT,
	EDITABLE_SMOOTH,
	EDITABLE_UV_X,
	EDITABLE_UV_Y,
	EDITABLE_FROM_SOURCE, // 1 on the source mesh's faces, 0 on the shapes'.
	EDITABLE_NORMAL_X,
	EDITABLE_NORMAL_Y,
	EDITABLE_NORMAL_Z,
	EDITABLE_UV2_X,
	EDITABLE_UV2_Y,
	EDITABLE_COLOR_R,
	EDITABLE_COLOR_G,
	EDITABLE_COLOR_B,
	EDITABLE_COLOR_A,
	EDITABLE_PROPERTY_MAX
};

// One output surface: a surface of the source mesh, or a material of the shapes.
struct Slot {
	Ref<Material> material;
	bool from_source = false;
	bool has_normals = false;
	bool has_uv = true;
	bool has_uv2 = false;
	bool has_color = false;
	bool has_tangents = false;
};

struct OutVertex {
	Vector3 position;
	Vector3 normal;
	Vector2 uv;
	Vector2 uv2;
	Color color;
};

struct SourceTriangle {
	int slot = 0;
	double corners[3][EDITABLE_PROPERTY_MAX];
};

OutVertex _read_vertex(const double *p_properties) {
	OutVertex v;
	v.position = Vector3(p_properties[EDITABLE_POSITION_X], p_properties[EDITABLE_POSITION_Y], p_properties[EDITABLE_POSITION_Z]);
	v.uv = Vector2(p_properties[EDITABLE_UV_X], p_properties[EDITABLE_UV_Y]);
	v.normal = Vector3(p_properties[EDITABLE_NORMAL_X], p_properties[EDITABLE_NORMAL_Y], p_properties[EDITABLE_NORMAL_Z]);
	v.uv2 = Vector2(p_properties[EDITABLE_UV2_X], p_properties[EDITABLE_UV2_Y]);
	v.color = Color(p_properties[EDITABLE_COLOR_R], p_properties[EDITABLE_COLOR_G], p_properties[EDITABLE_COLOR_B], p_properties[EDITABLE_COLOR_A]);
	if (p_properties[EDITABLE_FROM_SOURCE] < 0.5) {
		v.color = Color(1, 1, 1, 1); // Faces the shapes bring have no colors of their own.
	}
	return v;
}

// Godot winds front faces clockwise, Manifold counterclockwise; CSG flips the same way.
constexpr int32_t WINDING[3] = { 0, 2, 1 };

void _begin_run(manifold::MeshGL64 &r_mesh, HashMap<uint32_t, int> &r_slot_of_id, int p_slot) {
	r_mesh.runIndex.push_back(r_mesh.triVerts.size());
	const uint32_t id = manifold::Manifold::ReserveIDs(1);
	r_mesh.runOriginalID.push_back(id);
	r_slot_of_id.insert(id, p_slot);
}

double *_add_vertex(manifold::MeshGL64 &r_mesh) {
	r_mesh.triVerts.push_back(r_mesh.vertProperties.size() / EDITABLE_PROPERTY_MAX);
	const size_t begin = r_mesh.vertProperties.size();
	r_mesh.vertProperties.resize(begin + EDITABLE_PROPERTY_MAX, 0.0);
	return &r_mesh.vertProperties[begin];
}

int _find(LocalVector<int> &r_parent, int p_x) {
	while (r_parent[p_x] != p_x) {
		r_parent[p_x] = r_parent[r_parent[p_x]];
		p_x = r_parent[p_x];
	}
	return p_x;
}

manifold::Manifold _finish(manifold::MeshGL64 &r_mesh) {
	r_mesh.runIndex.push_back(r_mesh.triVerts.size());
	r_mesh.tolerance = 2 * FLT_EPSILON;
	r_mesh.Merge();
	return manifold::Manifold(r_mesh);
}

} // namespace

void EditableMeshInstance3D::_operand_changed() {
	_queue_rebuild();
}

bool EditableMeshInstance3D::_can_rebuild() const {
	return Engine::get_singleton()->is_editor_hint() || update_at_runtime;
}

void EditableMeshInstance3D::_queue_rebuild() {
	if (rebuild_queued || !_can_rebuild()) {
		return;
	}
	rebuild_queued = true;
	callable_mp(this, &EditableMeshInstance3D::_rebuild).call_deferred();
}

Vector<CSGShape3D *> EditableMeshInstance3D::_get_operands() const {
	Vector<CSGShape3D *> operands;
	for (int i = 0; i < get_child_count(); i++) {
		CSGShape3D *shape = Object::cast_to<CSGShape3D>(get_child(i));
		if (shape && shape->is_visible()) {
			operands.push_back(shape);
		}
	}
	return operands;
}

bool EditableMeshInstance3D::is_source_closed() const {
	return !source_not_closed;
}

bool EditableMeshInstance3D::has_edits() const {
	return !_get_operands().is_empty();
}

void EditableMeshInstance3D::rebuild() {
	_rebuild();
}

void EditableMeshInstance3D::_rebuild() {
	rebuild_queued = false;
	const bool was_not_closed = source_not_closed;
	const bool was_dropping = dropped_animation_data;
	const bool was_partly_open = source_partly_open;
	source_not_closed = false;
	source_partly_open = false;
	dropped_animation_data = false;

	const Vector<CSGShape3D *> operands = _get_operands();
	operand_count = operands.size();

	if (source_mesh.is_null() || operands.is_empty()) {
		// Nothing to cut: the mesh is the source itself, with everything it has.
		MeshInstance3D::set_mesh(source_mesh);
		if (was_not_closed || was_dropping) {
			update_configuration_warnings();
		}
		return;
	}

	// The source mesh's triangles with everything they carry, in Manifold's winding.
	LocalVector<Slot> slots;
	HashMap<uint32_t, int> slot_of_id;
	LocalVector<SourceTriangle> triangles;
	int main_slot = -1;
	int main_triangles = 0;
	for (int surface = 0; surface < source_mesh->get_surface_count(); surface++) {
		if (source_mesh->surface_get_primitive_type(surface) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		const Array arrays = source_mesh->surface_get_arrays(surface);
		if (arrays.is_empty()) {
			continue;
		}
		const PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
		if (vertices.is_empty()) {
			continue;
		}
		const PackedVector3Array normals = arrays[Mesh::ARRAY_NORMAL];
		const PackedVector2Array uvs = arrays[Mesh::ARRAY_TEX_UV];
		const PackedVector2Array uv2s = arrays[Mesh::ARRAY_TEX_UV2];
		const PackedColorArray colors = arrays[Mesh::ARRAY_COLOR];
		const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
		const int vertex_count = vertices.size();
		const bool has_normals = normals.size() == vertex_count;
		const bool has_uv = uvs.size() == vertex_count;
		const bool has_uv2 = uv2s.size() == vertex_count;
		const bool has_color = colors.size() == vertex_count;

		if (source_mesh->get_blend_shape_count() > 0 || (source_mesh->surface_get_format(surface) & Mesh::ARRAY_FORMAT_BONES)) {
			dropped_animation_data = true;
		}

		Slot slot;
		slot.material = source_mesh->surface_get_material(surface);
		slot.from_source = true;
		slot.has_normals = has_normals;
		slot.has_uv = has_uv;
		slot.has_uv2 = has_uv2;
		slot.has_color = has_color;
		slot.has_tangents = source_mesh->surface_get_format(surface) & Mesh::ARRAY_FORMAT_TANGENT;
		const int slot_index = slots.size();
		slots.push_back(slot);

		const int triangle_count = indices.is_empty() ? vertex_count / 3 : indices.size() / 3;
		if (triangle_count > main_triangles) {
			main_triangles = triangle_count;
			main_slot = slot_index;
		}
		for (int t = 0; t < triangle_count; t++) {
			int corner[3];
			for (int k = 0; k < 3; k++) {
				corner[k] = indices.is_empty() ? t * 3 + k : indices[t * 3 + k];
			}
			// Zero-area triangles are kept: exported meshes close T-junctions with slivers whose
			// corners lie on one line, and dropping them would open the mesh. Manifold removes
			// them itself once the topology is closed.
			SourceTriangle triangle;
			triangle.slot = slot_index;
			memset(triangle.corners, 0, sizeof(triangle.corners));
			for (int k = 0; k < 3; k++) {
				const int index = corner[WINDING[k]];
				double *vertex = triangle.corners[k];
				vertex[EDITABLE_POSITION_X] = vertices[index].x;
				vertex[EDITABLE_POSITION_Y] = vertices[index].y;
				vertex[EDITABLE_POSITION_Z] = vertices[index].z;
				vertex[EDITABLE_SMOOTH] = 1.0;
				vertex[EDITABLE_FROM_SOURCE] = 1.0;
				if (has_uv) {
					vertex[EDITABLE_UV_X] = uvs[index].x;
					vertex[EDITABLE_UV_Y] = uvs[index].y;
				}
				if (has_normals) {
					vertex[EDITABLE_NORMAL_X] = normals[index].x;
					vertex[EDITABLE_NORMAL_Y] = normals[index].y;
					vertex[EDITABLE_NORMAL_Z] = normals[index].z;
				}
				if (has_uv2) {
					vertex[EDITABLE_UV2_X] = uv2s[index].x;
					vertex[EDITABLE_UV2_Y] = uv2s[index].y;
				}
				if (has_color) {
					vertex[EDITABLE_COLOR_R] = colors[index].r;
					vertex[EDITABLE_COLOR_G] = colors[index].g;
					vertex[EDITABLE_COLOR_B] = colors[index].b;
					vertex[EDITABLE_COLOR_A] = colors[index].a;
				}
			}
			triangles.push_back(triangle);
		}
	}
	if (slots.is_empty() || triangles.is_empty()) {
		MeshInstance3D::set_mesh(source_mesh);
		return;
	}
	const int source_slot_count = slots.size();

	// Real level meshes are often several closed blocks in one mesh that touch along edges,
	// like wall pieces meeting at a corner: such an edge has four or more triangles, which
	// Manifold refuses as a whole. Split the mesh into its blocks instead. Triangles that
	// share an ordinary edge belong together; around a shared edge, faces are paired the
	// way the blocks enclose their volumes: going round the edge, a face whose outward
	// normal points back and the next face whose normal points ahead bound one block.
	// Every closed block becomes a Manifold, and they are united into the source solid.
	// Blocks that aren't closed are drawn as they are, uncut, instead of failing the mesh.
	LocalVector<int> block_of_triangle;
	LocalVector<int> corner_parent;
	{
		const int triangle_count = triangles.size();
		HashMap<Vector3i, int> vertex_ids;
		LocalVector<Vector3> positions;
		LocalVector<int> corner_vertex;
		corner_vertex.resize(triangle_count * 3);
		for (int t = 0; t < triangle_count; t++) {
			for (int k = 0; k < 3; k++) {
				const double *p = triangles[t].corners[k];
				const Vector3 position(p[EDITABLE_POSITION_X], p[EDITABLE_POSITION_Y], p[EDITABLE_POSITION_Z]);
				// Welded to a millimeter: exported level meshes often have the "same" corner a
				// fraction of a millimeter apart, which would leave the edge open.
				const Vector3i key(Math::round(position.x * 1000.0), Math::round(position.y * 1000.0), Math::round(position.z * 1000.0));
				const int *existing = vertex_ids.getptr(key);
				if (existing) {
					corner_vertex[t * 3 + k] = *existing;
				} else {
					corner_vertex[t * 3 + k] = positions.size();
					vertex_ids.insert(key, positions.size());
					positions.push_back(position);
				}
			}
		}
		// Manifold must see welded corners at exactly the same place.
		for (int t = 0; t < triangle_count; t++) {
			for (int k = 0; k < 3; k++) {
				const Vector3 &welded = positions[corner_vertex[t * 3 + k]];
				triangles[t].corners[k][EDITABLE_POSITION_X] = welded.x;
				triangles[t].corners[k][EDITABLE_POSITION_Y] = welded.y;
				triangles[t].corners[k][EDITABLE_POSITION_Z] = welded.z;
			}
		}
		// Triangles left out of the blocks: 1 = drawn uncut, 2 = not drawn at all.
		LocalVector<uint8_t> skipped;
		skipped.resize(triangle_count);
		{
			HashMap<Vector3i, int> first_with_corners;
			for (int t = 0; t < triangle_count; t++) {
				skipped[t] = 0;
				int ids[3] = { corner_vertex[t * 3], corner_vertex[t * 3 + 1], corner_vertex[t * 3 + 2] };
				if (ids[0] == ids[1] || ids[1] == ids[2] || ids[0] == ids[2]) {
					skipped[t] = 2; // A sliver the weld collapsed.
					continue;
				}
				// The same corners in the same turning order is the same face given twice: keep
				// one. In the opposite order it is a sheet without volume: drop both.
				const int start = ids[0] < ids[1] ? (ids[0] < ids[2] ? 0 : 2) : (ids[1] < ids[2] ? 1 : 2);
				const Vector3i turning(ids[start], ids[(start + 1) % 3], ids[(start + 2) % 3]);
				int sorted[3] = { ids[0], ids[1], ids[2] };
				SortArray<int> sorter;
				sorter.sort(sorted, 3);
				const Vector3i key(sorted[0], sorted[1], sorted[2]);
				const int *first = first_with_corners.getptr(key);
				if (first) {
					const int f = *first;
					int first_ids[3] = { corner_vertex[f * 3], corner_vertex[f * 3 + 1], corner_vertex[f * 3 + 2] };
					const int first_start = first_ids[0] < first_ids[1] ? (first_ids[0] < first_ids[2] ? 0 : 2) : (first_ids[1] < first_ids[2] ? 1 : 2);
					const Vector3i first_turning(first_ids[first_start], first_ids[(first_start + 1) % 3], first_ids[(first_start + 2) % 3]);
					if (first_turning == turning) {
						skipped[t] = 2; // The same face again.
					} else {
						skipped[t] = 1; // A sheet: both sides stay as they are.
						skipped[f] = 1;
					}
				} else {
					first_with_corners.insert(key, t);
				}
			}
		}
		LocalVector<int> parent;
		parent.resize(triangle_count);
		for (int t = 0; t < triangle_count; t++) {
			parent[t] = t;
		}
		auto find = [&](int p_x) {
			while (parent[p_x] != p_x) {
				parent[p_x] = parent[parent[p_x]];
				p_x = parent[p_x];
			}
			return p_x;
		};
		auto unite = [&](int p_a, int p_b) {
			const int a = find(p_a);
			const int b = find(p_b);
			if (a != b) {
				parent[a] = b;
			}
		};
		// Corners are joined the same way, across every edge that pairs two triangles: a
		// group of joined corners is one vertex for Manifold. Where a block touches itself
		// along an edge, the two sides of that edge then stay separate vertices.
		corner_parent.resize(triangle_count * 3);
		for (int c = 0; c < triangle_count * 3; c++) {
			corner_parent[c] = c;
		}
		auto unite_corners = [&](int p_a, int p_b) {
			const int a = _find(corner_parent, p_a);
			const int b = _find(corner_parent, p_b);
			if (a != b) {
				corner_parent[a] = b;
			}
		};
		struct EdgeUse {
			int triangle = 0;
			int opposite = 0; // The triangle's third vertex.
			int corner_low = 0; // The triangle's corner at the edge's lower vertex id.
			int corner_high = 0;
		};
		auto pair = [&](const EdgeUse &p_a, const EdgeUse &p_b) {
			unite(p_a.triangle, p_b.triangle);
			unite_corners(p_a.corner_low, p_b.corner_low);
			unite_corners(p_a.corner_high, p_b.corner_high);
		};
		HashMap<uint64_t, LocalVector<EdgeUse>> edges;
		for (int t = 0; t < triangle_count; t++) {
			if (skipped[t]) {
				continue;
			}
			for (int k = 0; k < 3; k++) {
				const int a = corner_vertex[t * 3 + k];
				const int b = corner_vertex[t * 3 + (k + 1) % 3];
				if (a == b) {
					continue;
				}
				const uint64_t key = ((uint64_t)MIN(a, b) << 32) | (uint64_t)MAX(a, b);
				const int corner_a = t * 3 + k;
				const int corner_b = t * 3 + (k + 1) % 3;
				edges[key].push_back({ t, corner_vertex[t * 3 + (k + 2) % 3], a < b ? corner_a : corner_b, a < b ? corner_b : corner_a });
			}
		}
		struct Wing {
			real_t angle = 0;
			int use = 0;
			bool normal_ahead = false;
			bool operator<(const Wing &p_other) const { return angle < p_other.angle; }
		};
		for (const KeyValue<uint64_t, LocalVector<EdgeUse>> &E : edges) {
			const LocalVector<EdgeUse> &uses = E.value;
			if (uses.size() == 2) {
				pair(uses[0], uses[1]);
				continue;
			}
			if (uses.size() < 2) {
				continue;
			}
			const Vector3 from = positions[E.key >> 32];
			const Vector3 axis = (positions[E.key & 0xffffffff] - from).normalized();
			const Vector3 u = axis.get_any_perpendicular();
			const Vector3 v = axis.cross(u);
			LocalVector<Wing> wings;
			for (uint32_t use_index = 0; use_index < uses.size(); use_index++) {
				const EdgeUse &use = uses[use_index];
				const Vector3 wing = positions[use.opposite] - from;
				Wing w;
				w.angle = Math::atan2(wing.dot(v), wing.dot(u));
				w.use = use_index;
				const Vector3 p0 = positions[corner_vertex[use.triangle * 3 + 0]];
				const Vector3 p1 = positions[corner_vertex[use.triangle * 3 + 1]];
				const Vector3 p2 = positions[corner_vertex[use.triangle * 3 + 2]];
				const Vector3 outward = (p1 - p0).cross(p2 - p0); // Manifold's winding: counterclockwise from outside.
				const Vector3 ahead = v * Math::cos(w.angle) - u * Math::sin(w.angle);
				w.normal_ahead = outward.dot(ahead) > 0;
				wings.push_back(w);
			}
			wings.sort();
			for (uint32_t i = 0; i < wings.size(); i++) {
				const Wing &here = wings[i];
				const Wing &next = wings[(i + 1) % wings.size()];
				if (!here.normal_ahead && next.normal_ahead) {
					pair(uses[here.use], uses[next.use]); // The wedge between them is inside one block.
				}
			}
		}
		block_of_triangle.resize(triangle_count);
		for (int t = 0; t < triangle_count; t++) {
			block_of_triangle[t] = skipped[t] == 2 ? -2 : (skipped[t] == 1 ? -1 : find(t));
		}
	}

	HashMap<int, LocalVector<int>> triangles_of_block;
	std::vector<manifold::Manifold> blocks;
	LocalVector<int> uncut; // Triangles of blocks that aren't closed.
	for (uint32_t t = 0; t < triangles.size(); t++) {
		if (block_of_triangle[t] == -1) {
			uncut.push_back(t);
		} else if (block_of_triangle[t] >= 0) {
			triangles_of_block[block_of_triangle[t]].push_back(t);
		}
	}
	for (const KeyValue<int, LocalVector<int>> &E : triangles_of_block) {
		manifold::MeshGL64 block;
		block.numProp = EDITABLE_PROPERTY_MAX;
		HashMap<int, uint64_t> row_of_corner;
		for (int slot_index = 0; slot_index < source_slot_count; slot_index++) {
			bool run_started = false;
			for (int t : E.value) {
				if (triangles[t].slot != slot_index) {
					continue;
				}
				if (!run_started) {
					_begin_run(block, slot_of_id, slot_index);
					run_started = true;
				}
				for (int k = 0; k < 3; k++) {
					row_of_corner.insert(t * 3 + k, block.vertProperties.size() / EDITABLE_PROPERTY_MAX);
					memcpy(_add_vertex(block), triangles[t].corners[k], sizeof(double) * EDITABLE_PROPERTY_MAX);
				}
			}
		}
		// Joined corners are one vertex; the rest stay apart even where they coincide.
		for (const KeyValue<int, uint64_t> &C : row_of_corner) {
			const int representative = _find(corner_parent, C.key);
			const uint64_t *to = row_of_corner.getptr(representative);
			if (representative != C.key && to) {
				block.mergeFromVert.push_back(C.value);
				block.mergeToVert.push_back(*to);
			}
		}
		block.runIndex.push_back(block.triVerts.size());
		block.tolerance = 2 * FLT_EPSILON;
		manifold::Manifold solid(block);
		if (solid.Status() == manifold::Manifold::Error::NoError && !solid.IsEmpty()) {
			blocks.push_back(solid);
		} else {
			print_verbose(vformat("EditableMeshInstance3D: a block of %d triangles is not closed (Manifold status %d).", (int)E.value.size(), (int)solid.Status()));
			for (int t : E.value) {
				uncut.push_back(t);
			}
		}
	}
	if (blocks.empty()) {
		// The boolean needs closed blocks, ones that enclose a volume. Show it as it is.
		source_not_closed = true;
		MeshInstance3D::set_mesh(source_mesh);
		update_configuration_warnings();
		return;
	}
	source_partly_open = !uncut.is_empty();
	manifold::Manifold result = blocks.size() == 1 ? blocks[0] : manifold::Manifold::BatchBoolean(blocks, manifold::OpType::Add);

	// The shapes, in child order, the way CSG combines its children: runs of the same
	// operation are batched, and the faces they bring take the shape's own material, or
	// the material of the source's largest surface when the shape has none.
	HashMap<uint64_t, int> slot_of_material; // By material instance id, 0 for none.
	manifold::OpType current_operation = manifold::OpType::Add;
	std::vector<manifold::Manifold> batch;
	batch.push_back(result);
	for (CSGShape3D *shape : operands) {
		CSGBrush *brush = shape->_get_brush();
		if (!brush || brush->faces.is_empty()) {
			continue;
		}
		CSGBrush transformed;
		transformed.copy_from(*brush, shape->get_transform());

		manifold::MeshGL64 operand;
		operand.numProp = EDITABLE_PROPERTY_MAX;
		HashMap<int, Vector<int>> faces_by_material;
		for (int i = 0; i < transformed.faces.size(); i++) {
			faces_by_material[transformed.faces[i].material].push_back(i);
		}
		for (const KeyValue<int, Vector<int>> &E : faces_by_material) {
			Ref<Material> material;
			if (E.key >= 0 && E.key < transformed.materials.size()) {
				material = transformed.materials[E.key];
			}
			if (material.is_null() && main_slot >= 0) {
				material = slots[main_slot].material;
			}
			const uint64_t material_key = material.is_valid() ? (uint64_t)material->get_instance_id() : 0;
			if (!slot_of_material.has(material_key)) {
				// Faces with the material of one of the source's surfaces join that surface, so
				// the surfaces, and the node's surface material overrides, stay as they were.
				for (int i = 0; i < source_slot_count; i++) {
					if (slots[i].material == material) {
						slot_of_material.insert(material_key, i);
						break;
					}
				}
			}
			int *existing = slot_of_material.getptr(material_key);
			int slot_index;
			if (existing) {
				slot_index = *existing;
			} else {
				Slot slot;
				slot.material = material;
				slot_index = slots.size();
				slots.push_back(slot);
				slot_of_material.insert(material_key, slot_index);
			}
			_begin_run(operand, slot_of_id, slot_index);
			for (int face_index : E.value) {
				const CSGBrush::Face &face = transformed.faces[face_index];
				for (int k = 0; k < 3; k++) {
					const int i = WINDING[k];
					double *vertex = _add_vertex(operand);
					vertex[EDITABLE_POSITION_X] = face.vertices[i].x;
					vertex[EDITABLE_POSITION_Y] = face.vertices[i].y;
					vertex[EDITABLE_POSITION_Z] = face.vertices[i].z;
					vertex[EDITABLE_UV_X] = face.uvs[i].x;
					vertex[EDITABLE_UV_Y] = face.uvs[i].y;
					vertex[EDITABLE_SMOOTH] = face.smooth ? 1.0 : 0.0;
					vertex[EDITABLE_INVERT] = face.invert ? 1.0 : 0.0;
				}
			}
		}
		manifold::Manifold operand_manifold = _finish(operand);
		if (operand_manifold.Status() != manifold::Manifold::Error::NoError) {
			continue;
		}
		manifold::OpType operation = manifold::OpType::Add;
		if (shape->get_operation() == CSGShape3D::OPERATION_SUBTRACTION) {
			operation = manifold::OpType::Subtract;
		} else if (shape->get_operation() == CSGShape3D::OPERATION_INTERSECTION) {
			operation = manifold::OpType::Intersect;
		}
		if (operation != current_operation && batch.size() > 1) {
			manifold::Manifold combined = manifold::Manifold::BatchBoolean(batch, current_operation);
			batch.clear();
			batch.push_back(combined);
		}
		current_operation = operation;
		batch.push_back(operand_manifold);
	}
	if (batch.size() > 1) {
		result = manifold::Manifold::BatchBoolean(batch, current_operation);
	}

	// Back to triangles per slot.
	const manifold::MeshGL64 out = result.GetMeshGL64();
	const int num_prop = out.numProp;
	struct OutFace {
		OutVertex vertices[3];
		bool from_source = false;
		bool smooth = false;
		bool invert = false;
	};
	LocalVector<LocalVector<OutFace>> faces_of_slot;
	faces_of_slot.resize(slots.size());
	HashMap<Vector3, Vector3> smooth_normals; // Shapes' smooth faces share normals by position.
	// Blocks of the source that aren't closed are drawn as they were, uncut.
	for (int t : uncut) {
		OutFace face;
		face.from_source = true;
		for (int k = 0; k < 3; k++) {
			face.vertices[k] = _read_vertex(triangles[t].corners[WINDING[k]]);
		}
		faces_of_slot[triangles[t].slot].push_back(face);
	}
	for (size_t run = 0; run + 1 < out.runIndex.size(); run++) {
		int slot_index = -1;
		if (run < out.runOriginalID.size()) {
			const int *found = slot_of_id.getptr(out.runOriginalID[run]);
			if (found) {
				slot_index = *found;
			}
		}
		if (slot_index < 0) {
			continue;
		}
		for (size_t corner = out.runIndex[run]; corner + 2 < out.runIndex[run + 1]; corner += 3) {
			OutFace face;
			for (int k = 0; k < 3; k++) {
				const double *p = &out.vertProperties[out.triVerts[corner + WINDING[k]] * num_prop];
				face.vertices[k] = _read_vertex(p);
				if (k == 0) {
					face.from_source = p[EDITABLE_FROM_SOURCE] > 0.5;
					face.smooth = p[EDITABLE_SMOOTH] > 0.5;
					face.invert = p[EDITABLE_INVERT] > 0.5;
				}
			}
			if (!face.from_source && face.smooth) {
				const Vector3 plane_normal = Plane(face.vertices[0].position, face.vertices[1].position, face.vertices[2].position).normal;
				for (int k = 0; k < 3; k++) {
					Vector3 *sum = smooth_normals.getptr(face.vertices[k].position);
					if (sum) {
						*sum += plane_normal;
					} else {
						smooth_normals.insert(face.vertices[k].position, plane_normal);
					}
				}
			}
			faces_of_slot[slot_index].push_back(face);
		}
	}

	Ref<ArrayMesh> edited;
	edited.instantiate();
	for (uint32_t slot_index = 0; slot_index < slots.size(); slot_index++) {
		const Slot &slot = slots[slot_index];
		const LocalVector<OutFace> &faces = faces_of_slot[slot_index];
		if (faces.is_empty()) {
			continue;
		}
		Ref<SurfaceTool> st;
		st.instantiate();
		st->begin(Mesh::PRIMITIVE_TRIANGLES);
		for (const OutFace &face : faces) {
			int order[3] = { 0, 1, 2 };
			if (face.invert) {
				SWAP(order[1], order[2]);
			}
			const Vector3 plane_normal = Plane(face.vertices[0].position, face.vertices[1].position, face.vertices[2].position).normal;
			for (int k = 0; k < 3; k++) {
				const OutVertex &v = face.vertices[order[k]];
				Vector3 normal = plane_normal;
				if (face.from_source && slot.has_normals && v.normal.length_squared() > CMP_EPSILON2) {
					normal = v.normal.normalized();
				} else if (!face.from_source && face.smooth) {
					const Vector3 *sum = smooth_normals.getptr(v.position);
					if (sum && sum->length_squared() > CMP_EPSILON2) {
						normal = sum->normalized();
					}
				}
				if (face.invert) {
					normal = -normal;
				}
				st->set_normal(normal);
				if (slot.has_uv) {
					st->set_uv(v.uv);
				}
				if (slot.has_uv2) {
					st->set_uv2(v.uv2);
				}
				if (slot.has_color) {
					st->set_color(v.color);
				}
				st->add_vertex(v.position);
			}
		}
		st->index();
		if (slot.has_uv && (slot.has_tangents || !slot.from_source)) {
			st->generate_tangents();
		}
		st->set_material(slot.material);
		st->commit(edited);
	}
	MeshInstance3D::set_mesh(edited);
	if (was_not_closed != source_not_closed || was_dropping != dropped_animation_data || was_partly_open != source_partly_open) {
		update_configuration_warnings();
	}
}

// Makes the edited mesh the new source and removes the shapes that made it.
void EditableMeshInstance3D::apply_edits() {
	if (rebuild_queued || !_can_rebuild()) {
		_rebuild(); // Make sure the mesh shows the shapes as they are now.
	}
	const Ref<Mesh> edited = get_mesh();
	for (CSGShape3D *shape : _get_operands()) {
		remove_child(shape);
		shape->queue_free();
	}
	set_source_mesh(edited);
	_rebuild();
	notify_property_list_changed();
}

void EditableMeshInstance3D::set_source_mesh(const Ref<Mesh> &p_mesh) {
	if (source_mesh == p_mesh) {
		return;
	}
	if (source_mesh.is_valid()) {
		source_mesh->disconnect_changed(callable_mp(this, &EditableMeshInstance3D::_queue_rebuild));
	}
	source_mesh = p_mesh;
	if (source_mesh.is_valid()) {
		source_mesh->connect_changed(callable_mp(this, &EditableMeshInstance3D::_queue_rebuild));
	}
	if (_can_rebuild()) {
		_queue_rebuild();
	} else if (!has_edits()) {
		MeshInstance3D::set_mesh(source_mesh);
	}
}

Ref<Mesh> EditableMeshInstance3D::get_source_mesh() const {
	return source_mesh;
}

void EditableMeshInstance3D::set_update_at_runtime(bool p_enable) {
	update_at_runtime = p_enable;
}

bool EditableMeshInstance3D::is_updating_at_runtime() const {
	return update_at_runtime;
}

void EditableMeshInstance3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			// A MeshInstance3D changed to this type brings its mesh along: that is what gets edited.
			if (source_mesh.is_null() && get_mesh().is_valid()) {
				set_source_mesh(get_mesh());
			}
			_queue_rebuild();
		} break;
		case NOTIFICATION_CHILD_ORDER_CHANGED: {
			_queue_rebuild();
		} break;
	}
}

void EditableMeshInstance3D::_validate_property(PropertyInfo &p_property) const {
	// The mesh is the result. Edit the source mesh, or the shapes.
	if (p_property.name == "mesh") {
		p_property.usage |= PROPERTY_USAGE_READ_ONLY;
	}
}

PackedStringArray EditableMeshInstance3D::get_configuration_warnings() const {
	PackedStringArray warnings = MeshInstance3D::get_configuration_warnings();
	if (source_mesh.is_null()) {
		warnings.push_back(RTR("Set a source mesh to edit, or change a MeshInstance3D with a mesh to this type."));
	}
	if (source_not_closed) {
		warnings.push_back(RTR("The source mesh can't be cut: CSG shapes need a closed mesh, one that encloses a volume without holes or loose edges. It is shown unchanged."));
	} else if (source_partly_open) {
		warnings.push_back(RTR("Some parts of the source mesh aren't closed, so shapes don't cut them; they are shown unchanged. The closed parts are cut as usual."));
	}
	if (dropped_animation_data) {
		warnings.push_back(RTR("The source mesh has blend shapes or skinning, which the edited mesh doesn't keep."));
	}
	return warnings;
}

void EditableMeshInstance3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source_mesh", "mesh"), &EditableMeshInstance3D::set_source_mesh);
	ClassDB::bind_method(D_METHOD("get_source_mesh"), &EditableMeshInstance3D::get_source_mesh);
	ClassDB::bind_method(D_METHOD("set_update_at_runtime", "enable"), &EditableMeshInstance3D::set_update_at_runtime);
	ClassDB::bind_method(D_METHOD("is_updating_at_runtime"), &EditableMeshInstance3D::is_updating_at_runtime);
	ClassDB::bind_method(D_METHOD("rebuild"), &EditableMeshInstance3D::rebuild);
	ClassDB::bind_method(D_METHOD("has_edits"), &EditableMeshInstance3D::has_edits);
	ClassDB::bind_method(D_METHOD("is_source_closed"), &EditableMeshInstance3D::is_source_closed);
	ClassDB::bind_method(D_METHOD("apply_edits"), &EditableMeshInstance3D::apply_edits);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "source_mesh", PROPERTY_HINT_RESOURCE_TYPE, Mesh::get_class_static()), "set_source_mesh", "get_source_mesh");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "update_at_runtime"), "set_update_at_runtime", "is_updating_at_runtime");
}

EditableMeshInstance3D::EditableMeshInstance3D() {
}
