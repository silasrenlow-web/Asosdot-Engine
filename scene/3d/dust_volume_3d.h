/**************************************************************************/
/*  dust_volume_3d.h                                                      */
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

#include "scene/3d/gpu_particles_3d.h"

class ParticleProcessMaterial;
class QuadMesh;
class StandardMaterial3D;
class ImageTexture;
class GradientTexture1D;

// A box of air full of dust. DustVolume3D fills its size with slow-drifting
// motes that fade in and out over their lives, so the box reads as still,
// dusty air. It is a GPUParticles3D preconfigured end to end — emission box,
// soft round mote texture, zero gravity, gentle drift — so the only knobs
// that matter stay on this node: how big the box is, how thick the dust is,
// how large and bright the motes are, and how fast they drift.
//
// Place one inside a LightVolume3D shaft and the motes catch the beam;
// place one in a room and the whole room feels unventilated.
class DustVolume3D : public GPUParticles3D {
	GDCLASS(DustVolume3D, GPUParticles3D);

private:
	Vector3 size = Vector3(4, 3, 4);
	real_t density = 6.0; // Motes per cubic metre.
	real_t mote_size = 0.02;
	Color mote_color = Color(1, 0.95, 0.85);
	real_t opacity = 0.5;
	real_t drift_speed = 0.05;

	Ref<ParticleProcessMaterial> process_material;
	Ref<QuadMesh> quad;
	Ref<StandardMaterial3D> mote_material;
	Ref<GradientTexture1D> fade_ramp;
	static Ref<ImageTexture> mote_texture;

	void _rebuild();
	int _mote_count() const;

protected:
	static void _bind_methods();

public:
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;

	void set_density(real_t p_density);
	real_t get_density() const;

	void set_mote_size(real_t p_size);
	real_t get_mote_size() const;

	void set_mote_color(const Color &p_color);
	Color get_mote_color() const;

	void set_opacity(real_t p_opacity);
	real_t get_opacity() const;

	void set_drift_speed(real_t p_speed);
	real_t get_drift_speed() const;

	DustVolume3D();
	~DustVolume3D();
};
