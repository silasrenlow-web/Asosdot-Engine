/**************************************************************************/
/*  acoustic_engine.h                                                     */
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

#include "core/object/object.h"
#include "core/templates/hash_set.h"
#include "core/templates/local_vector.h"
#include "core/variant/dictionary.h"
#include "scene/3d/acoustic_event.h"
#include "scene/resources/3d/acoustic_material.h"

class AcousticHearing3D;
class AudioEffectReverb;
class AudioStreamPlayer3D;
class Node3D;
class World3D;
#ifndef PHYSICS_3D_DISABLED
class PhysicsDirectSpaceState3D;
#endif // PHYSICS_3D_DISABLED

// Asosdot Acoustic Engine (AAE) 0.1: how sound gets from one place to another in
// the level. Through walls by what they are made of, around corners through the
// openings a character could walk through, and into the reverb of the room the
// listener is in. The same answer drives what the player hears and what enemies
// hear.
class AcousticEngine : public Object {
	GDCLASS(AcousticEngine, Object);

	static AcousticEngine *singleton;

public:
	// What happened to a sound on its way from one point to another.
	struct Trace {
		// At the far end: 1 at the source, falling to 0 at the edge of its range.
		float loudness = 0.0;
		// How far it travelled.
		real_t distance = 0.0;
		// What got through whatever is in the straight line between the two.
		float transmission = 1.0;
		// It came the long way round, through openings, because that was louder.
		bool around = false;
		// How far it had to bend on the way round, in radians, and at how many
		// corners it turned more than a little.
		real_t bend = 0.0;
		int corners = 0;
		// Where it seems to come from at the far end.
		Vector3 apparent_position;
		Vector<Vector3> path;
	};

	// The room around the listener, measured by casting rays from it.
	struct Room {
		float enclosure = 0.0; // How much of what is around is closed in, 0 to 1.
		float size = 0.0; // The average distance to the surfaces around, in meters.
		float absorption = 1.0;
		float scattering = 0.0;
		float reverb_time = 0.0; // RT60, in seconds.
		float wet = 0.0; // How much reverb a sound gets.
	};

private:
	LocalVector<ObjectID> players;
	LocalVector<ObjectID> hearings;
	uint32_t next_player = 0;

	Room room;
	Room room_target;
	double room_timer = 0.0;

	Ref<AudioEffectReverb> reverb;
	StringName reverb_bus;

	Ref<AcousticMaterial> fallback_material;
	mutable Ref<AcousticMaterial> default_material;
	mutable String default_material_path;

	LocalVector<Vector3> room_directions;

	static float _falloff(real_t p_distance, real_t p_range);
	Ref<AcousticMaterial> _get_material(ObjectID p_collider) const;
	static void _exclude_body_of(const Node *p_node, HashSet<RID> &r_exclude);
	static void _prune(LocalVector<ObjectID> &r_list);
	Ref<World3D> _get_world(const Node *p_node) const;
	Node3D *_get_listener() const;

#ifndef PHYSICS_3D_DISABLED
	float _transmission(PhysicsDirectSpaceState3D *p_space, const Vector3 &p_from, const Vector3 &p_to, const HashSet<RID> &p_exclude) const;
#endif // PHYSICS_3D_DISABLED
	Vector<Vector3> _find_path(const Ref<World3D> &p_world, const Vector3 &p_from, const Vector3 &p_to) const;

	void _trace_player(AudioStreamPlayer3D *p_player, Node3D *p_listener);
	void _follow_player(AudioStreamPlayer3D *p_player, float p_blend);
	void _measure_room(Node3D *p_listener);
	void _update_reverb();

protected:
	static void _bind_methods();

public:
	static AcousticEngine *get_singleton() { return singleton; }

	Trace trace(const Ref<World3D> &p_world, const Vector3 &p_from, const Vector3 &p_to, real_t p_range, const HashSet<RID> &p_exclude) const;

	int emit_sound(const Ref<AcousticEvent> &p_event);
	Dictionary trace_sound(const Vector3 &p_from, const Vector3 &p_to, float p_intensity = 1.0) const;
	Dictionary get_room() const;

	// The bus the reverb of the listener's room is on, or empty when there is none.
	StringName get_reverb_bus() const { return reverb.is_valid() ? reverb_bus : StringName(); }
	float get_reverb_wet() const { return room.wet; }

	void register_player(AudioStreamPlayer3D *p_player);
	void unregister_player(AudioStreamPlayer3D *p_player);
	void register_hearing(AcousticHearing3D *p_hearing);
	void unregister_hearing(AcousticHearing3D *p_hearing);

	void _tick(double p_delta);

	AcousticEngine();
	~AcousticEngine();
};
