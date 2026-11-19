/**************************************************************************/
/*  light_volume_3d.h                                                     */
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

#include "scene/3d/visual_instance_3d.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/mesh.h"
#include "scene/resources/shader.h"
#include "scene/resources/texture.h"

class Light3D;
class ShaderMaterial;

// An editable shaft of light: a box, cone or cylinder volume that extends
// along the node's -Z axis and renders as a soft volumetric-looking beam.
// Unlike the world volumetric fog, a LightVolume3D belongs to one light or
// one opening: shape it in the editor (or fit it to a SpotLight3D), give it
// a cookie texture to project through it, and tilt the far end with shear
// so the shaft slants the way real light falls through a window.
//
// The shaft is a generated shell mesh with a built-in additive shader:
// fresnel edge fade, axial fade, animated noise (dust shimmer), a projected
// cookie, and a depth fade so geometry crossing the beam softens it instead
// of slicing it. Geometry in front of the beam still occludes it through
// the normal depth test.
class LightVolume3D : public VisualInstance3D {
	GDCLASS(LightVolume3D, VisualInstance3D);

public:
	enum Shape {
		SHAPE_BOX,
		SHAPE_CONE,
		SHAPE_CYLINDER,
	};

private:
	Shape shape = SHAPE_CONE;
	real_t length = 6.0;
	Vector2 size = Vector2(2, 2); // Box cross-section (X, Y).
	real_t start_radius = 0.4; // Cone/cylinder radius at the source.
	real_t end_radius = 1.6; // Cone/cylinder radius at the far end.
	Vector2 shear; // Far-end offset: slants the shaft without rotating the source.

	Color color = Color(1, 1, 1);
	real_t intensity = 0.35;
	real_t edge_softness = 1.5;
	real_t fade_start = 0.05;
	real_t fade_end = 0.9;

	Ref<Texture2D> cookie;
	real_t cookie_strength = 1.0;

	real_t noise_amount = 0.35;
	real_t noise_scale = 0.45;
	real_t noise_speed = 0.06;

	bool depth_fade = true;
	real_t depth_fade_distance = 0.75;

	NodePath source_light;
	bool follow_light = true;
	bool auto_fit = true;

	Ref<ArrayMesh> mesh;
	Ref<Shader> shader;
	Ref<ShaderMaterial> material;
	Ref<ImageTexture> white_texture;
	AABB aabb;

	void _build_mesh();
	void _update_material();
	void _apply_light(Light3D *p_light);
	void _update_processing();

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;
	void _notification(int p_what);

public:
	void set_shape(Shape p_shape);
	Shape get_shape() const;

	void set_length(real_t p_length);
	real_t get_length() const;

	void set_size(const Vector2 &p_size);
	Vector2 get_size() const;

	void set_start_radius(real_t p_radius);
	real_t get_start_radius() const;

	void set_end_radius(real_t p_radius);
	real_t get_end_radius() const;

	void set_shear(const Vector2 &p_shear);
	Vector2 get_shear() const;

	void set_color(const Color &p_color);
	Color get_color() const;

	void set_intensity(real_t p_intensity);
	real_t get_intensity() const;

	void set_edge_softness(real_t p_softness);
	real_t get_edge_softness() const;

	void set_fade_start(real_t p_fade);
	real_t get_fade_start() const;

	void set_fade_end(real_t p_fade);
	real_t get_fade_end() const;

	void set_cookie(const Ref<Texture2D> &p_cookie);
	Ref<Texture2D> get_cookie() const;

	void set_cookie_strength(real_t p_strength);
	real_t get_cookie_strength() const;

	void set_noise_amount(real_t p_amount);
	real_t get_noise_amount() const;

	void set_noise_scale(real_t p_scale);
	real_t get_noise_scale() const;

	void set_noise_speed(real_t p_speed);
	real_t get_noise_speed() const;

	void set_depth_fade(bool p_enabled);
	bool get_depth_fade() const;

	void set_depth_fade_distance(real_t p_distance);
	real_t get_depth_fade_distance() const;

	void set_source_light(const NodePath &p_path);
	NodePath get_source_light() const;

	void set_follow_light(bool p_follow);
	bool get_follow_light() const;

	void set_auto_fit(bool p_fit);
	bool get_auto_fit() const;

	// Editor gizmo and tests read the generated geometry through these.
	Ref<ArrayMesh> get_mesh() const { return mesh; }
	Vector2 get_near_extent() const;
	Vector2 get_far_extent() const;

	virtual AABB get_aabb() const override;
	virtual PackedStringArray get_configuration_warnings() const override;

	LightVolume3D();
	~LightVolume3D();
};

VARIANT_ENUM_CAST(LightVolume3D::Shape);
