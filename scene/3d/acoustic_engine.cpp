/**************************************************************************/
/*  acoustic_engine.cpp                                                   */
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

#include "acoustic_engine.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "scene/3d/acoustic_hearing_3d.h"
#include "scene/3d/audio_listener_3d.h"
#include "scene/3d/audio_stream_player_3d.h"
#include "scene/3d/camera_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/window.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/audio/audio_server.h"
#include "servers/audio/effects/audio_effect_reverb.h"

#ifndef PHYSICS_3D_DISABLED
#include "scene/3d/physics/collision_object_3d.h"
#include "scene/3d/physics/static_body_3d.h"
#include "servers/physics_3d/physics_server_3d.h"
#endif // PHYSICS_3D_DISABLED

#ifndef NAVIGATION_3D_DISABLED
#include "servers/navigation_3d/navigation_server_3d.h"
#endif // NAVIGATION_3D_DISABLED

AcousticEngine *AcousticEngine::singleton = nullptr;

// Bending a right angle round a corner on the way costs this much of the sound.
static constexpr float CORNER_FACTOR = 0.8f;

static float _bend_loss(real_t p_bend) {
	return Math::pow(CORNER_FACTOR, float(p_bend / (Math::PI * 0.5)));
}

float AcousticEngine::_falloff(real_t p_distance, real_t p_range) {
	if (p_range <= 0) {
		return 0.0f;
	}
	return CLAMP(float(1.0 - p_distance / p_range), 0.0f, 1.0f);
}

Ref<AcousticMaterial> AcousticEngine::_get_material(ObjectID p_collider) const {
	// Whatever the collider is, a body, a CSG shape or a script's own node: if it
	// has an acoustic_material, that is the one.
	if (Object *collider = ObjectDB::get_instance(p_collider)) {
		bool valid = false;
		const Variant value = collider->get(SNAME("acoustic_material"), &valid);
		if (valid) {
			const Ref<AcousticMaterial> material = value;
			if (material.is_valid()) {
				return material;
			}
		}
	}

	const String path = GLOBAL_GET("audio/acoustics/default_material");
	if (path != default_material_path) {
		default_material_path = path;
		default_material.unref();
		if (!path.is_empty()) {
			default_material = ResourceLoader::load(path, "AcousticMaterial");
		}
	}
	return default_material.is_valid() ? default_material : fallback_material;
}

void AcousticEngine::_exclude_body_of(const Node *p_node, HashSet<RID> &r_exclude) {
#ifndef PHYSICS_3D_DISABLED
	// The body the node is on, a character or a rigid body, is the one making or
	// hearing the sound, not something in its way. A static body is the level.
	for (const Node *node = p_node; node; node = node->get_parent()) {
		if (const CollisionObject3D *body = Object::cast_to<CollisionObject3D>(node)) {
			if (!Object::cast_to<StaticBody3D>(body)) {
				r_exclude.insert(body->get_rid());
			}
			return;
		}
	}
#endif // PHYSICS_3D_DISABLED
}

void AcousticEngine::_prune(LocalVector<ObjectID> &r_list) {
	for (uint32_t i = 0; i < r_list.size();) {
		if (ObjectDB::get_instance(r_list[i])) {
			i++;
		} else {
			r_list.remove_at_unordered(i);
		}
	}
}

Ref<World3D> AcousticEngine::_get_world(const Node *p_node) const {
	if (const Node3D *node = Object::cast_to<Node3D>(p_node)) {
		if (node->is_inside_tree()) {
			return node->get_world_3d();
		}
	}
	SceneTree *tree = SceneTree::get_singleton();
	if (!tree || !tree->get_root()) {
		return Ref<World3D>();
	}
	return tree->get_root()->find_world_3d();
}

Node3D *AcousticEngine::_get_listener() const {
	SceneTree *tree = SceneTree::get_singleton();
	Window *root = tree ? tree->get_root() : nullptr;
	if (!root) {
		return nullptr;
	}
	if (AudioListener3D *listener = root->get_audio_listener_3d()) {
		return listener;
	}
	return root->get_camera_3d();
}

#ifndef PHYSICS_3D_DISABLED
float AcousticEngine::_transmission(PhysicsDirectSpaceState3D *p_space, const Vector3 &p_from, const Vector3 &p_to, const HashSet<RID> &p_exclude) const {
	const Vector3 offset = p_to - p_from;
	const real_t length = offset.length();
	if (length < CMP_EPSILON) {
		return 1.0f;
	}
	const Vector3 direction = offset / length;

	PhysicsDirectSpaceState3D::RayParameters ray;
	ray.from = p_from;
	ray.to = p_to;
	ray.exclude = p_exclude;
	ray.collision_mask = GLOBAL_GET_CACHED(uint32_t, "audio/acoustics/collision_mask");
	ray.hit_back_faces = true;

	float transmission = 1.0f;
	// A level can be one collider, so the line goes on past each surface it meets
	// rather than leaving out everything that collider is.
	for (int crossing = 0; crossing < 16; crossing++) {
		PhysicsDirectSpaceState3D::RayResult hit;
		if (!p_space->intersect_ray(ray, hit)) {
			break;
		}
		// Going into a surface; coming out of the other side is the same wall.
		if (hit.normal.dot(direction) < 0) {
			transmission *= _get_material(hit.collider_id)->get_transmission();
			if (transmission < 0.0005f) {
				return 0.0f;
			}
		}
		ray.from = hit.position + direction * 0.01;
		if ((p_to - ray.from).dot(direction) <= 0) {
			break;
		}
	}
	return transmission;
}
#endif // PHYSICS_3D_DISABLED

Vector<Vector3> AcousticEngine::_find_path(const Ref<World3D> &p_world, const Vector3 &p_from, const Vector3 &p_to) const {
#ifndef NAVIGATION_3D_DISABLED
	NavigationServer3D *navigation = NavigationServer3D::get_singleton();
	if (!navigation || p_world.is_null()) {
		return Vector<Vector3>();
	}
	const RID map = p_world->get_navigation_map();
	if (!map.is_valid() || navigation->map_get_iteration_id(map) == 0 || navigation->map_get_regions(map).is_empty()) {
		return Vector<Vector3>();
	}
	Vector<Vector3> path = navigation->map_get_path(map, p_from, p_to, true);
	// Sound goes where a character could, so the level's navigation mesh is the way
	// round. Ends far from it are somewhere it doesn't reach.
	const real_t snap = 3.0;
	if (path.size() < 2 || path[0].distance_to(p_from) > snap || path[path.size() - 1].distance_to(p_to) > snap) {
		return Vector<Vector3>();
	}
	return path;
#else
	return Vector<Vector3>();
#endif // NAVIGATION_3D_DISABLED
}

AcousticEngine::Trace AcousticEngine::trace(const Ref<World3D> &p_world, const Vector3 &p_from, const Vector3 &p_to, real_t p_range, const HashSet<RID> &p_exclude) const {
	Trace result;
	result.apparent_position = p_from;
	const real_t straight = p_from.distance_to(p_to);
	result.distance = straight;
	if (p_range <= 0 || straight >= p_range) {
		return result;
	}

#ifndef PHYSICS_3D_DISABLED
	if (p_world.is_valid()) {
		if (PhysicsDirectSpaceState3D *space = PhysicsServer3D::get_singleton()->space_get_direct_state(p_world->get_space())) {
			result.transmission = _transmission(space, p_from, p_to, p_exclude);
		}
	}
#endif // PHYSICS_3D_DISABLED

	const float direct = _falloff(straight, p_range) * result.transmission;
	result.loudness = direct;
	if (result.transmission >= 1.0f) {
		return result;
	}

	// Something is in the way: the way round may be louder than what gets through.
	const Vector<Vector3> path = _find_path(p_world, p_from, p_to);
	if (path.size() < 2) {
		return result;
	}
	real_t length = p_from.distance_to(path[0]) + path[path.size() - 1].distance_to(p_to);
	for (int i = 1; i < path.size(); i++) {
		length += path[i - 1].distance_to(path[i]);
	}
	// What sound loses going round is how far it has to bend, however many points
	// the navigation mesh happens to put on the turn.
	real_t bend = 0.0;
	int corners = 0;
	for (int i = 1; i + 1 < path.size(); i++) {
		const Vector3 in = path[i] - path[i - 1];
		const Vector3 out = path[i + 1] - path[i];
		if (in.length_squared() < CMP_EPSILON2 || out.length_squared() < CMP_EPSILON2) {
			continue;
		}
		const real_t angle = in.angle_to(out);
		bend += angle;
		if (angle > Math::deg_to_rad(real_t(30.0))) {
			corners++;
		}
	}
	const float around = _falloff(length, p_range) * _bend_loss(bend);
	if (around <= direct) {
		return result;
	}

	result.around = true;
	result.loudness = around;
	result.distance = length;
	result.bend = bend;
	result.corners = corners;
	result.path = path;
	// The last turn before the far end is where it seems to come from, raised from
	// the floor the path keeps to by as much as the two ends are above it.
	const Vector3 corner = path.size() > 2 ? path[path.size() - 2] : path[0];
	const real_t height = ((p_from.y - path[0].y) + (p_to.y - path[path.size() - 1].y)) * 0.5;
	result.apparent_position = corner + Vector3(0, height, 0);
	return result;
}

int AcousticEngine::emit_sound(const Ref<AcousticEvent> &p_event) {
	ERR_FAIL_COND_V(p_event.is_null(), 0);
	emit_signal(SNAME("sound_emitted"), p_event);

	const real_t reach = p_event->get_intensity() * GLOBAL_GET_CACHED(real_t, "audio/acoustics/hearing_range");
	const Node *source = p_event->get_source();
	const Ref<World3D> world = _get_world(source);
	if (reach <= 0 || world.is_null()) {
		return 0;
	}
	struct Heard {
		ObjectID hearing;
		float loudness = 0.0;
		Vector3 from_position;
	};
	LocalVector<Heard> heard;

	_prune(hearings);
	const Vector3 origin = p_event->get_position();
	for (const ObjectID &id : hearings) {
		const AcousticHearing3D *hearing = ObjectDB::get_instance<AcousticHearing3D>(id);
		if (!hearing || !hearing->is_inside_tree() || hearing->get_world_3d() != world || !hearing->listens_to(p_event)) {
			continue;
		}
		// Keener ears hear the same sound from further away.
		const real_t range = reach * hearing->get_sensitivity();
		const Vector3 ear = hearing->get_global_position();
		if (origin.distance_to(ear) >= range) {
			continue;
		}
		HashSet<RID> exclude;
		_exclude_body_of(source, exclude);
		_exclude_body_of(hearing, exclude);
		const Trace result = trace(world, origin, ear, range, exclude);
		if (result.loudness > 0.0f && result.loudness >= hearing->get_min_loudness()) {
			Heard entry;
			entry.hearing = id;
			entry.loudness = result.loudness;
			entry.from_position = result.apparent_position;
			heard.push_back(entry);
		}
	}

	// Told only once all are worked out, since what they do about it can free them.
	int count = 0;
	for (const Heard &entry : heard) {
		if (AcousticHearing3D *hearing = ObjectDB::get_instance<AcousticHearing3D>(entry.hearing)) {
			hearing->emit_signal(SNAME("sound_heard"), p_event, entry.loudness, entry.from_position);
			count++;
		}
	}
	return count;
}

Dictionary AcousticEngine::trace_sound(const Vector3 &p_from, const Vector3 &p_to, float p_intensity) const {
	const real_t range = p_intensity * GLOBAL_GET_CACHED(real_t, "audio/acoustics/hearing_range");
	const Trace result = trace(_get_world(nullptr), p_from, p_to, range, HashSet<RID>());
	Dictionary info;
	info["loudness"] = result.loudness;
	info["distance"] = result.distance;
	info["transmission"] = result.transmission;
	info["around"] = result.around;
	info["corners"] = result.corners;
	info["bend"] = Math::rad_to_deg(result.bend);
	info["apparent_position"] = result.apparent_position;
	info["path"] = PackedVector3Array(result.path);
	return info;
}

Dictionary AcousticEngine::get_room() const {
	Dictionary info;
	info["enclosure"] = room.enclosure;
	info["size"] = room.size;
	info["absorption"] = room.absorption;
	info["scattering"] = room.scattering;
	info["reverb_time"] = room.reverb_time;
	info["wet"] = room.wet;
	return info;
}

void AcousticEngine::register_player(AudioStreamPlayer3D *p_player) {
	ERR_FAIL_NULL(p_player);
	const ObjectID id = p_player->get_instance_id();
	if (players.find(id) < 0) {
		players.push_back(id);
	}
}

void AcousticEngine::unregister_player(AudioStreamPlayer3D *p_player) {
	ERR_FAIL_NULL(p_player);
	const int64_t index = players.find(p_player->get_instance_id());
	if (index >= 0) {
		players.remove_at_unordered(index);
	}
}

void AcousticEngine::register_hearing(AcousticHearing3D *p_hearing) {
	ERR_FAIL_NULL(p_hearing);
	const ObjectID id = p_hearing->get_instance_id();
	if (hearings.find(id) < 0) {
		hearings.push_back(id);
	}
}

void AcousticEngine::unregister_hearing(AcousticHearing3D *p_hearing) {
	ERR_FAIL_NULL(p_hearing);
	const int64_t index = hearings.find(p_hearing->get_instance_id());
	if (index >= 0) {
		hearings.remove_at_unordered(index);
	}
}

void AcousticEngine::_trace_player(AudioStreamPlayer3D *p_player, Node3D *p_listener) {
	AudioStreamPlayer3D::AcousticState target;
	const Ref<World3D> world = p_player->get_world_3d();
	if (world.is_null() || world != p_listener->get_world_3d()) {
		p_player->acoustic_target = target;
		p_player->acoustic_state = target;
		return;
	}

	HashSet<RID> exclude;
	_exclude_body_of(p_player, exclude);
	_exclude_body_of(p_listener, exclude);
	const Vector3 from = p_player->get_global_position();
	const Vector3 to = p_listener->get_global_position();
	const real_t range = MAX(real_t(p_player->get_max_distance()), real_t(200.0));
	const Trace result = trace(world, from, to, range, exclude);

	target.valid = true;
	if (result.around) {
		// Heard from the opening it comes through, as far away as the way round.
		Vector3 direction = result.apparent_position - to;
		if (direction.length_squared() < CMP_EPSILON2) {
			direction = from - to;
		}
		target.offset = direction.normalized() * result.distance;
		const float right_angles = float(result.bend / (Math::PI * 0.5));
		target.gain = _bend_loss(result.bend);
		target.muffle = MIN(0.2f * right_angles, 0.6f);
	} else {
		// Through the wall: quieter, and losing its high end first.
		target.offset = from - to;
		target.gain = result.transmission;
		target.muffle = CLAMP(1.0f - Math::sqrt(result.transmission), 0.0f, 1.0f);
	}
	target.reverb_send = room.wet;
	p_player->acoustic_target = target;
	p_player->acoustic_listener = p_listener->get_instance_id();
	if (!p_player->acoustic_state.valid) {
		p_player->acoustic_state = target;
	}
}

void AcousticEngine::_follow_player(AudioStreamPlayer3D *p_player, float p_blend) {
	AudioStreamPlayer3D::AcousticState &state = p_player->acoustic_state;
	const AudioStreamPlayer3D::AcousticState &target = p_player->acoustic_target;
	if (!state.valid || !target.valid) {
		return;
	}
	// The way and the distance move on their own, so a sound that switches from the
	// wall to the doorway swings round the listener instead of passing through it.
	const real_t length = Math::lerp(state.offset.length(), target.offset.length(), real_t(p_blend));
	Vector3 direction = state.offset.normalized().lerp(target.offset.normalized(), p_blend);
	if (direction.length_squared() < CMP_EPSILON2) {
		direction = target.offset.normalized();
	}
	state.offset = direction.normalized() * length;
	state.gain = Math::lerp(state.gain, target.gain, p_blend);
	state.muffle = Math::lerp(state.muffle, target.muffle, p_blend);
	state.reverb_send = room.wet;
}

void AcousticEngine::_measure_room(Node3D *p_listener) {
#ifndef PHYSICS_3D_DISABLED
	const Ref<World3D> world = p_listener->get_world_3d();
	PhysicsDirectSpaceState3D *space = world.is_valid() ? PhysicsServer3D::get_singleton()->space_get_direct_state(world->get_space()) : nullptr;
	if (!space) {
		return;
	}
	HashSet<RID> exclude;
	_exclude_body_of(p_listener, exclude);
	const Vector3 origin = p_listener->get_global_position();
	const real_t probe_length = 60.0;

	int hits = 0;
	real_t distance_sum = 0.0;
	float absorption_sum = 0.0f;
	float scattering_sum = 0.0f;
	for (const Vector3 &direction : room_directions) {
		PhysicsDirectSpaceState3D::RayParameters ray;
		ray.from = origin;
		ray.to = origin + direction * probe_length;
		ray.exclude = exclude;
		ray.collision_mask = GLOBAL_GET_CACHED(uint32_t, "audio/acoustics/collision_mask");
		PhysicsDirectSpaceState3D::RayResult hit;
		if (space->intersect_ray(ray, hit)) {
			const Ref<AcousticMaterial> material = _get_material(hit.collider_id);
			hits++;
			distance_sum += origin.distance_to(hit.position);
			absorption_sum += material->get_absorption();
			scattering_sum += material->get_scattering();
		}
	}

	Room measured;
	const int count = int(room_directions.size());
	if (hits > 0 && count > 0) {
		measured.enclosure = float(hits) / float(count);
		measured.size = float(distance_sum / hits);
		measured.absorption = absorption_sum / float(hits);
		measured.scattering = scattering_sum / float(hits);
		// Eyring's reverberation time, for a sphere as far out as the walls are on
		// average. An open direction lets the sound out for good, as if its surface
		// took all of it.
		const float absorbed = CLAMP((absorption_sum + float(count - hits)) / float(count), 0.001f, 0.999f);
		measured.reverb_time = CLAMP(0.161f * measured.size / (3.0f * -Math::log(1.0f - absorbed)), 0.0f, 10.0f);
		measured.wet = CLAMP(measured.enclosure * (0.2f + 0.5f * MIN(measured.reverb_time / 3.0f, 1.0f)), 0.0f, 0.8f);
	}
	room_target = measured;
#endif // PHYSICS_3D_DISABLED
}

void AcousticEngine::_update_reverb() {
	AudioServer *audio = AudioServer::get_singleton();
	if (!audio || !GLOBAL_GET_CACHED(bool, "audio/acoustics/reverb")) {
		reverb.unref();
		return;
	}

	// Its own bus, made when first needed and made again if a bus layout replaced it.
	reverb_bus = StringName(String(GLOBAL_GET("audio/acoustics/reverb_bus")));
	int bus = audio->get_bus_index(reverb_bus);
	if (bus < 0) {
		audio->add_bus();
		bus = audio->get_bus_count() - 1;
		audio->set_bus_name(bus, reverb_bus);
		audio->set_bus_send(bus, SNAME("Master"));
		reverb.unref();
	}
	if (reverb.is_null()) {
		for (int i = 0; i < audio->get_bus_effect_count(bus); i++) {
			reverb = audio->get_bus_effect(bus, i);
			if (reverb.is_valid()) {
				break;
			}
		}
	}
	if (reverb.is_null()) {
		reverb.instantiate();
		audio->add_bus_effect(bus, reverb);
	}

	reverb->set_dry(0.0f);
	reverb->set_wet(1.0f);
	reverb->set_room_size(CLAMP(room.reverb_time / 4.0f, 0.05f, 1.0f));
	reverb->set_damping(CLAMP(0.15f + room.absorption * 0.85f, 0.0f, 1.0f));
	reverb->set_spread(CLAMP(0.5f + room.scattering * 0.5f, 0.0f, 1.0f));
	reverb->set_predelay_msec(CLAMP(2.0f * room.size / 343.0f * 1000.0f, 20.0f, 300.0f));
}

void AcousticEngine::_tick(double p_delta) {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	_prune(players);
	if (players.is_empty()) {
		return;
	}
	Node3D *listener = _get_listener();
	if (!listener || !listener->is_inside_tree()) {
		return;
	}

	room_timer -= p_delta;
	if (room_timer <= 0.0) {
		room_timer = 0.2;
		_measure_room(listener);
	}
	const float room_blend = 1.0f - Math::exp(-float(p_delta) * 3.0f);
	room.enclosure = Math::lerp(room.enclosure, room_target.enclosure, room_blend);
	room.size = Math::lerp(room.size, room_target.size, room_blend);
	room.absorption = Math::lerp(room.absorption, room_target.absorption, room_blend);
	room.scattering = Math::lerp(room.scattering, room_target.scattering, room_blend);
	room.reverb_time = Math::lerp(room.reverb_time, room_target.reverb_time, room_blend);
	room.wet = Math::lerp(room.wet, room_target.wet, room_blend);
	_update_reverb();

	// A sound that has just started is traced straight away; the rest take turns, a
	// few a frame, so many sounds cost no more than a few.
	for (const ObjectID &id : players) {
		AudioStreamPlayer3D *player = ObjectDB::get_instance<AudioStreamPlayer3D>(id);
		if (player && player->is_playing() && !player->acoustic_state.valid) {
			_trace_player(player, listener);
		}
	}
	const uint32_t turns = MIN(uint32_t(8), players.size());
	for (uint32_t i = 0; i < turns; i++) {
		next_player = next_player % players.size();
		AudioStreamPlayer3D *player = ObjectDB::get_instance<AudioStreamPlayer3D>(players[next_player]);
		next_player++;
		if (player && player->is_playing()) {
			_trace_player(player, listener);
		}
	}

	const float follow = 1.0f - Math::exp(-float(p_delta) * 10.0f);
	for (const ObjectID &id : players) {
		if (AudioStreamPlayer3D *player = ObjectDB::get_instance<AudioStreamPlayer3D>(id)) {
			_follow_player(player, follow);
		}
	}
}

void AcousticEngine::_bind_methods() {
	ClassDB::bind_method(D_METHOD("emit_sound", "event"), &AcousticEngine::emit_sound);
	ClassDB::bind_method(D_METHOD("trace_sound", "from", "to", "intensity"), &AcousticEngine::trace_sound, DEFVAL(1.0));
	ClassDB::bind_method(D_METHOD("get_room"), &AcousticEngine::get_room);

	ADD_SIGNAL(MethodInfo("sound_emitted", PropertyInfo(Variant::OBJECT, "event", PROPERTY_HINT_RESOURCE_TYPE, "AcousticEvent")));
}

AcousticEngine::AcousticEngine() {
	singleton = this;

	GLOBAL_DEF(PropertyInfo(Variant::FLOAT, "audio/acoustics/hearing_range", PROPERTY_HINT_RANGE, "1,1000,0.1,or_greater,suffix:m"), 40.0);
	GLOBAL_DEF(PropertyInfo(Variant::INT, "audio/acoustics/collision_mask", PROPERTY_HINT_LAYERS_3D_PHYSICS), 0xFFFFFFFF);
	GLOBAL_DEF(PropertyInfo(Variant::STRING, "audio/acoustics/default_material", PROPERTY_HINT_FILE, "*.tres,*.res"), "");
	GLOBAL_DEF("audio/acoustics/reverb", true);
	GLOBAL_DEF("audio/acoustics/reverb_bus", "AAE Reverb");

	fallback_material.instantiate();

	// Evenly over the sphere, so no direction counts for more than another.
	const int count = 32;
	for (int i = 0; i < count; i++) {
		const real_t y = 1.0 - (i + 0.5) * 2.0 / count;
		const real_t radius = Math::sqrt(1.0 - y * y);
		const real_t angle = i * Math::PI * (3.0 - Math::sqrt(5.0));
		room_directions.push_back(Vector3(Math::cos(angle) * radius, y, Math::sin(angle) * radius));
	}
}

AcousticEngine::~AcousticEngine() {
	singleton = nullptr;
}
