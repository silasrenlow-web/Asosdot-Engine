/**************************************************************************/
/*  acoustic_event.cpp                                                    */
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

#include "acoustic_event.h"

#include "core/object/class_db.h"
#include "scene/main/node.h"

void AcousticEvent::set_source(Node *p_source) {
	source = p_source ? p_source->get_instance_id() : ObjectID();
}

Node *AcousticEvent::get_source() const {
	return ObjectDB::get_instance<Node>(source);
}

void AcousticEvent::set_position(const Vector3 &p_position) {
	position = p_position;
}

Vector3 AcousticEvent::get_position() const {
	return position;
}

void AcousticEvent::set_sound_type(const StringName &p_type) {
	sound_type = p_type;
}

StringName AcousticEvent::get_sound_type() const {
	return sound_type;
}

void AcousticEvent::set_intensity(float p_intensity) {
	intensity = MAX(p_intensity, 0.0f);
}

float AcousticEvent::get_intensity() const {
	return intensity;
}

void AcousticEvent::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_source", "source"), &AcousticEvent::set_source);
	ClassDB::bind_method(D_METHOD("get_source"), &AcousticEvent::get_source);
	ClassDB::bind_method(D_METHOD("set_position", "position"), &AcousticEvent::set_position);
	ClassDB::bind_method(D_METHOD("get_position"), &AcousticEvent::get_position);
	ClassDB::bind_method(D_METHOD("set_sound_type", "sound_type"), &AcousticEvent::set_sound_type);
	ClassDB::bind_method(D_METHOD("get_sound_type"), &AcousticEvent::get_sound_type);
	ClassDB::bind_method(D_METHOD("set_intensity", "intensity"), &AcousticEvent::set_intensity);
	ClassDB::bind_method(D_METHOD("get_intensity"), &AcousticEvent::get_intensity);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "source", PROPERTY_HINT_NODE_TYPE, "Node", PROPERTY_USAGE_NONE), "set_source", "get_source");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "position", PROPERTY_HINT_NONE, "suffix:m"), "set_position", "get_position");
	ADD_PROPERTY(PropertyInfo(Variant::STRING_NAME, "sound_type"), "set_sound_type", "get_sound_type");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "intensity", PROPERTY_HINT_RANGE, "0,4,0.01,or_greater"), "set_intensity", "get_intensity");
}
