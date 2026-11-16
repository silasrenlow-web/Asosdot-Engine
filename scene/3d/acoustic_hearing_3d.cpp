/**************************************************************************/
/*  acoustic_hearing_3d.cpp                                               */
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

#include "acoustic_hearing_3d.h"

#include "core/object/class_db.h"
#include "scene/3d/acoustic_engine.h"
#include "scene/3d/acoustic_event.h"

void AcousticHearing3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			if (AcousticEngine *engine = AcousticEngine::get_singleton()) {
				engine->register_hearing(this);
			}
		} break;
		case NOTIFICATION_EXIT_TREE: {
			if (AcousticEngine *engine = AcousticEngine::get_singleton()) {
				engine->unregister_hearing(this);
			}
		} break;
	}
}

void AcousticHearing3D::set_sensitivity(float p_sensitivity) {
	sensitivity = MAX(p_sensitivity, 0.0f);
}

float AcousticHearing3D::get_sensitivity() const {
	return sensitivity;
}

void AcousticHearing3D::set_min_loudness(float p_loudness) {
	min_loudness = CLAMP(p_loudness, 0.0f, 1.0f);
}

float AcousticHearing3D::get_min_loudness() const {
	return min_loudness;
}

void AcousticHearing3D::set_sound_types(const PackedStringArray &p_types) {
	sound_types = p_types;
}

PackedStringArray AcousticHearing3D::get_sound_types() const {
	return sound_types;
}

void AcousticHearing3D::set_hears_own_sounds(bool p_hears) {
	hears_own_sounds = p_hears;
}

bool AcousticHearing3D::is_hearing_own_sounds() const {
	return hears_own_sounds;
}

bool AcousticHearing3D::listens_to(const Ref<AcousticEvent> &p_event) const {
	if (p_event.is_null()) {
		return false;
	}
	if (!sound_types.is_empty() && !sound_types.has(String(p_event->get_sound_type()))) {
		return false;
	}
	if (!hears_own_sounds) {
		// Its own footsteps: made by the body these ears are on.
		const Node *source = p_event->get_source();
		if (source && (source == this || source->is_ancestor_of(this))) {
			return false;
		}
	}
	return true;
}

void AcousticHearing3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_sensitivity", "sensitivity"), &AcousticHearing3D::set_sensitivity);
	ClassDB::bind_method(D_METHOD("get_sensitivity"), &AcousticHearing3D::get_sensitivity);
	ClassDB::bind_method(D_METHOD("set_min_loudness", "loudness"), &AcousticHearing3D::set_min_loudness);
	ClassDB::bind_method(D_METHOD("get_min_loudness"), &AcousticHearing3D::get_min_loudness);
	ClassDB::bind_method(D_METHOD("set_sound_types", "types"), &AcousticHearing3D::set_sound_types);
	ClassDB::bind_method(D_METHOD("get_sound_types"), &AcousticHearing3D::get_sound_types);
	ClassDB::bind_method(D_METHOD("set_hears_own_sounds", "hears"), &AcousticHearing3D::set_hears_own_sounds);
	ClassDB::bind_method(D_METHOD("is_hearing_own_sounds"), &AcousticHearing3D::is_hearing_own_sounds);
	ClassDB::bind_method(D_METHOD("listens_to", "event"), &AcousticHearing3D::listens_to);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sensitivity", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_sensitivity", "get_sensitivity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "min_loudness", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_min_loudness", "get_min_loudness");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "sound_types"), "set_sound_types", "get_sound_types");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hears_own_sounds"), "set_hears_own_sounds", "is_hearing_own_sounds");

	ADD_SIGNAL(MethodInfo("sound_heard", PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "AcousticEvent"), PropertyInfo(Variant::FLOAT, "loudness"), PropertyInfo(Variant::VECTOR3, "from_position")));
}
