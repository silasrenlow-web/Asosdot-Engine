/**************************************************************************/
/*  acoustic_hearing_3d.h                                                 */
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

#include "scene/3d/node_3d.h"

class AcousticEvent;

// Asosdot Acoustic Engine: ears, for an enemy or anything else that reacts to what
// it hears. Hears the sounds given to AcousticEngine.emit_sound() that reach it,
// through walls and around corners as the sound would.
class AcousticHearing3D : public Node3D {
	GDCLASS(AcousticHearing3D, Node3D);

	float sensitivity = 1.0;
	float min_loudness = 0.02;
	PackedStringArray sound_types;
	bool hears_own_sounds = false;

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_sensitivity(float p_sensitivity);
	float get_sensitivity() const;

	void set_min_loudness(float p_loudness);
	float get_min_loudness() const;

	void set_sound_types(const PackedStringArray &p_types);
	PackedStringArray get_sound_types() const;

	void set_hears_own_sounds(bool p_hears);
	bool is_hearing_own_sounds() const;

	// Whether a sound is one this listens for at all, before how loud it is.
	bool listens_to(const Ref<AcousticEvent> &p_event) const;
};
