/**************************************************************************/
/*  mesh_painter.h                                                        */
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

#include "core/io/image.h"
#include "core/io/resource.h"
#include "core/math/triangle_mesh.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/resources/material.h"
#include "scene/resources/mesh.h"
#include "scene/resources/texture.h"

// Asosdot: the brush a MeshPainter paints with. Its own resource so a brush can be saved and
// shared, and so painting calls stay short.
class MeshPaintBrush : public Resource {
	GDCLASS(MeshPaintBrush, Resource);

public:
	enum BlendMode {
		BLEND_MIX,
		BLEND_ADD,
		BLEND_MULTIPLY,
		BLEND_LIGHTEN,
		BLEND_DARKEN,
		BLEND_ERASE,
		BLEND_BLUR,
		BLEND_MAX,
	};

private:
	Color color = Color(1, 1, 1, 1);
	float strength = 1.0;
	float hardness = 0.5;
	BlendMode blend_mode = BLEND_MIX;
	Ref<Image> texture_image;
	Ref<Texture2D> texture;
	float texture_angle = 0.0;
	bool paint_backfaces = false;
	bool occlusion = true;

protected:
	static void _bind_methods();

public:
	void set_color(const Color &p_color);
	Color get_color() const { return color; }
	void set_strength(float p_strength);
	float get_strength() const { return strength; }
	void set_hardness(float p_hardness);
	float get_hardness() const { return hardness; }
	void set_blend_mode(BlendMode p_mode);
	BlendMode get_blend_mode() const { return blend_mode; }
	void set_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_texture() const { return texture; }
	void set_texture_angle(float p_degrees);
	float get_texture_angle() const { return texture_angle; }
	void set_paint_backfaces(bool p_enable);
	bool is_painting_backfaces() const { return paint_backfaces; }
	void set_occlusion(bool p_enable);
	bool has_occlusion() const { return occlusion; }

	// RGBA8 copy of the texture, made once when the texture is set.
	const Ref<Image> &get_texture_image() const { return texture_image; }
	// Opacity at a texel, 0..1, for a distance of p_t (0 at the centre, 1 at the radius).
	float falloff(float p_t) const;
};

VARIANT_ENUM_CAST(MeshPaintBrush::BlendMode);

// Asosdot: the Mesh Paint tool's engine side. Loads any Mesh, keeps a face list where every
// face belongs to a material slot, finds UV islands, unwraps and packs UVs (xatlas), selects
// faces, and paints into images through the mesh's UVs with a 3D brush (so strokes cross UV
// seams cleanly) or straight in UV space. commit_mesh() gives back a plain ArrayMesh with one
// surface per slot, so painted meshes cost nothing extra at runtime.
class MeshPainter : public RefCounted {
	GDCLASS(MeshPainter, RefCounted);

public:
	enum Channel {
		CHANNEL_ALBEDO,
		CHANNEL_ROUGHNESS,
		CHANNEL_METALLIC,
		CHANNEL_EMISSION,
		CHANNEL_MAX,
	};

	struct UnwrapOptions {
		float angle_limit = 66.0; // Degrees; lower makes more, flatter islands.
		int padding = 4; // Texels between islands at the target resolution.
		int resolution = 1024;
		bool rotate = true;
		bool pack_only = false; // Keep the islands' shapes, only place them again.
	};

	// Set by the xatlas_unwrap module. With pack_only, p_uvs holds the current UVs (one per
	// vertex) and p_face_groups the island of each face. Otherwise the positions are unwrapped
	// and p_face_groups keeps faces of different groups out of the same island. Returns, per
	// face corner, the new UV (0..1) and the input vertex it came from.
	typedef bool (*UnwrapFunc)(const UnwrapOptions &p_options, const Vector<Vector3> &p_positions, const Vector<Vector3> &p_normals, const Vector<Vector2> &p_uvs, const Vector<int> &p_indices, const Vector<uint32_t> &p_face_groups, Vector<Vector2> &r_corner_uvs, Vector<int> &r_corner_xref);
	static inline UnwrapFunc unwrap_func = nullptr;

private:
	struct SourceSurface {
		Array arrays;
		Array blend_shapes;
		uint64_t format = 0;
		String name;
		int vertex_count = 0; // -1: a line or point surface, carried through as it is.
		// Typed copies of the arrays (shared, no extra memory), read once per vertex.
		Vector<Vector3> vertices;
		Vector<Vector3> normals;
		Vector<float> tangents;
		Vector<Color> colors;
		Vector<Vector2> uv2s;
		Vector<int> bones;
		Vector<float> weights;
	};

	struct Vert {
		int surface = 0;
		int index = 0;
		Vector2 uv;
	};

	struct Face {
		int v[3] = { 0, 0, 0 };
		int slot = 0;
	};

	struct Slot {
		Ref<Material> material;
		String name;
	};

	struct StrokeImage {
		Ref<Image> original;
		LocalVector<float> mask;
		Rect2i dirty;
	};

	Ref<Mesh> loaded_mesh;
	Ref<ArrayMesh> committed_mesh;
	Vector<SourceSurface> sources;
	LocalVector<Vert> verts;
	LocalVector<Face> faces;
	LocalVector<Slot> slots;
	LocalVector<uint8_t> selected;
	bool uvs_present = false;
	bool uvs_changed = false;

	// Geometry the brush and picking use: the posed mesh when one was given.
	LocalVector<Vector3> positions; // Per vert.
	LocalVector<Vector3> face_normals;
	LocalVector<AABB> face_bounds;
	Ref<TriangleMesh> triangle_mesh;
	real_t weld_epsilon = 0.0001;

	// Caches, rebuilt when the topology or UVs change.
	bool topology_dirty = true;
	bool islands_dirty = true;
	LocalVector<int> vert_weld; // Vert -> welded position id.
	int weld_count = 0;
	LocalVector<int> weld_face_offsets; // CSR: weld id -> faces using it.
	LocalVector<int> weld_faces;
	LocalVector<int> face_island;
	int island_count = 0;
	LocalVector<int> face_piece;
	int piece_count = 0;

	HashMap<Image *, StrokeImage> stroke_images;
	HashMap<int, bool> stroke_visibility;
	Vector3 stroke_eye;

	Array blend_shape_names;
	Mesh::BlendShapeMode blend_shape_mode = Mesh::BLEND_SHAPE_MODE_RELATIVE;
	Ref<Mesh> shadow_mesh;
	bool had_lods = false;
	AABB custom_aabb;
	Size2i lightmap_size_hint;

	void _clear();
	void _update_geometry(const Ref<Mesh> &p_pose_mesh);
	void _update_face_data();
	void _update_topology();
	void _update_islands();
	void _set_corner_uvs(const LocalVector<int> &p_faces, const Vector<Vector2> &p_corner_uvs);
	Vector2 _uv_density_scale(const LocalVector<int> &p_faces) const;
	StrokeImage &_stroke_image(const Ref<Image> &p_image);
	bool _face_visible(int p_face, const Vector3 &p_eye);
	void _blend_texel(StrokeImage &p_stroke, const Ref<Image> &p_image, int p_x, int p_y, float p_alpha, const Color &p_color, MeshPaintBrush::BlendMode p_mode);
	Color _sample_brush(const Ref<MeshPaintBrush> &p_brush, float p_u, float p_v) const;
	template <typename F>
	void _rasterize_face(int p_face, const Size2i &p_size, float p_bleed, F &&p_texel) const;

protected:
	static void _bind_methods();

public:
	// Loading and output.
	Error load_mesh(const Ref<Mesh> &p_mesh, const Ref<Mesh> &p_pose_mesh = Ref<Mesh>(), const TypedArray<Material> &p_surface_overrides = TypedArray<Material>());
	void set_pose_mesh(const Ref<Mesh> &p_pose_mesh);
	Ref<Mesh> get_loaded_mesh() const { return loaded_mesh; }
	Ref<ArrayMesh> get_committed_mesh() const { return committed_mesh; }
	Ref<ArrayMesh> commit_mesh(bool p_generate_lods = true);
	bool is_loaded() const { return !faces.is_empty(); }
	void clear() { _clear(); }

	int get_face_count() const { return faces.size(); }
	bool has_uvs() const { return uvs_present; }

	// Material slots.
	int get_slot_count() const { return slots.size(); }
	int add_slot(const Ref<Material> &p_material, const String &p_name = String());
	void remove_slot(int p_slot);
	void set_slot_material(int p_slot, const Ref<Material> &p_material);
	Ref<Material> get_slot_material(int p_slot) const;
	void set_slot_name(int p_slot, const String &p_name);
	String get_slot_name(int p_slot) const;
	int get_slot_face_count(int p_slot) const;
	PackedInt32Array get_slot_faces(int p_slot) const;
	void set_faces_slot(const PackedInt32Array &p_faces, int p_slot);
	int get_face_slot(int p_face) const;

	// Selection.
	void set_face_selected(int p_face, bool p_selected);
	bool is_face_selected(int p_face) const;
	PackedInt32Array get_selected_faces() const;
	void set_selected_faces(const PackedInt32Array &p_faces);
	int get_selected_face_count() const;
	void select_all(bool p_select = true);
	void invert_selection();
	void grow_selection();
	void shrink_selection();
	void select_islands_of_selection();
	void select_pieces_of_selection();

	// Islands (faces joined by shared UV edges) and pieces (faces joined in 3D).
	int get_island_count();
	int get_face_island(int p_face);
	PackedInt32Array get_island_faces(int p_island);
	PackedInt32Array get_islands_of_faces(const PackedInt32Array &p_faces);
	Rect2 get_island_uv_rect(int p_island);
	int get_face_piece(int p_face);
	PackedInt32Array get_piece_faces(int p_piece);

	// UVs.
	Vector2 get_face_uv(int p_face, int p_corner) const;
	PackedVector2Array get_face_uvs() const;
	void transform_faces_uv(const PackedInt32Array &p_faces, const Transform2D &p_transform);
	Error smart_unwrap(const PackedInt32Array &p_faces = PackedInt32Array(), float p_angle_limit = 66.0, int p_padding = 4, int p_resolution = 1024);
	Error pack_islands(int p_padding = 4, int p_resolution = 1024, bool p_rotate = true, bool p_average_scale = true);

	// Geometry queries, in the mesh's local space.
	Dictionary intersect_ray(const Vector3 &p_from, const Vector3 &p_dir) const;
	bool intersect_ray_face(const Vector3 &p_from, const Vector3 &p_dir, int &r_face, Vector3 &r_position, Vector3 &r_normal) const;
	PackedInt32Array get_faces_in_sphere(const Vector3 &p_center, real_t p_radius, const Vector3 &p_eye = Vector3(), bool p_front_only = false);
	Vector3 get_face_normal(int p_face) const;
	Vector3 get_face_position(int p_face, int p_corner) const;
	Vector3 get_barycentric(int p_face, const Vector3 &p_point) const;
	PackedVector3Array get_faces_triangles(const PackedInt32Array &p_faces, real_t p_offset = 0.0) const;
	AABB get_aabb() const;

	// Painting. Images are RGBA8. A stroke groups dabs so one stroke never darkens itself
	// where dabs overlap, and keeps the untouched image for undo.
	void begin_stroke();
	Rect2i paint_dab(const Ref<Image> &p_image, const Ref<MeshPaintBrush> &p_brush, const Vector3 &p_center, real_t p_radius, const Vector3 &p_eye, const Vector3 &p_up, int p_slot = -1);
	Rect2i paint_dab_uv(const Ref<Image> &p_image, const Ref<MeshPaintBrush> &p_brush, const Vector2 &p_uv, real_t p_radius_uv);
	Rect2i fill_faces(const Ref<Image> &p_image, const PackedInt32Array &p_faces, const Ref<MeshPaintBrush> &p_brush);
	Rect2i get_stroke_rect(const Ref<Image> &p_image) const;
	Ref<Image> get_stroke_original(const Ref<Image> &p_image) const;
	TypedArray<Image> get_stroke_images() const;
	void end_stroke();
	Color sample_image(const Ref<Image> &p_image, int p_face, const Vector3 &p_barycentric) const;
	Vector2 get_uv_at(int p_face, const Vector3 &p_barycentric) const;

	// Material channels (StandardMaterial3D / ORMMaterial3D).
	static Ref<Texture2D> get_channel_texture(const Ref<BaseMaterial3D> &p_material, Channel p_channel);
	static Ref<Image> make_channel_image(const Ref<BaseMaterial3D> &p_material, Channel p_channel, const Vector2i &p_size);
	static void set_channel_texture(const Ref<BaseMaterial3D> &p_material, Channel p_channel, const Ref<Texture2D> &p_texture);
	static String get_channel_name(Channel p_channel);

	MeshPainter();
};

VARIANT_ENUM_CAST(MeshPainter::Channel);
