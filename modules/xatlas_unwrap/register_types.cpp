/**************************************************************************/
/*  register_types.cpp                                                    */
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

#include "register_types.h"

#include "core/crypto/crypto_core.h"
#include "core/error/error_macros.h"
#include "core/os/memory.h"

#ifndef _3D_DISABLED
#include "scene/resources/3d/mesh_painter.h"
#endif // _3D_DISABLED

#include <xatlas.h>

#include <cstring>

extern bool (*array_mesh_lightmap_unwrap_callback)(float p_texel_size, const float *p_vertices, const float *p_normals, int p_vertex_count, const int *p_indices, int p_index_count, const uint8_t *p_cache_data, bool *r_use_cache, uint8_t **r_mesh_cache, int *r_mesh_cache_size, float **r_uv, int **r_vertex, int *r_vertex_count, int **r_index, int *r_index_count, int *r_size_hint_x, int *r_size_hint_y);

bool xatlas_mesh_lightmap_unwrap_callback(float p_texel_size, const float *p_vertices, const float *p_normals, int p_vertex_count, const int *p_indices, int p_index_count, const uint8_t *p_cache_data, bool *r_use_cache, uint8_t **r_mesh_cache, int *r_mesh_cache_size, float **r_uv, int **r_vertex, int *r_vertex_count, int **r_index, int *r_index_count, int *r_size_hint_x, int *r_size_hint_y) {
	CryptoCore::MD5Context ctx;
	ctx.start();

	ctx.update((unsigned char *)&p_texel_size, sizeof(float));
	ctx.update((unsigned char *)p_indices, sizeof(int) * p_index_count);
	ctx.update((unsigned char *)p_vertices, sizeof(float) * p_vertex_count * 3);
	ctx.update((unsigned char *)p_normals, sizeof(float) * p_vertex_count * 3);

	unsigned char hash[16];
	ctx.finish(hash);

	bool cached = false;
	unsigned int cache_idx = 0;

	*r_mesh_cache = nullptr;
	*r_mesh_cache_size = 0;

	if (p_cache_data) {
		//Check if hash is in cache data
		int *cache_data = (int *)p_cache_data;
		int n_entries = cache_data[0];
		unsigned int read_idx = 1;
		for (int i = 0; i < n_entries; ++i) {
			if (memcmp(&cache_data[read_idx], hash, 16) == 0) {
				cached = true;
				cache_idx = read_idx;
				break;
			}

			read_idx += 4; // hash
			read_idx += 2; // size hint

			int vertex_count = cache_data[read_idx];
			read_idx += 1; // vertex count
			read_idx += vertex_count; // vertex
			read_idx += vertex_count * 2; // uvs

			int index_count = cache_data[read_idx];
			read_idx += 1; // index count
			read_idx += index_count; // indices
		}
	}

	if (cached) {
		int *cache_data = (int *)p_cache_data;

		cache_idx += 4;

		// Load size
		*r_size_hint_x = cache_data[cache_idx];
		*r_size_hint_y = cache_data[cache_idx + 1];
		cache_idx += 2;

		// Load vertices
		*r_vertex_count = cache_data[cache_idx];
		cache_idx++;
		*r_vertex = &cache_data[cache_idx];
		cache_idx += *r_vertex_count;

		// Load UVs
		*r_uv = (float *)&cache_data[cache_idx];
		cache_idx += *r_vertex_count * 2;

		// Load indices
		*r_index_count = cache_data[cache_idx];
		cache_idx++;
		*r_index = &cache_data[cache_idx];
	} else {
		// set up input mesh
		xatlas::MeshDecl input_mesh;
		input_mesh.indexData = p_indices;
		input_mesh.indexCount = p_index_count;
		input_mesh.indexFormat = xatlas::IndexFormat::UInt32;

		input_mesh.vertexCount = p_vertex_count;
		input_mesh.vertexPositionData = p_vertices;
		input_mesh.vertexPositionStride = sizeof(float) * 3;
		input_mesh.vertexNormalData = p_normals;
		input_mesh.vertexNormalStride = sizeof(uint32_t) * 3;
		input_mesh.vertexUvData = nullptr;
		input_mesh.vertexUvStride = 0;

		xatlas::ChartOptions chart_options;
		chart_options.fixWinding = true;

		ERR_FAIL_COND_V_MSG(p_texel_size <= 0.0f, false, "Texel size must be greater than 0.");

		xatlas::PackOptions pack_options;
		pack_options.padding = 1;
		pack_options.maxChartSize = 4094; // Lightmap atlassing needs 2 for padding between meshes, so 4096-2
		pack_options.blockAlign = true;
		pack_options.texelsPerUnit = 1.0 / p_texel_size;

		xatlas::Atlas *atlas = xatlas::Create();

		xatlas::AddMeshError err = xatlas::AddMesh(atlas, input_mesh, 1);
		ERR_FAIL_COND_V_MSG(err != xatlas::AddMeshError::Success, false, xatlas::StringForEnum(err));

		xatlas::Generate(atlas, chart_options, pack_options);

		*r_size_hint_x = atlas->width;
		*r_size_hint_y = atlas->height;

		float w = *r_size_hint_x;
		float h = *r_size_hint_y;

		if (w == 0 || h == 0) {
			xatlas::Destroy(atlas);
			return false; //could not bake because there is no area
		}

		const xatlas::Mesh &output = atlas->meshes[0];

		*r_vertex = (int *)memalloc(sizeof(int) * output.vertexCount);
		ERR_FAIL_NULL_V_MSG(*r_vertex, false, "Out of memory.");
		*r_uv = (float *)memalloc(sizeof(float) * output.vertexCount * 2);
		ERR_FAIL_NULL_V_MSG(*r_uv, false, "Out of memory.");
		*r_index = (int *)memalloc(sizeof(int) * output.indexCount);
		ERR_FAIL_NULL_V_MSG(*r_index, false, "Out of memory.");

		float max_x = 0;
		float max_y = 0;
		for (uint32_t i = 0; i < output.vertexCount; i++) {
			(*r_vertex)[i] = output.vertexArray[i].xref;
			(*r_uv)[i * 2 + 0] = output.vertexArray[i].uv[0] / w;
			(*r_uv)[i * 2 + 1] = output.vertexArray[i].uv[1] / h;
			max_x = MAX(max_x, output.vertexArray[i].uv[0]);
			max_y = MAX(max_y, output.vertexArray[i].uv[1]);
		}

		*r_vertex_count = output.vertexCount;

		for (uint32_t i = 0; i < output.indexCount; i++) {
			(*r_index)[i] = output.indexArray[i];
		}

		*r_index_count = output.indexCount;

		xatlas::Destroy(atlas);
	}

	if (*r_use_cache) {
		// Build cache data for current mesh

		unsigned int new_cache_size = 4 + 2 + 1 + *r_vertex_count + (*r_vertex_count * 2) + 1 + *r_index_count; // hash + size hint + vertex_count + vertices + uvs + index_count + indices
		new_cache_size *= sizeof(int);
		int *new_cache_data = (int *)memalloc(new_cache_size);
		unsigned int new_cache_idx = 0;

		// hash
		memcpy(&new_cache_data[new_cache_idx], hash, 16);
		new_cache_idx += 4;

		// size hint
		new_cache_data[new_cache_idx] = *r_size_hint_x;
		new_cache_data[new_cache_idx + 1] = *r_size_hint_y;
		new_cache_idx += 2;

		// vertex count
		new_cache_data[new_cache_idx] = *r_vertex_count;
		new_cache_idx++;

		// vertices
		memcpy(&new_cache_data[new_cache_idx], *r_vertex, sizeof(int) * (*r_vertex_count));
		new_cache_idx += *r_vertex_count;

		// uvs
		memcpy(&new_cache_data[new_cache_idx], *r_uv, sizeof(float) * (*r_vertex_count) * 2);
		new_cache_idx += *r_vertex_count * 2;

		// index count
		new_cache_data[new_cache_idx] = *r_index_count;
		new_cache_idx++;

		// indices
		memcpy(&new_cache_data[new_cache_idx], *r_index, sizeof(int) * (*r_index_count));

		// Return cache data to the caller
		*r_mesh_cache = (uint8_t *)new_cache_data;
		*r_mesh_cache_size = new_cache_size;
	}

	*r_use_cache = cached; // Return whether cache was used.

	return true;
}

#ifndef _3D_DISABLED
// Asosdot: the Mesh Paint tool's Smart Unwrap and Pack Islands. Unlike the lightmap unwrap,
// faces carry a group (material slot, or island when packing) and the result is scaled to
// fill a square texture of the asked resolution.
static bool xatlas_mesh_painter_unwrap(const MeshPainter::UnwrapOptions &p_options, const Vector<Vector3> &p_positions, const Vector<Vector3> &p_normals, const Vector<Vector2> &p_uvs, const Vector<int> &p_indices, const Vector<uint32_t> &p_face_groups, Vector<Vector2> &r_corner_uvs, Vector<int> &r_corner_xref) {
	const int face_count = p_indices.size() / 3;
	ERR_FAIL_COND_V(face_count == 0 || p_indices.size() % 3 != 0, false);
	ERR_FAIL_COND_V(p_face_groups.size() != face_count, false);
	const int resolution = MAX(p_options.resolution, 16);

	xatlas::Atlas *atlas = xatlas::Create();
	Vector<float> uv_data;
	Vector<float> position_data;
	Vector<float> normal_data;
	double area = 0.0;

	if (p_options.pack_only) {
		uv_data.resize(p_uvs.size() * 2);
		for (int i = 0; i < p_uvs.size(); i++) {
			uv_data.write[i * 2 + 0] = p_uvs[i].x;
			uv_data.write[i * 2 + 1] = p_uvs[i].y;
		}
		for (int f = 0; f < face_count; f++) {
			const Vector2 a = p_uvs[p_indices[f * 3 + 0]];
			const Vector2 b = p_uvs[p_indices[f * 3 + 1]];
			const Vector2 c = p_uvs[p_indices[f * 3 + 2]];
			area += Math::abs((b - a).cross(c - a)) * 0.5;
		}
		xatlas::UvMeshDecl decl;
		decl.vertexUvData = uv_data.ptr();
		decl.vertexStride = sizeof(float) * 2;
		decl.vertexCount = p_uvs.size();
		decl.indexData = p_indices.ptr();
		decl.indexCount = p_indices.size();
		decl.indexFormat = xatlas::IndexFormat::UInt32;
		decl.faceMaterialData = p_face_groups.ptr();
		const xatlas::AddMeshError err = xatlas::AddUvMesh(atlas, decl);
		if (err != xatlas::AddMeshError::Success) {
			xatlas::Destroy(atlas);
			ERR_FAIL_V_MSG(false, xatlas::StringForEnum(err));
		}
		xatlas::ComputeCharts(atlas);
	} else {
		ERR_FAIL_COND_V(p_positions.size() != p_normals.size(), false);
		position_data.resize(p_positions.size() * 3);
		normal_data.resize(p_normals.size() * 3);
		for (int i = 0; i < p_positions.size(); i++) {
			for (int k = 0; k < 3; k++) {
				position_data.write[i * 3 + k] = p_positions[i][k];
				normal_data.write[i * 3 + k] = p_normals[i][k];
			}
		}
		for (int f = 0; f < face_count; f++) {
			area += Face3(p_positions[p_indices[f * 3]], p_positions[p_indices[f * 3 + 1]], p_positions[p_indices[f * 3 + 2]]).get_area();
		}
		xatlas::MeshDecl decl;
		decl.vertexPositionData = position_data.ptr();
		decl.vertexPositionStride = sizeof(float) * 3;
		decl.vertexNormalData = normal_data.ptr();
		decl.vertexNormalStride = sizeof(float) * 3;
		decl.vertexCount = p_positions.size();
		decl.indexData = p_indices.ptr();
		decl.indexCount = p_indices.size();
		decl.indexFormat = xatlas::IndexFormat::UInt32;
		decl.faceMaterialData = p_face_groups.ptr();
		const xatlas::AddMeshError err = xatlas::AddMesh(atlas, decl, 1);
		if (err != xatlas::AddMeshError::Success) {
			xatlas::Destroy(atlas);
			ERR_FAIL_V_MSG(false, xatlas::StringForEnum(err));
		}
		xatlas::ChartOptions chart_options;
		chart_options.fixWinding = true;
		// xatlas grows islands while the face's normal deviation (1 - cos) times this weight
		// stays under maxCost; at 66 degrees this is xatlas' own default weight of 2.
		const float angle = Math::deg_to_rad(CLAMP(p_options.angle_limit, 1.0f, 89.0f));
		chart_options.normalDeviationWeight = 2.0f * (1.0f - Math::cos(Math::deg_to_rad(66.0f))) / MAX(1.0f - Math::cos(angle), 0.0001f);
		chart_options.maxIterations = 2;
		xatlas::ComputeCharts(atlas, chart_options);
	}

	if (area <= 0.0) {
		xatlas::Destroy(atlas);
		ERR_FAIL_V_MSG(false, "Nothing to unwrap: the faces have no area.");
	}

	// Aim for the charts to fill most of the square, then correct the scale from what the
	// packer actually made.
	float texels_per_unit = resolution * Math::sqrt(0.6 / area);
	uint32_t size = 0;
	for (int attempt = 0; attempt < 5; attempt++) {
		xatlas::PackOptions pack_options;
		pack_options.padding = MAX(p_options.padding, 0);
		pack_options.texelsPerUnit = texels_per_unit;
		pack_options.bilinear = true;
		pack_options.rotateCharts = p_options.rotate;
		pack_options.rotateChartsToAxis = p_options.rotate;
		pack_options.maxChartSize = resolution - pack_options.padding * 2;
		xatlas::PackCharts(atlas, pack_options);
		size = MAX(atlas->width, atlas->height);
		if (size == 0) {
			break;
		}
		const float ratio = float(resolution) / size;
		if (ratio >= 0.97f && ratio <= 1.0f) {
			break;
		}
		texels_per_unit *= ratio * (ratio > 1.0f ? 0.99f : 0.98f);
	}
	if (size == 0 || atlas->meshCount == 0) {
		xatlas::Destroy(atlas);
		ERR_FAIL_V_MSG(false, "xatlas made no charts.");
	}

	const xatlas::Mesh &output = atlas->meshes[0];
	if ((int)output.indexCount != p_indices.size()) {
		xatlas::Destroy(atlas);
		ERR_FAIL_V_MSG(false, "xatlas changed the face count.");
	}
	// Divide by the longer side, so a non-square atlas keeps its proportions in the square.
	const float scale = 1.0f / MAX(size, (uint32_t)resolution);
	r_corner_uvs.resize(output.indexCount);
	r_corner_xref.resize(output.indexCount);
	for (uint32_t i = 0; i < output.indexCount; i++) {
		const xatlas::Vertex &vertex = output.vertexArray[output.indexArray[i]];
		r_corner_uvs.write[i] = Vector2(vertex.uv[0] * scale, vertex.uv[1] * scale);
		r_corner_xref.write[i] = vertex.xref;
	}
	xatlas::Destroy(atlas);
	return true;
}
#endif // _3D_DISABLED

void initialize_xatlas_unwrap_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	array_mesh_lightmap_unwrap_callback = xatlas_mesh_lightmap_unwrap_callback;
#ifndef _3D_DISABLED
	MeshPainter::unwrap_func = xatlas_mesh_painter_unwrap;
#endif // _3D_DISABLED
}

void uninitialize_xatlas_unwrap_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}
