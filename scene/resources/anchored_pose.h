/**************************************************************************/
/*  anchored_pose.h                                                       */
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
#include "core/variant/dictionary.h"

// A pose of some bones, kept relative to an anchor. See AnchoredPoseModifier3D.
class AnchoredPose : public Resource {
	GDCLASS(AnchoredPose, Resource);
	OBJ_SAVE_TYPE(AnchoredPose);

	// Bone name -> Transform3D. A bone without a parent bone is stored relative to the anchor (unless it is
	// listed in body_bones), any other bone as its local pose.
	Dictionary bones;
	PackedStringArray body_bones;

protected:
	static void _bind_methods();

public:
	void set_bones(const Dictionary &p_bones);
	Dictionary get_bones() const;
	void set_body_bones(const PackedStringArray &p_bones);
	PackedStringArray get_body_bones() const;

	void set_bone(const StringName &p_bone, const Transform3D &p_transform);
	Transform3D get_bone(const StringName &p_bone) const;
	bool has_bone(const StringName &p_bone) const;
	void erase_bone(const StringName &p_bone);
	PackedStringArray get_bone_names() const;
	void clear();
};
