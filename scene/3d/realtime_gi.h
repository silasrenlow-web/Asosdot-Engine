/**************************************************************************/
/*  realtime_gi.h                                                         */
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

#include "core/io/resource.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/resources/mesh.h"

// What a RealtimeGI bake produces: the patches static surfaces were cut into,
// how light travels between them, the geometry that casts shadows, and the
// probes that light dynamic objects. None of it depends on the lights, which
// is what lets them move.
class RealtimeGIData : public Resource {
	GDCLASS(RealtimeGIData, Resource);
	RES_BASE_EXTENSION("rgi");

public:
	struct User {
		NodePath path;
		int32_t sub_instance = -1;
		Rect2 uv_scale;
		int32_t mesh = -1; // into meshes, for nodes drawn with a copy given UV2
	};

private:
	Vector<User> users;
	Vector<Ref<Mesh>> meshes;

	Vector2i atlas_size;
	float patch_size = 0.5;
	PackedFloat32Array patches;
	PackedInt32Array cluster_offsets;
	PackedInt32Array texel_patches;

	int transport_rays = 0;
	PackedInt32Array transport_offsets;
	PackedInt32Array transport_entries;
	PackedFloat32Array sky_visibility;

	AABB bounds;
	PackedInt32Array bvh;
	PackedFloat32Array triangles;

	PackedVector3Array probe_points;
	int probe_rays = 0;
	PackedInt32Array probe_ray_hits;
	PackedInt32Array probe_tetrahedra;
	PackedInt32Array probe_bsp_tree;

	void _set_user_data(const Array &p_data);
	Array _get_user_data() const;
	void _set_meshes(const Array &p_meshes);
	Array _get_meshes() const;
	void _set_surface_data(const Dictionary &p_data);
	Dictionary _get_surface_data() const;
	void _set_transport_data(const Dictionary &p_data);
	Dictionary _get_transport_data() const;
	void _set_geometry_data(const Dictionary &p_data);
	Dictionary _get_geometry_data() const;
	void _set_probe_data(const Dictionary &p_data);
	Dictionary _get_probe_data() const;

protected:
	static void _bind_methods();

public:
	void add_user(const NodePath &p_path, const Rect2 &p_uv_scale, int32_t p_sub_instance = -1, int32_t p_mesh = -1);
	int add_mesh(const Ref<Mesh> &p_mesh);
	Ref<Mesh> get_user_mesh(int p_user) const;
	int get_generated_mesh_count() const { return meshes.size(); }
	int get_user_count() const;
	NodePath get_user_path(int p_user) const;
	int32_t get_user_sub_instance(int p_user) const;
	Rect2 get_user_lightmap_uv_scale(int p_user) const;
	void clear_users(); // and the meshes they were given

	void set_surfaces(const Vector2i &p_atlas_size, float p_patch_size, const PackedFloat32Array &p_patches, const PackedInt32Array &p_cluster_offsets, const PackedInt32Array &p_texel_patches);
	void set_transport(int p_ray_count, const PackedInt32Array &p_offsets, const PackedInt32Array &p_entries, const PackedFloat32Array &p_sky_visibility);
	void set_geometry(const AABB &p_bounds, const PackedInt32Array &p_bvh, const PackedFloat32Array &p_triangles);
	void set_probes(const PackedVector3Array &p_points, int p_ray_count, const PackedInt32Array &p_rays, const PackedInt32Array &p_tetrahedra, const PackedInt32Array &p_bsp_tree);

	Vector2i get_atlas_size() const { return atlas_size; }
	float get_patch_size() const { return patch_size; }
	int get_patch_count() const { return patches.size() / 16; }
	int get_cluster_count() const { return MAX(cluster_offsets.size() - 1, 0); }
	int get_transport_entry_count() const { return transport_entries.size(); }
	int get_triangle_count() const { return triangles.size() / 12; }
	int get_probe_count() const { return probe_points.size(); }
	PackedVector3Array get_probe_points() const { return probe_points; }
	AABB get_bounds() const { return bounds; }

	// Pushes everything to a RenderingServer realtime GI.
	void apply(RID p_realtime_gi) const;
};

class RealtimeGI : public VisualInstance3D {
	GDCLASS(RealtimeGI, VisualInstance3D);

public:
	enum BakeQuality {
		BAKE_QUALITY_LOW,
		BAKE_QUALITY_MEDIUM,
		BAKE_QUALITY_HIGH,
		BAKE_QUALITY_ULTRA,
	};

	enum BakeError {
		BAKE_ERROR_OK,
		BAKE_ERROR_NO_SCENE_ROOT,
		BAKE_ERROR_NO_SAVE_PATH,
		BAKE_ERROR_NO_MESHES,
		BAKE_ERROR_NO_RAYCASTER,
		BAKE_ERROR_CANT_SAVE,
		BAKE_ERROR_USER_ABORTED,
	};

	typedef bool (*BakeStepFunc)(float p_progress, const String &p_description, void *p_userdata, bool p_refresh);

	// Bake internals, defined in realtime_gi.cpp.
	struct BakeMesh;
	struct Baker;

private:
	RID realtime_gi;
	Ref<RealtimeGIData> data;

	float patch_size = 0.5;
	BakeQuality bake_quality = BAKE_QUALITY_MEDIUM;
	float probe_spacing = 2.0;
	bool generate_uv2 = true;
	int max_patches = 65536;

	float energy = 1.0;
	float environment_energy = 1.0;
	float albedo_boost = 1.0;
	float emission_energy = 1.0;
	bool interior = false;

	int bounces_per_frame = 1;
	int shadow_samples = 2;
	int probe_update_interval = 4;

	Node *last_owner = nullptr;

	void _find_meshes(Node *p_at_node, Vector<BakeMesh> &r_meshes) const;
	void _assign_lightmaps();
	void _clear_lightmaps();
	void _update_energy();
	void _update_quality();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_data(const Ref<RealtimeGIData> &p_data);
	Ref<RealtimeGIData> get_data() const;

	void set_patch_size(float p_size);
	float get_patch_size() const;

	void set_bake_quality(BakeQuality p_quality);
	BakeQuality get_bake_quality() const;

	void set_probe_spacing(float p_spacing);
	float get_probe_spacing() const;

	void set_max_patches(int p_patches);
	int get_max_patches() const;

	void set_generate_uv2(bool p_enable);
	bool is_generating_uv2() const;

	void set_energy(float p_energy);
	float get_energy() const;

	void set_environment_energy(float p_energy);
	float get_environment_energy() const;

	void set_albedo_boost(float p_boost);
	float get_albedo_boost() const;

	void set_emission_energy(float p_energy);
	float get_emission_energy() const;

	void set_interior(bool p_interior);
	bool is_interior() const;

	void set_bounces_per_frame(int p_bounces);
	int get_bounces_per_frame() const;

	void set_shadow_samples(int p_samples);
	int get_shadow_samples() const;

	void set_probe_update_interval(int p_frames);
	int get_probe_update_interval() const;

	virtual AABB get_aabb() const override;

	// Bakes the static meshes under p_from_node. With no save path, saves
	// next to the scene.
	BakeError bake(Node *p_from_node, const String &p_save_path = String(), BakeStepFunc p_step = nullptr, void *p_userdata = nullptr);
	int _bake_bind(Node *p_from_node, const String &p_save_path);

	PackedStringArray get_configuration_warnings() const override;

	RealtimeGI();
	~RealtimeGI();
};

VARIANT_ENUM_CAST(RealtimeGI::BakeQuality);
VARIANT_ENUM_CAST(RealtimeGI::BakeError);
