/**************************************************************************/
/*  dust_volume_3d.cpp                                                    */
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

#include "dust_volume_3d.h"

#include "core/io/image.h"
#include "core/object/class_db.h"
#include "scene/resources/gradient_texture.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "scene/resources/particle_process_material.h"
#include "scene/resources/3d/primitive_meshes.h"

Ref<ImageTexture> DustVolume3D::mote_texture;

int DustVolume3D::_mote_count() const {
	real_t volume = size.x * size.y * size.z;
	return CLAMP(int(Math::round(volume * density)), 1, 4096);
}

void DustVolume3D::_rebuild() {
	if (process_material.is_null()) {
		return;
	}
	set_amount(_mote_count());
	set_visibility_aabb(AABB(-size * 0.5 - Vector3(0.5, 0.5, 0.5), size + Vector3(1, 1, 1)));

	process_material->set_emission_box_extents(size * 0.5);
	process_material->set_param_min(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, drift_speed * 0.4);
	process_material->set_param_max(ParticleProcessMaterial::PARAM_INITIAL_LINEAR_VELOCITY, drift_speed);

	quad->set_size(Vector2(mote_size, mote_size));
	mote_material->set_albedo(Color(mote_color, opacity));

	Ref<Gradient> gradient = fade_ramp->get_gradient();
	gradient->set_color(0, Color(1, 1, 1, 0));
	gradient->set_color(1, Color(1, 1, 1, 1));
	gradient->set_color(2, Color(1, 1, 1, 1));
	gradient->set_color(3, Color(1, 1, 1, 0));
}

void DustVolume3D::set_size(const Vector3 &p_size) {
	size = Vector3(MAX(p_size.x, 0.1), MAX(p_size.y, 0.1), MAX(p_size.z, 0.1));
	_rebuild();
}

Vector3 DustVolume3D::get_size() const {
	return size;
}

void DustVolume3D::set_density(real_t p_density) {
	density = CLAMP(p_density, 0.1, 200.0);
	_rebuild();
}

real_t DustVolume3D::get_density() const {
	return density;
}

void DustVolume3D::set_mote_size(real_t p_size) {
	mote_size = CLAMP(p_size, 0.002, 0.5);
	_rebuild();
}

real_t DustVolume3D::get_mote_size() const {
	return mote_size;
}

void DustVolume3D::set_mote_color(const Color &p_color) {
	mote_color = p_color;
	_rebuild();
}

Color DustVolume3D::get_mote_color() const {
	return mote_color;
}

void DustVolume3D::set_opacity(real_t p_opacity) {
	opacity = CLAMP(p_opacity, 0.0, 1.0);
	_rebuild();
}

real_t DustVolume3D::get_opacity() const {
	return opacity;
}

void DustVolume3D::set_drift_speed(real_t p_speed) {
	drift_speed = CLAMP(p_speed, 0.0, 1.0);
	_rebuild();
}

real_t DustVolume3D::get_drift_speed() const {
	return drift_speed;
}

void DustVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &DustVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &DustVolume3D::get_size);
	ClassDB::bind_method(D_METHOD("set_density", "density"), &DustVolume3D::set_density);
	ClassDB::bind_method(D_METHOD("get_density"), &DustVolume3D::get_density);
	ClassDB::bind_method(D_METHOD("set_mote_size", "mote_size"), &DustVolume3D::set_mote_size);
	ClassDB::bind_method(D_METHOD("get_mote_size"), &DustVolume3D::get_mote_size);
	ClassDB::bind_method(D_METHOD("set_mote_color", "mote_color"), &DustVolume3D::set_mote_color);
	ClassDB::bind_method(D_METHOD("get_mote_color"), &DustVolume3D::get_mote_color);
	ClassDB::bind_method(D_METHOD("set_opacity", "opacity"), &DustVolume3D::set_opacity);
	ClassDB::bind_method(D_METHOD("get_opacity"), &DustVolume3D::get_opacity);
	ClassDB::bind_method(D_METHOD("set_drift_speed", "drift_speed"), &DustVolume3D::set_drift_speed);
	ClassDB::bind_method(D_METHOD("get_drift_speed"), &DustVolume3D::get_drift_speed);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, ""), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "density", PROPERTY_HINT_RANGE, "0.1,200,0.1"), "set_density", "get_density");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mote_size", PROPERTY_HINT_RANGE, "0.002,0.5,0.001,suffix:m"), "set_mote_size", "get_mote_size");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "mote_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_mote_color", "get_mote_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "opacity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_opacity", "get_opacity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "drift_speed", PROPERTY_HINT_RANGE, "0,1,0.005,suffix:m/s"), "set_drift_speed", "get_drift_speed");
}

DustVolume3D::DustVolume3D() {
	// One shared soft-dot texture for every mote in every volume.
	if (mote_texture.is_null()) {
		const int texture_size = 64;
		Ref<Image> image = Image::create_empty(texture_size, texture_size, false, Image::FORMAT_RGBA8);
		Vector2 center = Vector2(texture_size - 1, texture_size - 1) * 0.5;
		for (int y = 0; y < texture_size; y++) {
			for (int x = 0; x < texture_size; x++) {
				real_t distance = Vector2(x, y).distance_to(center) / (texture_size * 0.5);
				real_t alpha = CLAMP(1.0 - distance, 0.0, 1.0);
				alpha = alpha * alpha * (3.0 - 2.0 * alpha);
				image->set_pixel(x, y, Color(1, 1, 1, alpha));
			}
		}
		mote_texture = ImageTexture::create_from_image(image);
	}

	process_material.instantiate();
	process_material->set_emission_shape(ParticleProcessMaterial::EMISSION_SHAPE_BOX);
	process_material->set_direction(Vector3(0, 1, 0));
	process_material->set_spread(180.0);
	process_material->set_gravity(Vector3());
	process_material->set_param_min(ParticleProcessMaterial::PARAM_DAMPING, 0.0);
	process_material->set_param_max(ParticleProcessMaterial::PARAM_DAMPING, 0.0);
	process_material->set_param_min(ParticleProcessMaterial::PARAM_SCALE, 0.7);
	process_material->set_param_max(ParticleProcessMaterial::PARAM_SCALE, 1.4);

	Ref<Gradient> gradient;
	gradient.instantiate();
	gradient->set_offsets(PackedFloat32Array({ 0.0, 0.25, 0.75, 1.0 }));
	gradient->set_colors(PackedColorArray({ Color(1, 1, 1, 0), Color(1, 1, 1), Color(1, 1, 1), Color(1, 1, 1, 0) }));
	fade_ramp.instantiate();
	fade_ramp->set_gradient(gradient);
	process_material->set_color_ramp(fade_ramp);

	mote_material.instantiate();
	mote_material->set_shading_mode(StandardMaterial3D::SHADING_MODE_UNSHADED);
	mote_material->set_transparency(StandardMaterial3D::TRANSPARENCY_ALPHA);
	mote_material->set_billboard_mode(StandardMaterial3D::BILLBOARD_ENABLED);
	mote_material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, mote_texture);
	mote_material->set_flag(StandardMaterial3D::FLAG_DISABLE_DEPTH_TEST, false);

	quad.instantiate();
	quad->set_material(mote_material);

	set_lifetime(14.0);
	set_pre_process_time(14.0); // The box starts full of dust, not filling up.
	set_randomness_ratio(0.85);
	set_use_local_coordinates(true);
	set_draw_pass_mesh(0, quad);
	set_process_material(process_material);
	_rebuild();
}

DustVolume3D::~DustVolume3D() {
}
