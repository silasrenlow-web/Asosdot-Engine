/**************************************************************************/
/*  acoustic_material.h                                                   */
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

// Asosdot Acoustic Engine: how a surface treats sound that reaches it. Given to a
// collider's acoustic_material; colliders without one use the project's default.
class AcousticMaterial : public Resource {
	GDCLASS(AcousticMaterial, Resource);

	// Of the sound that hits it: how much is soaked up rather than reflected, how
	// much goes through to the other side, and how much of what is reflected is
	// spread in every direction rather than bounced like a mirror.
	float absorption = 0.1;
	float transmission = 0.05;
	float scattering = 0.3;

protected:
	static void _bind_methods();

public:
	void set_absorption(float p_absorption);
	float get_absorption() const;

	void set_transmission(float p_transmission);
	float get_transmission() const;

	void set_scattering(float p_scattering);
	float get_scattering() const;
};
