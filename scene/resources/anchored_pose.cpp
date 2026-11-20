/**************************************************************************/
/*  anchored_pose.cpp                                                     */
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

#include "anchored_pose.h"

#include "core/object/class_db.h"

void AnchoredPose::set_bones(const Dictionary &p_bones) {
	bones = p_bones.duplicate();
	emit_changed();
}

Dictionary AnchoredPose::get_bones() const {
	return bones;
}

void AnchoredPose::set_body_bones(const PackedStringArray &p_bones) {
	body_bones = p_bones;
	emit_changed();
}

PackedStringArray AnchoredPose::get_body_bones() const {
	return body_bones;
}

void AnchoredPose::set_bone(const StringName &p_bone, const Transform3D &p_transform) {
	bones[String(p_bone)] = p_transform;
	emit_changed();
}

Transform3D AnchoredPose::get_bone(const StringName &p_bone) const {
	ERR_FAIL_COND_V_MSG(!bones.has(String(p_bone)), Transform3D(), vformat("Bone '%s' is not in the pose.", p_bone));
	return bones[String(p_bone)];
}

bool AnchoredPose::has_bone(const StringName &p_bone) const {
	return bones.has(String(p_bone));
}

void AnchoredPose::erase_bone(const StringName &p_bone) {
	bones.erase(String(p_bone));
	emit_changed();
}

PackedStringArray AnchoredPose::get_bone_names() const {
	PackedStringArray names;
	for (const Variant &key : bones.get_key_list()) {
		names.push_back(key);
	}
	return names;
}

void AnchoredPose::clear() {
	bones.clear();
	emit_changed();
}

void AnchoredPose::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_bones", "bones"), &AnchoredPose::set_bones);
	ClassDB::bind_method(D_METHOD("get_bones"), &AnchoredPose::get_bones);
	ClassDB::bind_method(D_METHOD("set_body_bones", "bones"), &AnchoredPose::set_body_bones);
	ClassDB::bind_method(D_METHOD("get_body_bones"), &AnchoredPose::get_body_bones);

	ClassDB::bind_method(D_METHOD("set_bone", "bone", "transform"), &AnchoredPose::set_bone);
	ClassDB::bind_method(D_METHOD("get_bone", "bone"), &AnchoredPose::get_bone);
	ClassDB::bind_method(D_METHOD("has_bone", "bone"), &AnchoredPose::has_bone);
	ClassDB::bind_method(D_METHOD("erase_bone", "bone"), &AnchoredPose::erase_bone);
	ClassDB::bind_method(D_METHOD("get_bone_names"), &AnchoredPose::get_bone_names);
	ClassDB::bind_method(D_METHOD("clear"), &AnchoredPose::clear);

	ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY, "bones", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE), "set_bones", "get_bones");
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_STRING_ARRAY, "body_bones"), "set_body_bones", "get_body_bones");
}
