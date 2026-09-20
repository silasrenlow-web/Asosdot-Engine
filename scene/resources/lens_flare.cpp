/**************************************************************************/
/*  lens_flare.cpp                                                        */
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

#include "lens_flare.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"

void LensFlareElement::set_type(Type p_type) {
	ERR_FAIL_INDEX(p_type, TYPE_MAX);
	if (type == p_type) {
		return;
	}
	type = p_type;
	notify_property_list_changed();
	emit_changed();
}

LensFlareElement::Type LensFlareElement::get_type() const {
	return type;
}

void LensFlareElement::set_texture(const Ref<Texture2D> &p_texture) {
	if (texture == p_texture) {
		return;
	}
	texture = p_texture;
	emit_changed();
}

Ref<Texture2D> LensFlareElement::get_texture() const {
	return texture;
}

void LensFlareElement::set_sides(int p_sides) {
	p_sides = CLAMP(p_sides, 3, 32);
	if (sides == p_sides) {
		return;
	}
	sides = p_sides;
	emit_changed();
}

int LensFlareElement::get_sides() const {
	return sides;
}

void LensFlareElement::set_falloff(float p_falloff) {
	p_falloff = CLAMP(p_falloff, 0.0f, 1.0f);
	if (falloff == p_falloff) {
		return;
	}
	falloff = p_falloff;
	emit_changed();
}

float LensFlareElement::get_falloff() const {
	return falloff;
}

void LensFlareElement::set_chromatic_aberration(float p_aberration) {
	p_aberration = CLAMP(p_aberration, 0.0f, 0.9f);
	if (chromatic_aberration == p_aberration) {
		return;
	}
	chromatic_aberration = p_aberration;
	emit_changed();
}

float LensFlareElement::get_chromatic_aberration() const {
	return chromatic_aberration;
}

void LensFlareElement::set_position(float p_position) {
	if (position == p_position) {
		return;
	}
	position = p_position;
	emit_changed();
}

float LensFlareElement::get_position() const {
	return position;
}

void LensFlareElement::set_size(const Vector2 &p_size) {
	const Vector2 clamped = p_size.maxf(0.0);
	if (size == clamped) {
		return;
	}
	size = clamped;
	emit_changed();
}

Vector2 LensFlareElement::get_size() const {
	return size;
}

void LensFlareElement::set_rotation(float p_radians) {
	if (rotation == p_radians) {
		return;
	}
	rotation = p_radians;
	emit_changed();
}

float LensFlareElement::get_rotation() const {
	return rotation;
}

void LensFlareElement::set_auto_rotate(bool p_enabled) {
	if (auto_rotate == p_enabled) {
		return;
	}
	auto_rotate = p_enabled;
	emit_changed();
}

bool LensFlareElement::is_auto_rotating() const {
	return auto_rotate;
}

void LensFlareElement::set_count(int p_count) {
	p_count = MAX(p_count, 1);
	if (count == p_count) {
		return;
	}
	count = p_count;
	notify_property_list_changed();
	emit_changed();
}

int LensFlareElement::get_count() const {
	return count;
}

void LensFlareElement::set_spacing(float p_spacing) {
	if (spacing == p_spacing) {
		return;
	}
	spacing = p_spacing;
	emit_changed();
}

float LensFlareElement::get_spacing() const {
	return spacing;
}

void LensFlareElement::set_color(const Color &p_color) {
	if (color == p_color) {
		return;
	}
	color = p_color;
	emit_changed();
}

Color LensFlareElement::get_color() const {
	return color;
}

void LensFlareElement::set_intensity(float p_intensity) {
	p_intensity = MAX(p_intensity, 0.0f);
	if (intensity == p_intensity) {
		return;
	}
	intensity = p_intensity;
	emit_changed();
}

float LensFlareElement::get_intensity() const {
	return intensity;
}

void LensFlareElement::set_blend_mode(BlendMode p_mode) {
	ERR_FAIL_INDEX(p_mode, BLEND_MODE_MAX);
	if (blend_mode == p_mode) {
		return;
	}
	blend_mode = p_mode;
	emit_changed();
}

LensFlareElement::BlendMode LensFlareElement::get_blend_mode() const {
	return blend_mode;
}

void LensFlareElement::set_tint_by_source(bool p_enabled) {
	if (tint_by_source == p_enabled) {
		return;
	}
	tint_by_source = p_enabled;
	emit_changed();
}

bool LensFlareElement::is_tinted_by_source() const {
	return tint_by_source;
}

void LensFlareElement::_validate_property(PropertyInfo &p_property) const {
	// Hidden with NO_EDITOR rather than NONE: NONE drops storage too, so setting
	// a texture, switching the type away and back would silently lose it.
	if (p_property.name == "texture" && type != TYPE_TEXTURE) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (p_property.name == "sides" && type != TYPE_POLYGON && type != TYPE_STARBURST) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	// Only the analytic shapes have a profile to shape; a texture has whatever
	// profile it was drawn with.
	if (p_property.name == "falloff" && type >= TYPE_TEXTURE) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (p_property.name == "spacing" && count <= 1) {
		p_property.usage |= PROPERTY_USAGE_READ_ONLY;
	}
}

void LensFlareElement::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_type", "type"), &LensFlareElement::set_type);
	ClassDB::bind_method(D_METHOD("get_type"), &LensFlareElement::get_type);
	ClassDB::bind_method(D_METHOD("set_texture", "texture"), &LensFlareElement::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture"), &LensFlareElement::get_texture);
	ClassDB::bind_method(D_METHOD("set_sides", "sides"), &LensFlareElement::set_sides);
	ClassDB::bind_method(D_METHOD("get_sides"), &LensFlareElement::get_sides);
	ClassDB::bind_method(D_METHOD("set_falloff", "falloff"), &LensFlareElement::set_falloff);
	ClassDB::bind_method(D_METHOD("get_falloff"), &LensFlareElement::get_falloff);
	ClassDB::bind_method(D_METHOD("set_chromatic_aberration", "aberration"), &LensFlareElement::set_chromatic_aberration);
	ClassDB::bind_method(D_METHOD("get_chromatic_aberration"), &LensFlareElement::get_chromatic_aberration);

	ClassDB::bind_method(D_METHOD("set_position", "position"), &LensFlareElement::set_position);
	ClassDB::bind_method(D_METHOD("get_position"), &LensFlareElement::get_position);
	ClassDB::bind_method(D_METHOD("set_size", "size"), &LensFlareElement::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &LensFlareElement::get_size);
	ClassDB::bind_method(D_METHOD("set_rotation", "radians"), &LensFlareElement::set_rotation);
	ClassDB::bind_method(D_METHOD("get_rotation"), &LensFlareElement::get_rotation);
	ClassDB::bind_method(D_METHOD("set_auto_rotate", "enabled"), &LensFlareElement::set_auto_rotate);
	ClassDB::bind_method(D_METHOD("is_auto_rotating"), &LensFlareElement::is_auto_rotating);
	ClassDB::bind_method(D_METHOD("set_count", "count"), &LensFlareElement::set_count);
	ClassDB::bind_method(D_METHOD("get_count"), &LensFlareElement::get_count);
	ClassDB::bind_method(D_METHOD("set_spacing", "spacing"), &LensFlareElement::set_spacing);
	ClassDB::bind_method(D_METHOD("get_spacing"), &LensFlareElement::get_spacing);

	ClassDB::bind_method(D_METHOD("set_color", "color"), &LensFlareElement::set_color);
	ClassDB::bind_method(D_METHOD("get_color"), &LensFlareElement::get_color);
	ClassDB::bind_method(D_METHOD("set_intensity", "intensity"), &LensFlareElement::set_intensity);
	ClassDB::bind_method(D_METHOD("get_intensity"), &LensFlareElement::get_intensity);
	ClassDB::bind_method(D_METHOD("set_blend_mode", "mode"), &LensFlareElement::set_blend_mode);
	ClassDB::bind_method(D_METHOD("get_blend_mode"), &LensFlareElement::get_blend_mode);
	ClassDB::bind_method(D_METHOD("set_tint_by_source", "enabled"), &LensFlareElement::set_tint_by_source);
	ClassDB::bind_method(D_METHOD("is_tinted_by_source"), &LensFlareElement::is_tinted_by_source);

	ADD_GROUP("Shape", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "type", PROPERTY_HINT_ENUM, "Glow,Disc,Ring,Polygon,Streak,Starburst,Texture,Corona (Built-in),Nova (Built-in),Glow (Built-in),Iris (Built-in),Hexagon (Built-in),Hexagon Ring (Built-in),Disc (Built-in),Halo (Built-in),Streak (Built-in)"), "set_type", "get_type");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "texture", PROPERTY_HINT_RESOURCE_TYPE, "Texture2D"), "set_texture", "get_texture");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "sides", PROPERTY_HINT_RANGE, "3,32,1"), "set_sides", "get_sides");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "falloff", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_falloff", "get_falloff");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "chromatic_aberration", PROPERTY_HINT_RANGE, "0,0.9,0.001"), "set_chromatic_aberration", "get_chromatic_aberration");

	ADD_GROUP("Layout", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "position", PROPERTY_HINT_RANGE, "-2,3,0.001,or_less,or_greater"), "set_position", "get_position");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size", PROPERTY_HINT_RANGE, "0,2,0.001,or_greater"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rotation", PROPERTY_HINT_RANGE, "-360,360,0.1,radians_as_degrees"), "set_rotation", "get_rotation");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "auto_rotate"), "set_auto_rotate", "is_auto_rotating");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "count", PROPERTY_HINT_RANGE, "1,32,1"), "set_count", "get_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "spacing", PROPERTY_HINT_RANGE, "-1,1,0.001,or_less,or_greater"), "set_spacing", "get_spacing");

	ADD_GROUP("Shading", "");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_color", "get_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "intensity", PROPERTY_HINT_RANGE, "0,16,0.001,or_greater"), "set_intensity", "get_intensity");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "blend_mode", PROPERTY_HINT_ENUM, "Add,Screen"), "set_blend_mode", "get_blend_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "tint_by_source"), "set_tint_by_source", "is_tinted_by_source");

	BIND_ENUM_CONSTANT(TYPE_GLOW);
	BIND_ENUM_CONSTANT(TYPE_DISC);
	BIND_ENUM_CONSTANT(TYPE_RING);
	BIND_ENUM_CONSTANT(TYPE_POLYGON);
	BIND_ENUM_CONSTANT(TYPE_STREAK);
	BIND_ENUM_CONSTANT(TYPE_STARBURST);
	BIND_ENUM_CONSTANT(TYPE_TEXTURE);
	BIND_ENUM_CONSTANT(TYPE_BUILTIN_CORONA);
	BIND_ENUM_CONSTANT(TYPE_BUILTIN_NOVA);
	BIND_ENUM_CONSTANT(TYPE_BUILTIN_GLOW);
	BIND_ENUM_CONSTANT(TYPE_BUILTIN_IRIS);
	BIND_ENUM_CONSTANT(TYPE_BUILTIN_HEXAGON);
	BIND_ENUM_CONSTANT(TYPE_BUILTIN_HEXAGON_RING);
	BIND_ENUM_CONSTANT(TYPE_BUILTIN_DISC);
	BIND_ENUM_CONSTANT(TYPE_BUILTIN_HALO);
	BIND_ENUM_CONSTANT(TYPE_BUILTIN_STREAK);
	BIND_ENUM_CONSTANT(TYPE_MAX);

	BIND_ENUM_CONSTANT(BLEND_MODE_ADD);
	BIND_ENUM_CONSTANT(BLEND_MODE_SCREEN);
	BIND_ENUM_CONSTANT(BLEND_MODE_MAX);
}

/* LensFlare */

void LensFlare::_element_changed() {
	emit_changed();
}

void LensFlare::set_elements(const TypedArray<LensFlareElement> &p_elements) {
	for (const Ref<LensFlareElement> &element : elements) {
		if (element.is_valid()) {
			element->disconnect_changed(callable_mp(this, &LensFlare::_element_changed));
		}
	}

	elements.resize(p_elements.size());
	for (int i = 0; i < p_elements.size(); i++) {
		Ref<LensFlareElement> element = p_elements[i];
		elements.write[i] = element;
		if (element.is_valid()) {
			element->connect_changed(callable_mp(this, &LensFlare::_element_changed));
		}
	}

	emit_changed();
}

TypedArray<LensFlareElement> LensFlare::get_elements() const {
	TypedArray<LensFlareElement> result;
	result.resize(elements.size());
	for (int i = 0; i < elements.size(); i++) {
		result[i] = elements[i];
	}
	return result;
}

int LensFlare::get_element_count() const {
	return elements.size();
}

void LensFlare::set_element(int p_index, const Ref<LensFlareElement> &p_element) {
	ERR_FAIL_INDEX(p_index, elements.size());

	if (elements[p_index].is_valid()) {
		elements[p_index]->disconnect_changed(callable_mp(this, &LensFlare::_element_changed));
	}

	elements.write[p_index] = p_element;

	if (p_element.is_valid()) {
		p_element->connect_changed(callable_mp(this, &LensFlare::_element_changed));
	}

	emit_changed();
}

Ref<LensFlareElement> LensFlare::get_element(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, elements.size(), Ref<LensFlareElement>());
	return elements[p_index];
}

void LensFlare::add_element(const Ref<LensFlareElement> &p_element) {
	elements.push_back(p_element);
	if (p_element.is_valid()) {
		p_element->connect_changed(callable_mp(this, &LensFlare::_element_changed));
	}
	emit_changed();
}

void LensFlare::remove_element(int p_index) {
	ERR_FAIL_INDEX(p_index, elements.size());

	if (elements[p_index].is_valid()) {
		elements[p_index]->disconnect_changed(callable_mp(this, &LensFlare::_element_changed));
	}

	elements.remove_at(p_index);
	emit_changed();
}

Ref<LensFlare> LensFlare::create_default() {
	struct Preset {
		LensFlareElement::Type type;
		float position;
		Vector2 size;
		float intensity;
		Color color;
		float falloff;
		int sides;
		int count;
		float spacing;
		float aberration;
		bool auto_rotate;
	};

	// Positions run from 0 at the source to 1 at the centre of the screen, so the
	// ghosts past 1 are the ones that swing out the far side as the source moves.
	// Built from the bundled CC0 textures rather than the analytic shapes: a
	// photographed flare has grain and asymmetry that a formula does not, and
	// these cost nothing in a saved scene because the element stores the type,
	// not the image. Swap any of them for TYPE_GLOW and friends if you would
	// rather have the resolution independent version.
	static const Preset presets[] = {
		{ LensFlareElement::TYPE_BUILTIN_CORONA, 0.0, Vector2(0.38, 0.38), 1.6, Color(1.0, 0.97, 0.9), 0.5, 6, 1, 0.0, 0.0, false },
		{ LensFlareElement::TYPE_BUILTIN_NOVA, 0.0, Vector2(0.72, 0.72), 0.45, Color(1.0, 0.95, 0.85), 0.5, 6, 1, 0.0, 0.0, false },
		{ LensFlareElement::TYPE_BUILTIN_STREAK, 0.0, Vector2(1.7, 0.16), 0.3, Color(0.55, 0.72, 1.0), 0.5, 6, 1, 0.0, 0.0, true },
		{ LensFlareElement::TYPE_BUILTIN_HALO, 0.95, Vector2(0.46, 0.46), 0.16, Color(0.8, 0.9, 1.0), 0.5, 6, 1, 0.0, 0.04, false },
		{ LensFlareElement::TYPE_BUILTIN_HEXAGON, 0.42, Vector2(0.1, 0.1), 0.16, Color(1.0, 0.8, 0.6), 0.5, 6, 1, 0.0, 0.08, true },
		{ LensFlareElement::TYPE_BUILTIN_HEXAGON_RING, 0.68, Vector2(0.06, 0.06), 0.12, Color(0.7, 1.0, 0.8), 0.5, 6, 4, 0.19, 0.06, true },
		{ LensFlareElement::TYPE_BUILTIN_IRIS, 1.35, Vector2(0.16, 0.16), 0.1, Color(0.6, 0.75, 1.0), 0.5, 6, 1, 0.0, 0.15, false },
		{ LensFlareElement::TYPE_BUILTIN_DISC, 1.75, Vector2(0.08, 0.08), 0.14, Color(1.0, 0.65, 0.5), 0.5, 6, 1, 0.0, 0.1, false },
	};

	Ref<LensFlare> result;
	result.instantiate();

	for (const Preset &preset : presets) {
		Ref<LensFlareElement> element;
		element.instantiate();
		element->set_type(preset.type);
		element->set_position(preset.position);
		element->set_size(preset.size);
		element->set_intensity(preset.intensity);
		element->set_color(preset.color);
		element->set_falloff(preset.falloff);
		element->set_sides(preset.sides);
		element->set_count(preset.count);
		element->set_spacing(preset.spacing);
		element->set_chromatic_aberration(preset.aberration);
		element->set_auto_rotate(preset.auto_rotate);
		result->add_element(element);
	}

	return result;
}

LensFlare::~LensFlare() {
	for (const Ref<LensFlareElement> &element : elements) {
		if (element.is_valid()) {
			element->disconnect_changed(callable_mp(this, &LensFlare::_element_changed));
		}
	}
}

void LensFlare::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_elements", "elements"), &LensFlare::set_elements);
	ClassDB::bind_method(D_METHOD("get_elements"), &LensFlare::get_elements);
	ClassDB::bind_method(D_METHOD("get_element_count"), &LensFlare::get_element_count);
	ClassDB::bind_method(D_METHOD("set_element", "index", "element"), &LensFlare::set_element);
	ClassDB::bind_method(D_METHOD("get_element", "index"), &LensFlare::get_element);
	ClassDB::bind_method(D_METHOD("add_element", "element"), &LensFlare::add_element);
	ClassDB::bind_method(D_METHOD("remove_element", "index"), &LensFlare::remove_element);
	ClassDB::bind_static_method("LensFlare", D_METHOD("create_default"), &LensFlare::create_default);

	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "elements", PROPERTY_HINT_ARRAY_TYPE, MAKE_RESOURCE_TYPE_HINT("LensFlareElement")), "set_elements", "get_elements");
}
