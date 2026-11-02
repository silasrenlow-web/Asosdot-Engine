/**************************************************************************/
/*  decal.cpp                                                             */
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

#include "decal.h"

#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "scene/animation/tween.h"
#include "scene/main/scene_tree.h"
#include "servers/rendering/rendering_server.h"

void Decal::set_size(const Vector3 &p_size) {
	size = p_size.maxf(0.001);
	RS::get_singleton()->decal_set_size(decal, size);
	update_gizmos();
}

Vector3 Decal::get_size() const {
	return size;
}

void Decal::set_texture(DecalTexture p_type, const Ref<Texture2D> &p_texture) {
	ERR_FAIL_INDEX(p_type, TEXTURE_MAX);
	textures[p_type] = p_texture;
	RID texture_rid = p_texture.is_valid() ? p_texture->get_rid() : RID();

#ifdef DEBUG_ENABLED
	if (p_texture.is_valid() &&
			(p_texture->is_class("AnimatedTexture") ||
					p_texture->is_class("AtlasTexture") ||
					p_texture->is_class("CameraTexture") ||
					p_texture->is_class("CanvasTexture") ||
					p_texture->is_class("MeshTexture") ||
					p_texture->is_class("Texture2DRD") ||
					p_texture->is_class("ViewportTexture"))) {
		WARN_PRINT(vformat("%s cannot be used as a Decal texture (%s). As a workaround, assign the value returned by %s's `get_image()` instead.", p_texture->get_class(), get_path(), p_texture->get_class()));
	}
#endif

	RS::get_singleton()->decal_set_texture(decal, RSE::DecalTexture(p_type), texture_rid);
	update_configuration_warnings();
}

Ref<Texture2D> Decal::get_texture(DecalTexture p_type) const {
	ERR_FAIL_INDEX_V(p_type, TEXTURE_MAX, Ref<Texture2D>());
	return textures[p_type];
}

void Decal::set_emission_energy(real_t p_energy) {
	emission_energy = p_energy;
	RS::get_singleton()->decal_set_emission_energy(decal, emission_energy);
}

real_t Decal::get_emission_energy() const {
	return emission_energy;
}

void Decal::set_albedo_mix(real_t p_mix) {
	albedo_mix = p_mix;
	RS::get_singleton()->decal_set_albedo_mix(decal, albedo_mix);
}

real_t Decal::get_albedo_mix() const {
	return albedo_mix;
}

void Decal::set_upper_fade(real_t p_fade) {
	upper_fade = MAX(p_fade, 0.0);
	RS::get_singleton()->decal_set_fade(decal, upper_fade, lower_fade);
}

real_t Decal::get_upper_fade() const {
	return upper_fade;
}

void Decal::set_lower_fade(real_t p_fade) {
	lower_fade = MAX(p_fade, 0.0);
	RS::get_singleton()->decal_set_fade(decal, upper_fade, lower_fade);
}

real_t Decal::get_lower_fade() const {
	return lower_fade;
}

void Decal::set_normal_fade(real_t p_fade) {
	normal_fade = p_fade;
	RS::get_singleton()->decal_set_normal_fade(decal, normal_fade);
}

real_t Decal::get_normal_fade() const {
	return normal_fade;
}

void Decal::set_modulate(Color p_modulate) {
	modulate = p_modulate;
	RS::get_singleton()->decal_set_modulate(decal, p_modulate);
}

Color Decal::get_modulate() const {
	return modulate;
}

void Decal::set_enable_distance_fade(bool p_enable) {
	distance_fade_enabled = p_enable;
	RS::get_singleton()->decal_set_distance_fade(decal, distance_fade_enabled, distance_fade_begin, distance_fade_length);
	notify_property_list_changed();
}

bool Decal::is_distance_fade_enabled() const {
	return distance_fade_enabled;
}

void Decal::set_distance_fade_begin(real_t p_distance) {
	distance_fade_begin = p_distance;
	RS::get_singleton()->decal_set_distance_fade(decal, distance_fade_enabled, distance_fade_begin, distance_fade_length);
}

real_t Decal::get_distance_fade_begin() const {
	return distance_fade_begin;
}

void Decal::set_distance_fade_length(real_t p_length) {
	distance_fade_length = p_length;
	RS::get_singleton()->decal_set_distance_fade(decal, distance_fade_enabled, distance_fade_begin, distance_fade_length);
}

real_t Decal::get_distance_fade_length() const {
	return distance_fade_length;
}

void Decal::set_cull_mask(uint32_t p_layers) {
	cull_mask = p_layers;
	RS::get_singleton()->decal_set_cull_mask(decal, cull_mask);
	update_configuration_warnings();
}

uint32_t Decal::get_cull_mask() const {
	return cull_mask;
}

// What a node stands for when the decal is limited to it. A visible thing is
// itself and whatever hangs under it, so naming an object names all its parts; a
// body that was named for its collider stands for the mesh it belongs to, which is
// the parent or a sibling.
void Decal::_collect_visuals(Node *p_node, LocalVector<VisualInstance3D *> &r_visuals) const {
	if (!p_node) {
		return;
	}
	if (VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(p_node)) {
		if (visual != this && !r_visuals.has(visual)) {
			r_visuals.push_back(visual);
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		_collect_visuals(p_node->get_child(i), r_visuals);
	}
}

void Decal::_collect_affected(LocalVector<VisualInstance3D *> &r_visuals) const {
	LocalVector<Node *> named;
	if (is_inside_tree()) {
		for (const NodePath &path : affected_paths) {
			if (Node *node = get_node_or_null(path)) {
				named.push_back(node);
			}
		}
	}
	for (const ObjectID &id : affected_objects) {
		if (Node *node = ObjectDB::get_instance<Node>(id)) {
			named.push_back(node);
		}
	}

	for (Node *node : named) {
		const uint32_t before = r_visuals.size();
		_collect_visuals(node, r_visuals);
		if (r_visuals.size() != before) {
			continue;
		}

		// Nothing visible in it: a body whose mesh is its parent, or whose mesh sits
		// beside it.
		Node *parent = node->get_parent();
		if (VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(parent)) {
			_collect_visuals(visual, r_visuals);
		} else if (parent) {
			for (int i = 0; i < parent->get_child_count(); i++) {
				if (Object::cast_to<VisualInstance3D>(parent->get_child(i))) {
					_collect_visuals(parent->get_child(i), r_visuals);
				}
			}
		}
	}
}

void Decal::_update_affected() {
	if (affected_paths.is_empty() && affected_objects.is_empty()) {
		RS::get_singleton()->instance_set_decal_filter(get_instance(), TypedArray<RID>());
		return;
	}

	LocalVector<VisualInstance3D *> visuals;
	_collect_affected(visuals);

	TypedArray<RID> instances;
	for (VisualInstance3D *visual : visuals) {
		instances.push_back(visual->get_instance());
	}
	// Even when none can be found yet: a decal limited to something that is not
	// there affects nothing, rather than everything. An empty list means no limit,
	// so a place that holds nothing is what says it.
	if (instances.is_empty()) {
		instances.push_back(RID());
	}
	RS::get_singleton()->instance_set_decal_filter(get_instance(), instances);
	update_configuration_warnings();
}

void Decal::set_affected_nodes(const TypedArray<NodePath> &p_paths) {
	affected_paths.clear();
	for (int i = 0; i < p_paths.size(); i++) {
		affected_paths.push_back(p_paths[i]);
	}
	_update_affected();
	update_configuration_warnings();
	notify_property_list_changed();
}

TypedArray<NodePath> Decal::get_affected_nodes() const {
	TypedArray<NodePath> paths;
	for (const NodePath &path : affected_paths) {
		paths.push_back(path);
	}
	return paths;
}

void Decal::add_affected_node(Node *p_node) {
	ERR_FAIL_NULL(p_node);
	ERR_FAIL_COND_MSG(p_node == this, "A decal can't be limited to itself.");

	if (is_inside_tree() && p_node->is_inside_tree()) {
		// A path survives the scene being saved and the node being re-added.
		const NodePath path = get_path_to(p_node);
		if (!affected_paths.has(path)) {
			affected_paths.push_back(path);
		}
	} else if (!affected_objects.has(p_node->get_instance_id())) {
		affected_objects.push_back(p_node->get_instance_id());
	}
	_update_affected();
	notify_property_list_changed();
}

void Decal::remove_affected_node(Node *p_node) {
	ERR_FAIL_NULL(p_node);
	for (int i = affected_paths.size() - 1; i >= 0; i--) {
		if (is_inside_tree() && get_node_or_null(affected_paths[i]) == p_node) {
			affected_paths.remove_at(i);
		}
	}
	affected_objects.erase(p_node->get_instance_id());
	_update_affected();
	notify_property_list_changed();
}

void Decal::clear_affected_nodes() {
	affected_paths.clear();
	affected_objects.clear();
	_update_affected();
	notify_property_list_changed();
}

TypedArray<VisualInstance3D> Decal::get_affected_visuals() const {
	LocalVector<VisualInstance3D *> visuals;
	_collect_affected(visuals);
	TypedArray<VisualInstance3D> result;
	for (VisualInstance3D *visual : visuals) {
		result.push_back(visual);
	}
	return result;
}

void Decal::refresh_affected_nodes() {
	_update_affected();
}

void Decal::fit_to_texture(real_t p_width) {
	// What gives a decal its shape is the first texture it has.
	Ref<Texture2D> texture = textures[TEXTURE_ALBEDO];
	for (int i = 0; i < TEXTURE_MAX && texture.is_null(); i++) {
		texture = textures[i];
	}
	ERR_FAIL_COND_MSG(texture.is_null(), "The decal has no texture to take its shape from.");
	ERR_FAIL_COND(texture->get_width() <= 0 || texture->get_height() <= 0);

	const real_t width = p_width > 0.0 ? p_width : size.x;
	// The texture runs along the decal's X and Z.
	set_size(Vector3(width, size.y, width * real_t(texture->get_height()) / real_t(texture->get_width())));
}

Decal *Decal::create_at(Node *p_parent, const Ref<Texture2D> &p_texture, const Vector3 &p_position, const Vector3 &p_normal, real_t p_size, Node *p_target, real_t p_lifetime, real_t p_rotation_degrees) {
	ERR_FAIL_NULL_V(p_parent, nullptr);
	ERR_FAIL_COND_V_MSG(p_texture.is_null(), nullptr, "A decal needs a texture.");

	Decal *decal = memnew(Decal);
	decal->set_texture(TEXTURE_ALBEDO, p_texture);

	const real_t width = MAX(p_size, real_t(0.001));
	const real_t aspect = p_texture->get_width() > 0 ? real_t(p_texture->get_height()) / real_t(p_texture->get_width()) : 1.0;
	// Thick enough to reach a surface that is not quite flat, thin enough not to
	// reach the next one.
	decal->set_size(Vector3(width, MAX(width * 0.5, real_t(0.1)), width * aspect));
	// Not on a surface that turns away from it, so a decal on a wall stops at the corner.
	decal->set_normal_fade(0.3);

	if (p_target) {
		decal->add_affected_node(p_target);
	}

	p_parent->add_child(decal);

	// The decal's Y is the way it projects, so that is the surface's normal.
	Vector3 up = p_normal.is_zero_approx() ? Vector3(0, 1, 0) : p_normal.normalized();
	Vector3 reference = Math::abs(up.y) < 0.99 ? Vector3(0, 1, 0) : Vector3(1, 0, 0);
	const Vector3 x = reference.cross(up).normalized();
	const Vector3 z = x.cross(up).normalized();
	Basis basis(x, up, z);
	basis = Basis(up, Math::deg_to_rad(p_rotation_degrees)) * basis;
	decal->set_global_transform(Transform3D(basis, p_position));

	if (p_lifetime > 0.0) {
		const double fade = MIN(1.0, double(p_lifetime) * 0.5);
		Ref<Tween> tween = decal->create_tween();
		tween->tween_interval(double(p_lifetime) - fade);
		tween->tween_property(decal, NodePath("modulate:a"), 0.0, fade);
		tween->tween_callback(callable_mp((Node *)decal, &Node::queue_free));
	}
	return decal;
}

void Decal::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE:
		case NOTIFICATION_READY: {
			// Ready too: every node of the scene is in the tree by then, so a path to a
			// node that comes later in the scene leads somewhere.
			_update_affected();
		} break;
	}
}

AABB Decal::get_aabb() const {
	AABB aabb;
	aabb.position = -size / 2;
	aabb.size = size;
	return aabb;
}

void Decal::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "cull_mask" && (!affected_paths.is_empty() || !affected_objects.is_empty())) {
		// Limited to nodes: the layers are not looked at, so there is nothing to set.
		p_property.usage = PROPERTY_USAGE_STORAGE;
	}
	if (p_property.name == "sorting_offset") {
		p_property.usage = PROPERTY_USAGE_DEFAULT;
	}
}

PackedStringArray Decal::get_configuration_warnings() const {
	PackedStringArray warnings = VisualInstance3D::get_configuration_warnings();

	if (OS::get_singleton()->get_current_rendering_method() == "gl_compatibility" || OS::get_singleton()->get_current_rendering_method() == "dummy") {
		warnings.push_back(RTR("Decals are only available when using the Forward+ or Mobile renderer."));
		return warnings;
	}

	if (textures[TEXTURE_ALBEDO].is_null() && textures[TEXTURE_NORMAL].is_null() && textures[TEXTURE_ORM].is_null() && textures[TEXTURE_EMISSION].is_null()) {
		warnings.push_back(RTR("The decal has no textures loaded into any of its texture properties, and will therefore not be visible."));
	}

	if ((textures[TEXTURE_NORMAL].is_valid() || textures[TEXTURE_ORM].is_valid()) && textures[TEXTURE_ALBEDO].is_null()) {
		warnings.push_back(RTR("The decal has a Normal and/or ORM texture, but no Albedo texture is set.\nAn Albedo texture with an alpha channel is required to blend the normal/ORM maps onto the underlying surface.\nIf you don't want the Albedo texture to be visible, set Albedo Mix to 0."));
	}

	for (const NodePath &path : affected_paths) {
		if (is_inside_tree() && !get_node_or_null(path)) {
			warnings.push_back(vformat(RTR("The decal is limited to \"%s\", which is not there."), String(path)));
		}
	}
	if (is_inside_tree() && (!affected_paths.is_empty() || !affected_objects.is_empty())) {
		LocalVector<VisualInstance3D *> visuals;
		_collect_affected(visuals);
		if (visuals.is_empty()) {
			warnings.push_back(RTR("None of the nodes the decal is limited to has anything visible, so the decal will not be seen on anything."));
		}
	}

	if (cull_mask == 0 && affected_paths.is_empty() && affected_objects.is_empty()) {
		warnings.push_back(RTR("The decal's Cull Mask has no bits enabled, which means the decal will not paint objects on any layer.\nTo resolve this, enable at least one bit in the Cull Mask property."));
	}

	return warnings;
}

void Decal::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &Decal::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &Decal::get_size);

	ClassDB::bind_method(D_METHOD("set_texture", "type", "texture"), &Decal::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture", "type"), &Decal::get_texture);

	ClassDB::bind_method(D_METHOD("set_emission_energy", "energy"), &Decal::set_emission_energy);
	ClassDB::bind_method(D_METHOD("get_emission_energy"), &Decal::get_emission_energy);

	ClassDB::bind_method(D_METHOD("set_albedo_mix", "energy"), &Decal::set_albedo_mix);
	ClassDB::bind_method(D_METHOD("get_albedo_mix"), &Decal::get_albedo_mix);

	ClassDB::bind_method(D_METHOD("set_modulate", "color"), &Decal::set_modulate);
	ClassDB::bind_method(D_METHOD("get_modulate"), &Decal::get_modulate);

	ClassDB::bind_method(D_METHOD("set_upper_fade", "fade"), &Decal::set_upper_fade);
	ClassDB::bind_method(D_METHOD("get_upper_fade"), &Decal::get_upper_fade);

	ClassDB::bind_method(D_METHOD("set_lower_fade", "fade"), &Decal::set_lower_fade);
	ClassDB::bind_method(D_METHOD("get_lower_fade"), &Decal::get_lower_fade);

	ClassDB::bind_method(D_METHOD("set_normal_fade", "fade"), &Decal::set_normal_fade);
	ClassDB::bind_method(D_METHOD("get_normal_fade"), &Decal::get_normal_fade);

	ClassDB::bind_method(D_METHOD("set_enable_distance_fade", "enable"), &Decal::set_enable_distance_fade);
	ClassDB::bind_method(D_METHOD("is_distance_fade_enabled"), &Decal::is_distance_fade_enabled);

	ClassDB::bind_method(D_METHOD("set_distance_fade_begin", "distance"), &Decal::set_distance_fade_begin);
	ClassDB::bind_method(D_METHOD("get_distance_fade_begin"), &Decal::get_distance_fade_begin);

	ClassDB::bind_method(D_METHOD("set_distance_fade_length", "distance"), &Decal::set_distance_fade_length);
	ClassDB::bind_method(D_METHOD("get_distance_fade_length"), &Decal::get_distance_fade_length);

	ClassDB::bind_method(D_METHOD("set_cull_mask", "mask"), &Decal::set_cull_mask);
	ClassDB::bind_method(D_METHOD("get_cull_mask"), &Decal::get_cull_mask);

	ClassDB::bind_method(D_METHOD("set_affected_nodes", "paths"), &Decal::set_affected_nodes);
	ClassDB::bind_method(D_METHOD("get_affected_nodes"), &Decal::get_affected_nodes);
	ClassDB::bind_method(D_METHOD("add_affected_node", "node"), &Decal::add_affected_node);
	ClassDB::bind_method(D_METHOD("remove_affected_node", "node"), &Decal::remove_affected_node);
	ClassDB::bind_method(D_METHOD("clear_affected_nodes"), &Decal::clear_affected_nodes);
	ClassDB::bind_method(D_METHOD("get_affected_visuals"), &Decal::get_affected_visuals);
	ClassDB::bind_method(D_METHOD("refresh_affected_nodes"), &Decal::refresh_affected_nodes);
	ClassDB::bind_method(D_METHOD("fit_to_texture", "width"), &Decal::fit_to_texture, DEFVAL(0.0));
	ClassDB::bind_static_method("Decal", D_METHOD("create_at", "parent", "texture", "position", "normal", "size", "target", "lifetime", "rotation_degrees"), &Decal::create_at, DEFVAL(1.0), DEFVAL(Variant()), DEFVAL(0.0), DEFVAL(0.0));

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_RANGE, "0,1024,0.001,or_greater,suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "affected_nodes", PROPERTY_HINT_ARRAY_TYPE, "NodePath"), "set_affected_nodes", "get_affected_nodes");

	ADD_GROUP("Textures", "texture_");
	// Only allow texture types that display correctly.
	const String texture_hint = "Texture2D,-AnimatedTexture,-AtlasTexture,-CameraTexture,-CanvasTexture,-MeshTexture,-Texture2DRD,-ViewportTexture";
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "texture_albedo", PROPERTY_HINT_RESOURCE_TYPE, texture_hint), "set_texture", "get_texture", TEXTURE_ALBEDO);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "texture_normal", PROPERTY_HINT_RESOURCE_TYPE, texture_hint), "set_texture", "get_texture", TEXTURE_NORMAL);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "texture_orm", PROPERTY_HINT_RESOURCE_TYPE, texture_hint), "set_texture", "get_texture", TEXTURE_ORM);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "texture_emission", PROPERTY_HINT_RESOURCE_TYPE, texture_hint), "set_texture", "get_texture", TEXTURE_EMISSION);

	ADD_GROUP("Parameters", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission_energy", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_emission_energy", "get_emission_energy");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "modulate"), "set_modulate", "get_modulate");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "albedo_mix", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_albedo_mix", "get_albedo_mix");
	// A Normal Fade of 1.0 causes the decal to be invisible even if fully perpendicular to a surface.
	// Due to this, limit Normal Fade to 0.999.
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "normal_fade", PROPERTY_HINT_RANGE, "0,0.999,0.001"), "set_normal_fade", "get_normal_fade");

	ADD_GROUP("Vertical Fade", "");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "upper_fade", PROPERTY_HINT_EXP_EASING, "attenuation"), "set_upper_fade", "get_upper_fade");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lower_fade", PROPERTY_HINT_EXP_EASING, "attenuation"), "set_lower_fade", "get_lower_fade");

	ADD_GROUP("Distance Fade", "distance_fade_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "distance_fade_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_enable_distance_fade", "is_distance_fade_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "distance_fade_begin", PROPERTY_HINT_RANGE, "0.0,4096.0,0.01,or_greater,suffix:m"), "set_distance_fade_begin", "get_distance_fade_begin");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "distance_fade_length", PROPERTY_HINT_RANGE, "0.0,4096.0,0.01,or_greater,suffix:m"), "set_distance_fade_length", "get_distance_fade_length");

	ADD_GROUP("Cull Mask", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cull_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_cull_mask", "get_cull_mask");

	BIND_ENUM_CONSTANT(TEXTURE_ALBEDO);
	BIND_ENUM_CONSTANT(TEXTURE_NORMAL);
	BIND_ENUM_CONSTANT(TEXTURE_ORM);
	BIND_ENUM_CONSTANT(TEXTURE_EMISSION);
	BIND_ENUM_CONSTANT(TEXTURE_MAX);
}

#ifndef DISABLE_DEPRECATED
bool Decal::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		set_size((Vector3)p_value * 2);
		return true;
	}
	return false;
}

bool Decal::_get(const StringName &p_name, Variant &r_property) const {
	if (p_name == "extents") { // Compatibility with Godot 3.x.
		r_property = size / 2;
		return true;
	}
	return false;
}
#endif // DISABLE_DEPRECATED

Decal::Decal() {
	decal = RenderingServer::get_singleton()->decal_create();
	RS::get_singleton()->instance_set_base(get_instance(), decal);
}

Decal::~Decal() {
	ERR_FAIL_NULL(RenderingServer::get_singleton());
	RS::get_singleton()->free_rid(decal);
}
