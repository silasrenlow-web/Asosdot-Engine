/**************************************************************************/
/*  gltf_document_extension_blender_rig.cpp                               */
/**************************************************************************/
/*  Asosdot: Blender IK rigs rebuilt as skeleton modifiers.               */
/**************************************************************************/

#include "gltf_document_extension_blender_rig.h"

#include "../gltf_state.h"

#include "core/io/json.h"
#include "scene/3d/blender_constraint_3d.h"
#include "scene/3d/fabr_ik_3d.h"
#include "scene/3d/modifier_bone_target_3d.h"
#include "scene/3d/skeleton_3d.h"
#include "scene/3d/two_bone_ik_3d.h"

namespace {

struct RigItem {
	int depth = 0;
	int order = 0;
	// The owner: a bone, or a node that follows the armature.
	int bone = -1;
	Node3D *owner_node = nullptr;
	String owner_label;
	Dictionary constraint;
	// Bones whose pose this constraint sets, and bones whose pose it reads.
	LocalVector<int> modifies;
	LocalVector<int> reads;
};

// Parents before children, as Blender evaluates a pose. One bone's constraints keep their stack order.
struct RigItemOrder {
	bool operator()(const RigItem &p_a, const RigItem &p_b) const {
		return p_a.depth != p_b.depth ? p_a.depth < p_b.depth : p_a.order < p_b.order;
	}
};

Vector3 to_vector3(const Variant &p_value) {
	const Array array = p_value;
	return array.size() == 3 ? Vector3(array[0], array[1], array[2]) : Vector3();
}

} // namespace

struct AsosdotGLTFDocumentExtensionBlenderRig::Builder {
	Ref<GLTFState> state;
	Skeleton3D *skeleton = nullptr;
	Node *owner = nullptr;
	HashMap<String, Node3D *> targets;
	PackedStringArray skipped;
	PackedStringArray approximate;
	PackedStringArray baked;
	PackedStringArray missing;

	template <typename T>
	T *add(Node *p_parent, const String &p_name) {
		T *node = memnew(T);
		node->set_name(p_name.validate_node_name());
		p_parent->add_child(node, true);
		node->set_owner(owner);
		return node;
	}

	// What a constraint's target follows: a bone of this armature, or another object.
	Node3D *reference(const Variant &p_reference) {
		if (p_reference.get_type() != Variant::DICTIONARY) {
			return nullptr;
		}
		const Dictionary reference = p_reference;
		const String object = reference.get("object", String());
		if (!object.is_empty()) {
			const Vector<Ref<GLTFNode>> &nodes = state->get_nodes();
			for (GLTFNodeIndex i = 0; i < nodes.size(); i++) {
				if (nodes[i]->get_original_name() == object) {
					return Object::cast_to<Node3D>(state->get_scene_node(i));
				}
			}
			return nullptr;
		}
		const String bone = reference.get("bone", String());
		if (bone.is_empty() || skeleton->find_bone(bone) < 0) {
			return nullptr;
		}
		if (Node3D **existing = targets.getptr(bone)) {
			return *existing;
		}
		// A control bone is followed by a modifier, so constraints after it read this frame's pose.
		ModifierBoneTarget3D *target = add<ModifierBoneTarget3D>(skeleton, "Target " + bone);
		target->set_bone_name(bone);
		target->set_bone(skeleton->find_bone(bone));
		targets.insert(bone, target);
		return target;
	}

	// IK without a pole in Blender bends the chain where it bent before. Here, a
	// point that rides with the chain's parent, toward that bend, stands in for a pole.
	Node3D *stand_in_pole(const Dictionary &p_constraint, const String &p_bone_name) {
		if (!p_constraint.has("pole_point")) {
			return nullptr;
		}
		Node3D *parent = skeleton;
		Vector3 point = to_vector3(p_constraint["pole_point"]);
		const String pole_parent = p_constraint.get("pole_parent", String());
		if (pole_parent.is_empty()) {
			// Blender's armature space, Z up, as the glTF export turns it Y up.
			point = Vector3(point.x, point.z, -point.y);
		} else {
			Dictionary bone;
			bone["bone"] = pole_parent;
			parent = reference(bone);
			if (!parent) {
				return nullptr;
			}
		}
		Node3D *pole = add<Node3D>(parent, "Pole " + p_bone_name);
		pole->set_position(point);
		return pole;
	}

	void ik(const Dictionary &p_constraint, int p_bone, float p_influence) {
		const String bone_name = skeleton->get_bone_name(p_bone);
		if (!bool(p_constraint.get("use_tail", true))) {
			skipped.push_back(vformat("IK on %s (Use Tail is off)", bone_name));
			return;
		}
		Node3D *target = reference(p_constraint.get("target", Variant()));
		if (!target) {
			skipped.push_back(vformat("IK on %s (its target isn't in the import)", bone_name));
			return;
		}
		const float length = p_constraint.get("length", 0.0);
		const int chain = p_constraint.get("chain_count", 0);
		if (bool(p_constraint.get("use_rotation", false))) {
			approximate.push_back(vformat("IK on %s ignores Rotation", bone_name));
		}
		if (chain == 2) {
			const int root = skeleton->get_bone_parent(p_bone);
			if (root < 0) {
				skipped.push_back(vformat("IK on %s (no parent bone for a two-bone chain)", bone_name));
				return;
			}
			Node3D *pole = reference(p_constraint.get("pole", Variant()));
			if (!pole) {
				pole = stand_in_pole(p_constraint, bone_name);
				if (pole) {
					approximate.push_back(vformat("IK on %s has no pole, so one was placed at its bend", bone_name));
				}
			}
			if (!pole) {
				skipped.push_back(vformat("IK on %s (no pole and no bend to place one at)", bone_name));
				return;
			}
			const float pole_offset = p_constraint.get("pole_offset", 0.0);
			if (pole_offset > Math::deg_to_rad(1.0f)) {
				approximate.push_back(vformat("IK on %s bends %d degrees away from its pole in Blender; here it bends toward the pole", bone_name, int(Math::round(Math::rad_to_deg(pole_offset)))));
			}
			TwoBoneIK3D *solver = add<TwoBoneIK3D>(skeleton, "IK " + bone_name);
			solver->set_setting_count(1);
			// Indices as well as names: the scene is saved before this node reaches its
			// skeleton, and settings that depend on a known bone are only saved then.
			solver->set_root_bone_name(0, skeleton->get_bone_name(root));
			solver->set_root_bone(0, root);
			solver->set_middle_bone_name(0, bone_name);
			solver->set_middle_bone(0, p_bone);
			// Blender's chain ends at the bone's tail, which no bone may start at.
			solver->set_use_virtual_end(0, true);
			solver->set_extend_end_bone(0, true);
			solver->set_end_bone_direction(0, SkeletonModifier3D::BONE_DIRECTION_PLUS_Y);
			solver->set_end_bone_length(0, length);
			solver->set_target_node(0, solver->get_path_to(target));
			solver->set_pole_node(0, solver->get_path_to(pole));
			if (p_constraint.has("pole_vector")) {
				// The bone's own direction that Blender turns toward the pole, so the bones twist as they do there.
				solver->set_pole_direction(0, SkeletonModifier3D::SECONDARY_DIRECTION_CUSTOM);
				solver->set_pole_direction_vector(0, to_vector3(p_constraint["pole_vector"]));
			}
			if (p_constraint.has("root_pole_vector")) {
				// And the root bone's, which Blender keeps toward the pole as the chain bends.
				solver->set_root_pole_direction(0, SkeletonModifier3D::SECONDARY_DIRECTION_CUSTOM);
				solver->set_root_pole_direction_vector(0, to_vector3(p_constraint["root_pole_vector"]));
			}
			solver->set_influence(p_influence);
			return;
		}
		if (chain == 1) {
			skipped.push_back(vformat("IK on %s (a one-bone chain)", bone_name));
			return;
		}
		// Longer chains, or every bone up to the root for a chain count of 0.
		int root = p_bone;
		for (int i = 1; chain <= 0 || i < chain; i++) {
			const int parent = skeleton->get_bone_parent(root);
			if (parent < 0) {
				break;
			}
			root = parent;
		}
		approximate.push_back(vformat("IK on %s solves its longer chain with FABRIK", bone_name));
		FABRIK3D *solver = add<FABRIK3D>(skeleton, "IK " + bone_name);
		solver->set_setting_count(1);
		solver->set_root_bone_name(0, skeleton->get_bone_name(root));
		solver->set_root_bone(0, root);
		solver->set_end_bone_name(0, bone_name);
		solver->set_end_bone(0, p_bone);
		solver->set_extend_end_bone(0, true);
		solver->set_end_bone_direction(0, SkeletonModifier3D::BONE_DIRECTION_PLUS_Y);
		solver->set_end_bone_length(0, length);
		solver->set_target_node(0, solver->get_path_to(target));
		solver->set_deterministic(true);
		solver->set_influence(p_influence);
	}

	static BlenderConstraint3D::Space space_of(const String &p_space) {
		if (p_space == "POSE") {
			return BlenderConstraint3D::SPACE_POSE;
		} else if (p_space == "LOCAL_WITH_PARENT") {
			return BlenderConstraint3D::SPACE_LOCAL_WITH_PARENT;
		} else if (p_space == "LOCAL") {
			return BlenderConstraint3D::SPACE_LOCAL;
		}
		return BlenderConstraint3D::SPACE_WORLD;
	}

	static int type_of(const String &p_type) {
		static const char *types[BlenderConstraint3D::TYPE_MAX] = { "COPY_LOCATION", "COPY_ROTATION", "COPY_SCALE", "COPY_TRANSFORMS", "LIMIT_LOCATION", "LIMIT_ROTATION", "LIMIT_SCALE", "CHILD_OF", "DAMPED_TRACK", "TRACK_TO", "LOCKED_TRACK", "STRETCH_TO", "TRANSFORM", "MAINTAIN_VOLUME" };
		for (int i = 0; i < BlenderConstraint3D::TYPE_MAX; i++) {
			if (p_type == types[i]) {
				return i;
			}
		}
		return -1;
	}

	// A setting as the JSON gives it, in the type of its default.
	static Variant setting_value(const Variant &p_value, const Variant &p_default) {
		switch (p_default.get_type()) {
			case Variant::BOOL:
				return bool(p_value);
			case Variant::FLOAT:
				return double(p_value);
			case Variant::STRING:
				return String(p_value);
			case Variant::TRANSFORM3D: {
				// Blender writes a matrix by rows.
				const Array rows = p_value;
				if (rows.size() != 4) {
					return p_default;
				}
				const Array r0 = rows[0];
				const Array r1 = rows[1];
				const Array r2 = rows[2];
				if (r0.size() != 4 || r1.size() != 4 || r2.size() != 4) {
					return p_default;
				}
				return Transform3D(Basis(r0[0], r0[1], r0[2], r1[0], r1[1], r1[2], r2[0], r2[1], r2[2]), Vector3(r0[3], r1[3], r2[3]));
			}
			default:
				return p_value;
		}
	}

	Node3D *object_node(const String &p_name) {
		const Vector<Ref<GLTFNode>> &nodes = state->get_nodes();
		for (GLTFNodeIndex i = 0; i < nodes.size(); i++) {
			if (nodes[i]->get_original_name() == p_name) {
				return Object::cast_to<Node3D>(state->get_scene_node(i));
			}
		}
		return nullptr;
	}

	// The bone of this armature that a reference names.
	int bone_of(const Variant &p_reference) const {
		if (p_reference.get_type() != Variant::DICTIONARY) {
			return -1;
		}
		const Dictionary reference = p_reference;
		if (!String(reference.get("object", String())).is_empty()) {
			return -1;
		}
		const String bone = reference.get("bone", String());
		return bone.is_empty() ? -1 : skeleton->find_bone(bone);
	}

	// A Blender constraint, rebuilt as a BlenderConstraint3D with every setting it has there.
	void blender_constraint(const RigItem &p_item, int p_type) {
		const Dictionary &constraint = p_item.constraint;
		const String label = vformat("%s on %s", constraint.get("name", constraint.get("type", String())), p_item.owner_label);
		const bool muted = constraint.get("muted", false);
		String target_bone;
		Node3D *target_node = nullptr;
		const Variant target_value = constraint.get("target", Variant());
		if (target_value.get_type() == Variant::DICTIONARY) {
			const Dictionary target = target_value;
			const String object = target.get("object", String());
			if (object.is_empty()) {
				target_bone = target.get("bone", String());
				if (skeleton->find_bone(target_bone) < 0) {
					(muted ? missing : skipped).push_back(vformat("%s (its target bone isn't in the import)", label));
					return;
				}
			} else {
				target_node = object_node(object);
				if (!target_node) {
					(muted ? missing : skipped).push_back(vformat("%s (its target isn't in the import)", label));
					return;
				}
			}
		}
		BlenderConstraint3D *node = add<BlenderConstraint3D>(skeleton, label);
		node->set_constraint_type(BlenderConstraint3D::ConstraintType(p_type));
		if (p_item.bone >= 0) {
			// Index as well as name: the scene is saved before this node reaches its skeleton.
			node->set_bone_name(skeleton->get_bone_name(p_item.bone));
			node->set_bone(p_item.bone);
		} else {
			node->set_owner_node(node->get_path_to(p_item.owner_node));
		}
		node->set_owner_space(space_of(constraint.get("owner_space", "WORLD")));
		node->set_target_space(space_of(constraint.get("target_space", "WORLD")));
		node->set_owner_rotation_order(constraint.get("owner_rotation_order", "XYZ"));
		node->set_target_rotation_order(constraint.get("target_rotation_order", "XYZ"));
		if (!target_bone.is_empty()) {
			node->set_target_bone_name(target_bone);
			node->set_head_tail(constraint.get("head_tail", 0.0));
			node->set_target_bone_length(constraint.get("target_length", 0.0));
		} else if (target_node) {
			node->set_target_node(node->get_path_to(target_node));
		}
		const Dictionary defaults = node->get_settings();
		const Dictionary settings = constraint.get("settings", Dictionary());
		for (const KeyValue<Variant, Variant> &E : defaults) {
			if (settings.has(E.key)) {
				node->set_setting(E.key, setting_value(settings[E.key], E.value));
			}
		}
		node->set_influence(constraint.get("influence", 1.0));
		// Blender bakes an unmuted constraint into the animation, so here it would act twice.
		node->set_active(muted);
		if (!muted) {
			baked.push_back(vformat("%s (%s)", label, constraint.get("reason", "it was not muted for the export")));
		}
	}

	bool is_ancestor_or_self(int p_ancestor, int p_bone) const {
		for (int bone = p_bone; bone >= 0; bone = skeleton->get_bone_parent(bone)) {
			if (bone == p_ancestor) {
				return true;
			}
		}
		return false;
	}

	// Whether one constraint runs before another, as Blender's dependency graph orders
	// them: one that sets a bone runs before those that read it or a bone below it.
	bool runs_before(const RigItem &p_a, const RigItem &p_b) const {
		const bool same_owner = p_a.bone >= 0 ? p_a.bone == p_b.bone : p_a.owner_node == p_b.owner_node;
		if (same_owner) {
			return p_a.order < p_b.order;
		}
		for (const int bone : p_a.modifies) {
			if (p_b.bone >= 0 && bone != p_b.bone && is_ancestor_or_self(bone, p_b.bone)) {
				return true;
			}
			for (const int read : p_b.reads) {
				if (is_ancestor_or_self(bone, read)) {
					return true;
				}
			}
		}
		return false;
	}

	LocalVector<RigItem> sorted(const LocalVector<RigItem> &p_items) const {
		const uint32_t count = p_items.size();
		LocalVector<LocalVector<uint32_t>> after;
		LocalVector<int> waiting;
		LocalVector<bool> done;
		after.resize(count);
		waiting.resize(count);
		done.resize(count);
		for (uint32_t i = 0; i < count; i++) {
			waiting[i] = 0;
			done[i] = false;
		}
		for (uint32_t i = 0; i < count; i++) {
			for (uint32_t j = 0; j < count; j++) {
				if (i != j && runs_before(p_items[i], p_items[j])) {
					after[i].push_back(j);
					waiting[j]++;
				}
			}
		}
		RigItemOrder order;
		LocalVector<RigItem> result;
		for (uint32_t step = 0; step < count; step++) {
			// The first ready item, parents before children; in a cycle, the first left.
			int next = -1;
			for (int pass = 0; pass < 2 && next < 0; pass++) {
				for (uint32_t i = 0; i < count; i++) {
					if (!done[i] && (pass == 1 || waiting[i] == 0) && (next < 0 || order(p_items[i], p_items[next]))) {
						next = i;
					}
				}
			}
			done[next] = true;
			for (const uint32_t j : after[next]) {
				waiting[j]--;
			}
			result.push_back(p_items[next]);
		}
		return result;
	}

	void build(const Dictionary &p_rig, const String &p_name) {
		const Array constraints = p_rig.get("constraints", Array());
		LocalVector<RigItem> items;
		for (int i = 0; i < constraints.size(); i++) {
			if (constraints[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			RigItem item;
			item.constraint = constraints[i];
			item.order = i;
			const String type = item.constraint.get("type", String());
			const String owner_object = item.constraint.get("owner_object", String());
			if (!owner_object.is_empty()) {
				// An object that follows the armature runs after its bones.
				item.owner_node = object_node(owner_object);
				item.owner_label = owner_object;
				item.depth = INT32_MAX;
				if (!item.owner_node) {
					(bool(item.constraint.get("muted", false)) ? missing : skipped).push_back(vformat("%s on %s (the object isn't in the import)", type, owner_object));
					continue;
				}
			} else {
				const String bone_name = item.constraint.get("bone", String());
				item.bone = skeleton->find_bone(bone_name);
				item.owner_label = bone_name;
				if (item.bone < 0) {
					(bool(item.constraint.get("muted", false)) ? missing : skipped).push_back(vformat("%s on %s (the bone isn't in the import)", type, bone_name));
					continue;
				}
				for (int parent = skeleton->get_bone_parent(item.bone); parent >= 0; parent = skeleton->get_bone_parent(parent)) {
					item.depth++;
				}
				item.modifies.push_back(item.bone);
			}
			for (const char *key : { "target", "pole" }) {
				const int read = bone_of(item.constraint.get(key, Variant()));
				if (read >= 0) {
					item.reads.push_back(read);
				}
			}
			if (type == "IK" && item.bone >= 0) {
				// IK sets its whole chain, and a stand-in pole rides with a bone.
				const int chain = item.constraint.get("chain_count", 0);
				int bone = skeleton->get_bone_parent(item.bone);
				for (int i = 1; bone >= 0 && (chain <= 0 || i < chain); i++) {
					item.modifies.push_back(bone);
					bone = skeleton->get_bone_parent(bone);
				}
				const int pole_parent = skeleton->find_bone(item.constraint.get("pole_parent", String()));
				if (pole_parent >= 0) {
					item.reads.push_back(pole_parent);
				}
			}
			items.push_back(item);
		}
		for (const RigItem &item : sorted(items)) {
			const String type = item.constraint.get("type", String());
			const int constraint_type = type_of(type);
			if (type == "IK" && item.bone >= 0) {
				ik(item.constraint, item.bone, item.constraint.get("influence", 1.0));
			} else if (constraint_type >= 0) {
				blender_constraint(item, constraint_type);
			} else {
				const String reason = item.constraint.get("reason", String());
				skipped.push_back(vformat("%s on %s", item.constraint.get("name", type), item.owner_label) + (reason.begins_with("Asosdot could not") ? " (" + reason + ")" : String()));
			}
		}
		if (!missing.is_empty()) {
			ERR_PRINT(vformat("Blender rig \"%s\": these constraints were muted for the export but could not be rebuilt, so they have no effect: %s.", p_name, String("; ").join(missing)));
		}
		if (!skipped.is_empty()) {
			WARN_PRINT(vformat("Blender rig \"%s\": these constraints have no Asosdot equivalent; Blender baked them into the animation: %s.", p_name, String("; ").join(skipped)));
		}
		if (!baked.is_empty()) {
			WARN_PRINT(vformat("Blender rig \"%s\": these constraints stay baked into the animation, so their BlenderConstraint3D nodes were added switched off: %s.", p_name, String("; ").join(baked)));
		}
		if (!approximate.is_empty()) {
			WARN_PRINT(vformat("Blender rig \"%s\": rebuilt with differences from Blender: %s.", p_name, String("; ").join(approximate)));
		}
	}
};

Skeleton3D *AsosdotGLTFDocumentExtensionBlenderRig::_find_skeleton(Ref<GLTFState> p_state, GLTFNodeIndex p_armature) {
	if (Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_state->get_scene_node(p_armature))) {
		return skeleton;
	}
	const Vector<Ref<GLTFNode>> &nodes = p_state->get_nodes();
	const Vector<Ref<GLTFSkeleton>> &skeletons = p_state->get_skeletons();
	for (const GLTFNodeIndex child : nodes[p_armature]->get_children()) {
		const GLTFSkeletonIndex index = nodes[child]->get_skeleton();
		if (index >= 0 && index < skeletons.size()) {
			return skeletons[index]->get_godot_skeleton();
		}
	}
	return nullptr;
}

Error AsosdotGLTFDocumentExtensionBlenderRig::import_post(Ref<GLTFState> p_state, Node *p_root) {
	ERR_FAIL_COND_V(p_state.is_null(), ERR_INVALID_PARAMETER);
	ERR_FAIL_NULL_V(p_root, ERR_INVALID_PARAMETER);
	const Vector<Ref<GLTFNode>> &nodes = p_state->get_nodes();
	for (GLTFNodeIndex i = 0; i < nodes.size(); i++) {
		if (!nodes[i]->has_meta(SNAME("extras"))) {
			continue;
		}
		const Dictionary extras = nodes[i]->get_meta(SNAME("extras"));
		if (!extras.has("asosdot_rig")) {
			continue;
		}
		const Variant rig = JSON::parse_string(extras["asosdot_rig"]);
		Skeleton3D *skeleton = _find_skeleton(p_state, i);
		if (rig.get_type() != Variant::DICTIONARY || !skeleton) {
			WARN_PRINT(vformat("Blender rig \"%s\" was not rebuilt: its skeleton or constraint data is missing.", nodes[i]->get_original_name()));
			continue;
		}
		Builder builder;
		builder.state = p_state;
		builder.skeleton = skeleton;
		builder.owner = p_root;
		builder.build(rig, nodes[i]->get_original_name());
	}
	return OK;
}
