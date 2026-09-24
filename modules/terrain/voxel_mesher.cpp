/**************************************************************************/
/*  voxel_mesher.cpp                                                      */
/**************************************************************************/
/*  Asosdot native terrain and foliage.                                   */
/**************************************************************************/

#include "voxel_mesher.h"

#include "core/templates/local_vector.h"

namespace {

// Corner c of a cell sits at (c & 1, (c >> 1) & 1, (c >> 2) & 1).
_FORCE_INLINE_ int corner_x(int p_corner) {
	return p_corner & 1;
}
_FORCE_INLINE_ int corner_y(int p_corner) {
	return (p_corner >> 1) & 1;
}
_FORCE_INLINE_ int corner_z(int p_corner) {
	return (p_corner >> 2) & 1;
}

struct Table {
	// The two corners of each of the twelve edges, the one nearer the origin
	// first, and the axis the edge runs along.
	int edge_corners[12][2];
	int edge_axis[12];
	// Edges cut by each triangle, three a triangle, ended by -1. A cell cuts
	// at most twelve edges, which fan into at most ten triangles.
	int8_t triangles[256][31];
	String error;
};

Table make_table() {
	Table t;
	int edge_of[8][8];
	for (int i = 0; i < 8; i++) {
		for (int j = 0; j < 8; j++) {
			edge_of[i][j] = -1;
		}
	}
	int e = 0;
	for (int a = 0; a < 3; a++) {
		const int b = a == 0 ? 1 : 0;
		const int c = a == 2 ? 1 : 2;
		for (int k = 0; k < 4; k++) {
			const int c0 = ((k & 1) << b) | (((k >> 1) & 1) << c);
			const int c1 = c0 | (1 << a);
			t.edge_corners[e][0] = c0;
			t.edge_corners[e][1] = c1;
			t.edge_axis[e] = a;
			edge_of[c0][c1] = e;
			edge_of[c1][c0] = e;
			e++;
		}
	}

	for (int mask = 0; mask < 256; mask++) {
		// On each face, walk the corners counterclockwise as seen from
		// outside the cell. Every run of inside corners is cut off by one
		// segment, from the edge where the run ends back to the edge where it
		// began, which keeps the inside on the segment's left. A face with two
		// opposite corners inside has two runs and so two cuts, keeping those
		// corners apart, and the cell on the far side of the face makes the
		// same cuts.
		int next[12];
		int incoming[12];
		for (int i = 0; i < 12; i++) {
			next[i] = -1;
			incoming[i] = 0;
		}
		// The edges of each face with opposite corners inside, as bits.
		uint16_t ambiguous[6];
		int ambiguous_count = 0;
		for (int a = 0; a < 3; a++) {
			const int u = a == 0 ? 1 : 0;
			const int v = a == 2 ? 1 : 2;
			for (int s = 0; s < 2; s++) {
				const int base = s << a;
				int q[4] = { base, base | (1 << u), base | (1 << u) | (1 << v), base | (1 << v) };
				// That order turns about +a for (x, y) and (y, z) and about -a
				// for (x, z); flip it where that does not face outward.
				const int turn = a == 1 ? -1 : 1;
				const int outward = s ? 1 : -1;
				if (turn * outward < 0) {
					SWAP(q[1], q[3]);
				}
				bool in[4];
				for (int i = 0; i < 4; i++) {
					in[i] = (mask >> q[i]) & 1;
				}
				if (in[0] == in[2] && in[1] == in[3] && in[0] != in[1]) {
					uint16_t bits = 0;
					for (int i = 0; i < 4; i++) {
						bits |= 1 << edge_of[q[i]][q[(i + 1) % 4]];
					}
					ambiguous[ambiguous_count++] = bits;
				}
				for (int i = 0; i < 4; i++) {
					if (!in[i] || in[(i + 3) % 4]) {
						continue;
					}
					int j = i;
					while (in[(j + 1) % 4]) {
						j = (j + 1) % 4;
					}
					const int e_in = edge_of[q[(i + 3) % 4]][q[i]];
					const int e_out = edge_of[q[j]][q[(j + 1) % 4]];
					if (next[e_out] >= 0 && t.error.is_empty()) {
						t.error = vformat("case %d: edge %d leaves twice", mask, e_out);
					}
					next[e_out] = e_in;
					incoming[e_in]++;
				}
			}
		}

		for (int i = 0; i < 12; i++) {
			const bool cut = ((mask >> t.edge_corners[i][0]) & 1) != ((mask >> t.edge_corners[i][1]) & 1);
			if (t.error.is_empty() && cut != (next[i] >= 0 || incoming[i] > 0)) {
				t.error = vformat("case %d: edge %d cut=%d but used=%d", mask, i, cut, next[i] >= 0);
			}
			if (t.error.is_empty() && cut && (next[i] < 0 || incoming[i] != 1)) {
				t.error = vformat("case %d: edge %d has %d in and %d out", mask, i, incoming[i], next[i] >= 0);
			}
		}

		// Join the segments into loops around the cell and fan each one.
		bool used[12] = {};
		int n_out = 0;
		for (int start = 0; start < 12; start++) {
			if (next[start] < 0 || used[start]) {
				continue;
			}
			int loop[12];
			int n = 0;
			int cur = start;
			while (cur >= 0 && !used[cur] && n < 12) {
				used[cur] = true;
				loop[n++] = cur;
				cur = next[cur];
			}
			if (cur != start && t.error.is_empty()) {
				t.error = vformat("case %d: a loop does not close", mask);
			}
			// Fan from a corner of the loop whose diagonals stay off the
			// faces with two cuts. A diagonal across such a face would lie in
			// it, and the cell on the other side could draw the same one,
			// leaving four triangles on one edge.
			const auto crosses_ambiguous_face = [&](int p_a, int p_b) {
				for (int f = 0; f < ambiguous_count; f++) {
					if (((ambiguous[f] >> p_a) & 1) && ((ambiguous[f] >> p_b) & 1)) {
						return true;
					}
				}
				return false;
			};
			int apex = -1;
			for (int s = 0; s < n && apex < 0; s++) {
				bool clean = true;
				for (int k = 2; k < n - 1 && clean; k++) {
					clean = !crosses_ambiguous_face(loop[s], loop[(s + k) % n]);
				}
				if (clean) {
					apex = s;
				}
			}
			if (apex < 0) {
				if (t.error.is_empty()) {
					t.error = vformat("case %d: no clean way to fan a loop", mask);
				}
				apex = 0;
			}
			for (int i = 1; i + 1 < n; i++) {
				t.triangles[mask][n_out++] = loop[apex];
				t.triangles[mask][n_out++] = loop[(apex + i) % n];
				t.triangles[mask][n_out++] = loop[(apex + i + 1) % n];
			}
		}
		t.triangles[mask][n_out] = -1;
	}

	// The loops run one way round the surface; Godot's front faces wind
	// clockwise. One cell with a single corner inside says which way the
	// table came out, and the same rule made every other case.
	const int8_t *tri = t.triangles[1];
	Vector3 p[3];
	for (int k = 0; k < 3; k++) {
		const int c0 = t.edge_corners[tri[k]][0];
		const int c1 = t.edge_corners[tri[k]][1];
		p[k] = (Vector3(corner_x(c0), corner_y(c0), corner_z(c0)) + Vector3(corner_x(c1), corner_y(c1), corner_z(c1))) * 0.5f;
	}
	const Vector3 winding = (p[1] - p[0]).cross(p[2] - p[0]);
	// Corner 0 is the inside one, so the air is towards (1, 1, 1).
	if (winding.dot(Vector3(1, 1, 1)) > 0.0) {
		for (int mask = 0; mask < 256; mask++) {
			for (int i = 0; t.triangles[mask][i] >= 0; i += 3) {
				SWAP(t.triangles[mask][i + 1], t.triangles[mask][i + 2]);
			}
		}
	}
	return t;
}

const Table &get_table() {
	static const Table table = make_table();
	return table;
}

} // namespace

String VoxelMesher::validate_table() {
	return get_table().error;
}

void VoxelMesher::build(const Input &p_input, Output &r_output) {
	const Table &t = get_table();
	const float *sdf = p_input.sdf;
	const uint8_t *material = p_input.material;

	bool any_inside = false;
	bool any_outside = false;
	for (int z = 0; z < POINTS && !(any_inside && any_outside); z++) {
		for (int y = 0; y < POINTS; y++) {
			const float *row = sdf + sample_index(0, y, z);
			for (int x = 0; x < POINTS; x++) {
				if (row[x] < 0.0f) {
					any_inside = true;
				} else {
					any_outside = true;
				}
			}
		}
	}
	if (!any_inside || !any_outside) {
		return;
	}

	LocalVector<int32_t> vertex_ids;
	vertex_ids.resize(POINTS * POINTS * POINTS * 3);
	memset(vertex_ids.ptr(), 0xff, vertex_ids.size() * sizeof(int32_t));

	LocalVector<Vector3> positions;
	LocalVector<Vector3> normals;
	LocalVector<uint8_t> weights; // MAX_LAYERS a vertex.
	LocalVector<uint8_t> faces; // Which faces of the block a vertex lies on.
	LocalVector<int32_t> indices;

	const auto gradient = [&](int p_x, int p_y, int p_z) {
		return Vector3(
				sdf[sample_index(p_x + 1, p_y, p_z)] - sdf[sample_index(p_x - 1, p_y, p_z)],
				sdf[sample_index(p_x, p_y + 1, p_z)] - sdf[sample_index(p_x, p_y - 1, p_z)],
				sdf[sample_index(p_x, p_y, p_z + 1)] - sdf[sample_index(p_x, p_y, p_z - 1)]);
	};

	const auto vertex = [&](int p_x, int p_y, int p_z, int p_axis) -> int32_t {
		const int key = ((p_z * POINTS + p_y) * POINTS + p_x) * 3 + p_axis;
		if (vertex_ids[key] >= 0) {
			return vertex_ids[key];
		}
		const int x1 = p_x + (p_axis == 0);
		const int y1 = p_y + (p_axis == 1);
		const int z1 = p_z + (p_axis == 2);
		const int i0 = sample_index(p_x, p_y, p_z);
		const int i1 = sample_index(x1, y1, z1);
		const float s0 = sdf[i0];
		const float s1 = sdf[i1];
		float f = s0 / (s0 - s1);
		if (!Math::is_finite(f)) {
			f = 0.5;
		}
		f = CLAMP(f, 0.0f, 1.0f);

		Vector3 p(p_x, p_y, p_z);
		p[p_axis] += f;
		Vector3 n = gradient(p_x, p_y, p_z).lerp(gradient(x1, y1, z1), f);
		n = n.length_squared() > 1e-12f ? n.normalized() : Vector3(0, 1, 0);

		// The layer of whichever end is in the ground, since that is what the
		// surface is made of.
		int layer = s0 < 0.0f ? material[i0] : material[i1];
		if (p_input.layer_at) {
			const int found = p_input.layer_at(p_input.layer_userdata, p, n);
			if (found >= 0) {
				layer = found;
			}
		}
		layer = MIN(layer, MAX_LAYERS - 1);

		uint8_t on_faces = 0;
		const int coord[3] = { p_x, p_y, p_z };
		for (int b = 0; b < 3; b++) {
			if (b == p_axis) {
				continue;
			}
			if (coord[b] == 0) {
				on_faces |= 1 << (b * 2);
			} else if (coord[b] == CELLS) {
				on_faces |= 1 << (b * 2 + 1);
			}
		}

		const int32_t id = positions.size();
		positions.push_back(p * p_input.cell_size);
		normals.push_back(n);
		const uint32_t w = weights.size();
		weights.resize(w + MAX_LAYERS);
		memset(weights.ptr() + w, 0, MAX_LAYERS);
		weights[w + layer] = 255;
		faces.push_back(on_faces);
		vertex_ids[key] = id;
		return id;
	};

	for (int z = 0; z < CELLS; z++) {
		for (int y = 0; y < CELLS; y++) {
			for (int x = 0; x < CELLS; x++) {
				int mask = 0;
				for (int c = 0; c < 8; c++) {
					if (sdf[sample_index(x + corner_x(c), y + corner_y(c), z + corner_z(c))] < 0.0f) {
						mask |= 1 << c;
					}
				}
				if (mask == 0 || mask == 255) {
					continue;
				}
				const int8_t *tri = t.triangles[mask];
				for (int i = 0; tri[i] >= 0; i += 3) {
					int32_t v[3];
					for (int k = 0; k < 3; k++) {
						const int edge = tri[i + k];
						const int c0 = t.edge_corners[edge][0];
						v[k] = vertex(x + corner_x(c0), y + corner_y(c0), z + corner_z(c0), t.edge_axis[edge]);
					}
					if (v[0] == v[1] || v[1] == v[2] || v[0] == v[2]) {
						continue;
					}
					indices.push_back(v[0]);
					indices.push_back(v[1]);
					indices.push_back(v[2]);
				}
			}
		}
	}

	const int surface_indices = indices.size();
	if (p_input.collision) {
		r_output.collision_faces.resize(surface_indices);
		Vector3 *dst = r_output.collision_faces.ptrw();
		for (int i = 0; i < surface_indices; i++) {
			dst[i] = positions[indices[i]];
		}
	}

	if (p_input.skirt > 0.0f) {
		// Where the surface meets a face of the block, hang a strip down from
		// it. A neighbouring block at another level of detail meets that face
		// along a slightly different line, and the strip fills the gap. It
		// hangs inside the face's plane, into the ground, and is two sided,
		// since the gap can be seen from either block.
		for (int i = 0; i < surface_indices; i += 3) {
			for (int k = 0; k < 3; k++) {
				const int32_t a = indices[i + k];
				const int32_t b = indices[i + (k + 1) % 3];
				const uint8_t shared = faces[a] & faces[b];
				if (!shared) {
					continue;
				}
				int face = 0;
				while (!(shared & (1 << face))) {
					face++;
				}
				const int axis = face >> 1;
				int32_t hung[2];
				const int32_t ends[2] = { a, b };
				for (int j = 0; j < 2; j++) {
					Vector3 d = -normals[ends[j]];
					d[axis] = 0.0;
					if (d.length_squared() < 0.04f) {
						// The surface runs along the face; drop straight
						// into the ground instead.
						d = -normals[ends[j]];
					}
					d.normalize();
					hung[j] = positions.size();
					positions.push_back(positions[ends[j]] + d * p_input.skirt);
					normals.push_back(normals[ends[j]]);
					const uint32_t w = weights.size();
					weights.resize(w + MAX_LAYERS);
					memcpy(weights.ptr() + w, weights.ptr() + ends[j] * MAX_LAYERS, MAX_LAYERS);
				}
				const int32_t quad[12] = { a, b, hung[1], a, hung[1], hung[0], a, hung[1], b, a, hung[0], hung[1] };
				for (int j = 0; j < 12; j++) {
					indices.push_back(quad[j]);
				}
			}
		}
	}

	const int vertex_count = positions.size();
	r_output.vertices.resize(vertex_count);
	r_output.normals.resize(vertex_count);
	memcpy(r_output.vertices.ptrw(), positions.ptr(), vertex_count * sizeof(Vector3));
	memcpy(r_output.normals.ptrw(), normals.ptr(), vertex_count * sizeof(Vector3));
	for (int g = 0; g < 4; g++) {
		r_output.weights[g].resize(vertex_count * 4);
		uint8_t *dst = r_output.weights[g].ptrw();
		for (int v = 0; v < vertex_count; v++) {
			memcpy(dst + v * 4, weights.ptr() + v * MAX_LAYERS + g * 4, 4);
		}
	}
	r_output.indices.resize(indices.size());
	memcpy(r_output.indices.ptrw(), indices.ptr(), indices.size() * sizeof(int32_t));
	r_output.surface_index_count = surface_indices;
}
