/**************************************************************************/
/*  lens_flare.h                                                          */
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
#include "core/variant/typed_array.h"
#include "scene/resources/texture.h"

// One drawn piece of a lens flare: a ghost, a halo, a streak, the glow around
// the source itself. Elements are resources so the same ghost can be shared
// between flares and edited in one place.
class LensFlareElement : public Resource {
	GDCLASS(LensFlareElement, Resource);

public:
	enum Type {
		TYPE_GLOW,
		TYPE_DISC,
		TYPE_RING,
		TYPE_POLYGON,
		TYPE_STREAK,
		TYPE_STARBURST,
		TYPE_TEXTURE,
		// Bundled textures. They behave exactly like TYPE_TEXTURE but the image
		// comes from the engine, so an element that uses one is just an int in a
		// saved scene rather than an embedded copy of the image.
		TYPE_BUILTIN_CORONA,
		TYPE_BUILTIN_NOVA,
		TYPE_BUILTIN_GLOW,
		TYPE_BUILTIN_IRIS,
		TYPE_BUILTIN_HEXAGON,
		TYPE_BUILTIN_HEXAGON_RING,
		TYPE_BUILTIN_DISC,
		TYPE_BUILTIN_HALO,
		TYPE_BUILTIN_STREAK,
		TYPE_MAX,
	};

	enum BlendMode {
		BLEND_MODE_ADD,
		BLEND_MODE_SCREEN,
		BLEND_MODE_MAX,
	};

private:
	Type type = TYPE_GLOW;
	Ref<Texture2D> texture;
	int sides = 6;
	float falloff = 0.5;
	float chromatic_aberration = 0.0;

	float position = 0.0;
	Vector2 size = Vector2(0.1, 0.1);
	float rotation = 0.0;
	bool auto_rotate = false;
	int count = 1;
	float spacing = 0.25;

	Color color = Color(1, 1, 1);
	float intensity = 1.0;
	BlendMode blend_mode = BLEND_MODE_ADD;
	bool tint_by_source = true;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	void set_type(Type p_type);
	Type get_type() const;

	void set_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_texture() const;

	void set_sides(int p_sides);
	int get_sides() const;

	void set_falloff(float p_falloff);
	float get_falloff() const;

	void set_chromatic_aberration(float p_aberration);
	float get_chromatic_aberration() const;

	void set_position(float p_position);
	float get_position() const;

	void set_size(const Vector2 &p_size);
	Vector2 get_size() const;

	void set_rotation(float p_radians);
	float get_rotation() const;

	void set_auto_rotate(bool p_enabled);
	bool is_auto_rotating() const;

	void set_count(int p_count);
	int get_count() const;

	void set_spacing(float p_spacing);
	float get_spacing() const;

	void set_color(const Color &p_color);
	Color get_color() const;

	void set_intensity(float p_intensity);
	float get_intensity() const;

	void set_blend_mode(BlendMode p_mode);
	BlendMode get_blend_mode() const;

	void set_tint_by_source(bool p_enabled);
	bool is_tinted_by_source() const;
};

VARIANT_ENUM_CAST(LensFlareElement::Type);
VARIANT_ENUM_CAST(LensFlareElement::BlendMode);

// The ordered list of elements that make up one flare. Assign it to a
// LensFlare3D, which supplies the source's position, colour and occlusion.
class LensFlare : public Resource {
	GDCLASS(LensFlare, Resource);

	Vector<Ref<LensFlareElement>> elements;

	void _element_changed();

protected:
	static void _bind_methods();

public:
	void set_elements(const TypedArray<LensFlareElement> &p_elements);
	TypedArray<LensFlareElement> get_elements() const;

	int get_element_count() const;

	void set_element(int p_index, const Ref<LensFlareElement> &p_element);
	Ref<LensFlareElement> get_element(int p_index) const;

	void add_element(const Ref<LensFlareElement> &p_element);
	void remove_element(int p_index);

	// Used by LensFlare3D to push the list at the rendering server without
	// building a TypedArray it would immediately throw away.
	const Vector<Ref<LensFlareElement>> &get_element_list() const { return elements; }

	// The classic eight element flare, built from the procedural shapes so it
	// needs no textures. A new LensFlare is empty, the way a new Curve3D is:
	// this is the shortcut to something that looks like a lens flare, and the
	// node's configuration warning points at it.
	static Ref<LensFlare> create_default();

	~LensFlare();
};
