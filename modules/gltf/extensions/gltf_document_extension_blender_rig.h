/**************************************************************************/
/*  gltf_document_extension_blender_rig.h                                 */
/**************************************************************************/
/*  Asosdot: Blender IK rigs rebuilt as skeleton modifiers.               */
/**************************************************************************/

#pragma once

#include "gltf_document_extension.h"

class Node3D;
class Skeleton3D;

// Rebuilds a Blender armature's bone constraints as skeleton modifiers, so a
// rig animated with controls in Blender is animated the same way here. The
// Blender import writes the constraints to the armature's extras as
// "asosdot_rig", with the constraints of objects that follow the armature. Two-
// bone IK becomes a TwoBoneIK3D, longer chains a FABRIK3D, and Copy, Limit,
// Track, Child Of, Stretch To, Transformation and Maintain Volume constraints a
// BlenderConstraint3D each, in Blender's evaluation order. The import mutes the
// constraints rebuilt exactly, so the animation carries the poses before them.
class AsosdotGLTFDocumentExtensionBlenderRig : public GLTFDocumentExtension {
	GDCLASS(AsosdotGLTFDocumentExtensionBlenderRig, GLTFDocumentExtension);

	struct Builder;

	static Skeleton3D *_find_skeleton(Ref<GLTFState> p_state, GLTFNodeIndex p_armature);

public:
	Error import_post(Ref<GLTFState> p_state, Node *p_root) override;
};
