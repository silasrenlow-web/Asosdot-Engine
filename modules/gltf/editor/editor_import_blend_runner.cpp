/**************************************************************************/
/*  editor_import_blend_runner.cpp                                        */
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

#include "editor_import_blend_runner.h"

#include "core/io/http_client.h"
#include "core/io/xml_parser.h"
#include "core/object/callable_mp.h"
#include "core/os/os.h"
#include "editor/editor_node.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/settings/editor_settings.h"

// Asosdot: before the export, each armature's bone constraints are kept as a
// custom property, which the glTF export writes to the armature's extras.
// AsosdotGLTFDocumentExtensionBlenderRig rebuilds them as skeleton modifiers.
static constexpr char PYTHON_STORE_RIGS[] = R"(
import bpy, json, math, mathutils, sys
def asosdot_store_rigs(gltf_options):
  def reference(owner, target, subtarget):
    if target is None:
      return None
    if target == owner:
      return {'bone': subtarget} if subtarget else None
    return {'object': target.name, 'bone': subtarget or ''}
  def armature_point(owner, evaluated, target, subtarget):
    if target == owner:
      return evaluated.pose.bones[subtarget].head.copy()
    matrix = target.matrix_world.copy()
    if target.type == 'ARMATURE' and subtarget:
      matrix = matrix @ target.pose.bones[subtarget].matrix
    return owner.matrix_world.inverted() @ matrix.translation
  def solved_bend(owner, pbone, c):
    # Blender's own solve of a two-bone chain, bent by moving its target toward the
    # root. Blender keeps one direction of the middle bone facing the pole; Asosdot's
    # IK turns that same direction to the pole, so the bones twist alike.
    info = {}
    root = pbone.parent
    if root is None:
      return info
    moved = None
    if c.target == owner and c.subtarget in owner.pose.bones:
      moved = owner.pose.bones[c.subtarget]
      saved = moved.matrix_basis.copy()
      matrix = moved.matrix.copy()
      matrix.translation = root.bone.head_local.lerp(pbone.bone.tail_local, 0.7)
      moved.matrix = matrix
    try:
      bpy.context.view_layer.update()
      evaluated = owner.evaluated_get(bpy.context.evaluated_depsgraph_get())
      r = evaluated.pose.bones[root.name]
      m = evaluated.pose.bones[pbone.name]
      rh, mh, th = r.head.copy(), m.head.copy(), m.tail.copy()
      axis = th - rh
      if axis.length < 1e-6:
        return info
      axis.normalize()
      bend = (mh - rh) - axis * (mh - rh).dot(axis)
      forward = (th - mh).normalized()
      if c.pole_target is not None:
        pole = armature_point(owner, evaluated, c.pole_target, c.pole_subtarget)
        side = (pole - rh) - axis * (pole - rh).dot(axis)
        if side.length > 1e-6:
          k = axis.cross(side).cross(forward)
          if k.dot(side) < 0:
            k = -k
          if k.length > 1e-6:
            info['pole_vector'] = list(m.matrix.to_quaternion().to_matrix().inverted() @ k.normalized())
          # The root bone keeps its own side to the pole as well: the chain is a hinge.
          kr = axis.cross(side).cross((mh - rh).normalized())
          if kr.dot(side) < 0:
            kr = -kr
          if kr.length > 1e-6:
            info['root_pole_vector'] = list(r.matrix.to_quaternion().to_matrix().inverted() @ kr.normalized())
          if bend.length > 1e-6:
            info['pole_offset'] = bend.angle(side)
      elif bend.length > 1e-6:
        point = mh + bend.normalized() * ((mh - rh).length + (th - mh).length)
        if r.parent is not None:
          info['pole_parent'] = r.parent.name
          info['pole_point'] = list(r.parent.matrix.inverted() @ point)
        else:
          info['pole_point'] = list(point)
      return info
    finally:
      if moved is not None:
        moved.matrix_basis = saved
        bpy.context.view_layer.update()
  euler_orders = {'XYZ', 'XZY', 'YXZ', 'YZX', 'ZXY', 'ZYX'}
  # Constraints that Asosdot's BlenderConstraint3D evaluates with Blender's own math.
  rebuilt = {'COPY_LOCATION', 'COPY_ROTATION', 'COPY_SCALE', 'COPY_TRANSFORMS', 'LIMIT_LOCATION', 'LIMIT_ROTATION', 'LIMIT_SCALE', 'CHILD_OF', 'DAMPED_TRACK', 'TRACK_TO', 'LOCKED_TRACK', 'STRETCH_TO', 'TRANSFORM', 'MAINTAIN_VOLUME'}
  untargeted = {'LIMIT_LOCATION', 'LIMIT_ROTATION', 'LIMIT_SCALE', 'MAINTAIN_VOLUME'}
  bone_spaces = {'WORLD', 'POSE', 'LOCAL_WITH_PARENT', 'LOCAL'}
  skip = {'rna_type', 'name', 'type', 'mute', 'enabled', 'active', 'show_expanded', 'is_valid', 'is_override_data_editable', 'error_location', 'error_rotation', 'influence', 'owner_space', 'target_space', 'target', 'subtarget', 'pole_target', 'pole_subtarget', 'space_object', 'space_subtarget', 'head_tail', 'use_bbone_shape', 'set_inverse_pending'}
  def plain(value):
    if isinstance(value, (str, bool, int)):
      return value
    if isinstance(value, float):
      return value if math.isfinite(value) else 0.0
    if isinstance(value, mathutils.Matrix):
      return [plain(list(row)) for row in value]
    if isinstance(value, set):
      return sorted(value)
    try:
      return [plain(item) for item in value]
    except TypeError:
      return None
  def settings_of(c):
    result = {}
    for prop in c.bl_rna.properties:
      if prop.identifier in skip or prop.type in ('POINTER', 'COLLECTION'):
        continue
      try:
        result[prop.identifier] = plain(getattr(c, prop.identifier))
      except Exception:
        pass
    if c.type == 'LIMIT_ROTATION' and not hasattr(c, 'use_legacy_behavior'):
      # Before Blender 4.2, Limit Rotation clamped each angle plainly.
      result['use_legacy_behavior'] = True
    return result
  def rotation_order(owner):
    mode = getattr(owner, 'rotation_mode', 'XYZ')
    return mode if mode in euler_orders else 'XYZ'
  def plain_parenting(bone):
    return bone.use_inherit_rotation and getattr(bone, 'inherit_scale', 'FULL') == 'FULL' and bone.use_local_location and not bone.use_relative_parent
  def exported(o):
    try:
      if o.name not in bpy.context.scene.objects:
        return False
      if gltf_options.get('use_selection') and not o.select_get():
        return False
      if gltf_options.get('use_visible') and not o.visible_get():
        return False
      if gltf_options.get('use_renderable') and o.hide_render:
        return False
    except Exception:
      return False
    return True
  # Paths that actions or drivers animate, and bones that drivers read. Muting a
  # constraint changes what those see, so their constraints stay as Blender bakes them.
  animated = set()
  driver_bones = set()
  def bones_in_path(path):
    # Bone names in a data path such as pose.bones[...].rotation_euler. The script
    # has no backslashes or double quotes, which the command line running it rewrites.
    names = []
    start = path.find('pose.bones[')
    while start >= 0:
      quote = path[start + 11:start + 12]
      i = start + 12
      name = ''
      while i < len(path) and path[i] != quote:
        if path[i] == chr(92):
          i += 1
        name += path[i:i + 1]
        i += 1
      names.append(name)
      start = path.find('pose.bones[', i)
    return names
  for action in bpy.data.actions:
    curves = []
    for layer in getattr(action, 'layers', []):
      for strip in layer.strips:
        for bag in getattr(strip, 'channelbags', []):
          curves.extend(bag.fcurves)
    if not curves:
      try:
        curves = list(action.fcurves)
      except Exception:
        pass
    animated.update(curve.data_path for curve in curves)
  for collection in (bpy.data.objects, bpy.data.shape_keys, bpy.data.armatures, bpy.data.meshes, bpy.data.materials):
    for item in collection:
      data = getattr(item, 'animation_data', None)
      if data is None:
        continue
      for driver in data.drivers:
        animated.add(driver.data_path)
        for variable in driver.driver.variables:
          for target in variable.targets:
            if target.id is None or getattr(target.id, 'type', None) != 'ARMATURE':
              continue
            if getattr(target, 'bone_target', ''):
              driver_bones.add((target.id.name, target.bone_target))
            for bone in bones_in_path(target.data_path or ''):
              driver_bones.add((target.id.name, bone))
  def is_animated(c):
    prefix = c.path_from_id()
    return any(path.startswith(prefix + '.') for path in animated)
  armatures = {obj.name: obj for obj in bpy.data.objects if obj.type == 'ARMATURE' and obj.pose is not None}
  rigs = {name: [] for name in armatures}
  records = []
  def chain_of(pbone, c):
    bones = [pbone.name]
    parent = pbone.parent
    while parent is not None and (c.chain_count == 0 or len(bones) < c.chain_count):
      bones.append(parent.name)
      parent = parent.parent
    return bones
  def read_bones(c, rig):
    bones = []
    for target, sub in ((getattr(c, 'target', None), getattr(c, 'subtarget', '')), (getattr(c, 'pole_target', None), getattr(c, 'pole_subtarget', ''))):
      if target == rig and sub:
        bones.append(sub)
    return bones
  def inexact(c, rig, owner, pbone):
    if c.type not in rebuilt:
      return 'Asosdot has no exact equivalent'
    if is_animated(c):
      return 'its influence or settings are animated or driven'
    if pbone is None and c.owner_space != 'WORLD':
      return 'an object owner in ' + c.owner_space + ' space'
    if pbone is not None and c.owner_space not in bone_spaces:
      return 'owner space ' + c.owner_space
    if pbone is not None and c.owner_space == 'LOCAL' and not plain_parenting(pbone.bone):
      return 'local space of a bone that does not fully inherit its parent'
    if c.type == 'TRANSFORM' and c.map_from == 'ROTATION' and c.from_rotation_mode.startswith('SWING'):
      return 'swing and twist rotation input'
    if c.type == 'STRETCH_TO' and c.rest_length == 0:
      return 'no rest length'
    if c.type in untargeted:
      return None
    target, sub = c.target, getattr(c, 'subtarget', '')
    if target is None:
      return 'no target'
    if target.type == 'ARMATURE' and sub:
      if target != rig:
        return 'a bone target in another armature'
      if c.target_space not in bone_spaces:
        return 'target space ' + c.target_space
      tbone = target.pose.bones.get(sub)
      if tbone is None:
        return 'a missing target bone'
      if c.target_space == 'LOCAL' and not plain_parenting(tbone.bone):
        return 'local space of a target bone that does not fully inherit its parent'
      if getattr(c, 'head_tail', 0.0) > 0.000001 and getattr(c, 'use_bbone_shape', False) and tbone.bone.bbone_segments > 1:
        return 'a B-Bone shape target'
      return None
    if c.target_space != 'WORLD':
      return 'an object target in ' + c.target_space + ' space'
    if not exported(target):
      return 'a target object that is not exported'
    return None
  def entry_of(c, rig, owner, pbone):
    entry = {'type': c.type, 'name': c.name, 'influence': c.influence, 'owner_space': getattr(c, 'owner_space', 'WORLD'), 'target_space': getattr(c, 'target_space', 'WORLD')}
    if pbone is not None:
      entry['bone'] = pbone.name
    else:
      entry['owner_object'] = owner.name
    entry['owner_rotation_order'] = rotation_order(pbone if pbone is not None else owner)
    target = getattr(c, 'target', None)
    if target is not None:
      sub = getattr(c, 'subtarget', '')
      entry['target'] = {'bone': sub} if target == rig and sub else {'object': target.name, 'bone': sub or ''}
      tbone = target.pose.bones.get(sub) if target.type == 'ARMATURE' and sub else None
      entry['target_rotation_order'] = rotation_order(tbone if tbone is not None else target)
      if tbone is not None:
        entry['head_tail'] = getattr(c, 'head_tail', 0.0)
        entry['target_length'] = tbone.bone.length
    if c.type in rebuilt:
      entry['settings'] = settings_of(c)
    return entry
  def collect(c, rig, owner, pbone, index, ik_chains):
    if c.mute or not c.is_valid:
      return
    try:
      entry = entry_of(c, rig, owner, pbone)
      if c.type == 'IK':
        entry['pole'] = reference(rig, c.pole_target, c.pole_subtarget)
        entry['chain_count'] = c.chain_count
        entry['use_tail'] = c.use_tail
        entry['use_rotation'] = c.use_rotation
        entry['length'] = pbone.bone.length
        if c.chain_count == 2 and c.use_tail:
          entry.update(solved_bend(rig, pbone, c))
      reason = inexact(c, rig, owner, pbone)
      if reason is None and pbone is not None and pbone.name in ik_chains:
        reason = 'on a bone of the IK chain of ' + ik_chains[pbone.name]
      ik = c.type == 'IK'
    except Exception as error:
      # Left as Blender bakes it, and named in the import's output.
      entry = {'type': c.type, 'name': c.name}
      entry.update({'bone': pbone.name} if pbone is not None else {'owner_object': owner.name})
      reason = 'Asosdot could not read it: ' + str(error)
      ik = False
    rigs[rig.name].append(entry)
    records.append({'entry': entry, 'constraint': c, 'rig': rig, 'owner': ('bone', pbone.name) if pbone is not None else ('object', owner.name), 'index': index, 'reason': reason, 'reads': read_bones(c, rig), 'ik': ik})
  for name, obj in armatures.items():
    ik_chains = {}
    for pbone in obj.pose.bones:
      for c in pbone.constraints:
        if c.type == 'IK' and not c.mute:
          for bone in chain_of(pbone, c):
            ik_chains[bone] = pbone.name
    for pbone in obj.pose.bones:
      for index, c in enumerate(pbone.constraints):
        collect(c, obj, obj, pbone, index, ik_chains)
  # Objects whose constraints follow an armature, such as a prop that copies a hand.
  for obj in bpy.data.objects:
    host = None
    for c in obj.constraints:
      target = getattr(c, 'target', None)
      if not c.mute and target is not None and target.name in armatures and target != obj:
        host = target
        break
    if host is not None:
      for index, c in enumerate(obj.constraints):
        collect(c, host, obj, None, index, {})
  # A constraint is muted for the export, and evaluated live by Asosdot, when muting it
  # changes nothing that stays baked: no baked constraint after it in its stack, and
  # none on, or reading, its bone or a bone below it.
  tainted = set(driver_bones)
  for r in records:
    if r['reason'] is None or r['ik']:
      continue
    if r['owner'][0] == 'bone':
      tainted.add((r['rig'].name, r['owner'][1]))
    for bone in r['reads']:
      tainted.add((r['rig'].name, bone))
  def below(rig, ancestor, bone):
    pbone = rig.pose.bones.get(bone)
    while pbone is not None:
      if pbone.name == ancestor:
        return True
      pbone = pbone.parent
    return False
  stacks = {}
  for r in records:
    stacks.setdefault((r['rig'].name,) + r['owner'], []).append(r)
  for stack in stacks.values():
    stack.sort(key=lambda r: r['index'])
    blocked = None
    for r in reversed(stack):
      entry = r['entry']
      if r['ik']:
        blocked = blocked or 'IK after it on the same bone'
        continue
      if r['reason'] is not None:
        entry['reason'] = r['reason']
        blocked = blocked or (entry['name'] + ' after it stays baked')
        continue
      if blocked is not None:
        entry['reason'] = blocked
        continue
      if r['owner'][0] == 'bone':
        hit = next((bone for rig_name, bone in tainted if rig_name == r['rig'].name and below(r['rig'], r['owner'][1], bone)), None)
        if hit is not None:
          entry['reason'] = 'muting it would change ' + hit + ', which stays baked or drives something'
          blocked = entry['reason']
          continue
      entry['muted'] = True
  for name, entries in rigs.items():
    if entries:
      armatures[name]['asosdot_rig'] = json.dumps({'version': 2, 'constraints': entries})
  # Muted last, after everything above has read the pose with them on.
  for r in records:
    if r['entry'].get('muted'):
      r['constraint'].mute = True
)";

static constexpr char PYTHON_SCRIPT_RPC[] = R"(
import bpy, sys, threading
from xmlrpc.server import SimpleXMLRPCServer
req = threading.Condition()
res = threading.Condition()
info = None
export_err = None
def xmlrpc_server():
  server = SimpleXMLRPCServer(('127.0.0.1', %d))
  server.register_function(export_gltf)
  server.serve_forever()
def export_gltf(opts):
  with req:
    global info
    info = ('export_gltf', opts)
    req.notify()
  with res:
    res.wait()
  if export_err:
    raise export_err
  # Important to return a value to prevent the error 'cannot marshal None unless allow_none is enabled'.
  return 'BLENDER_GODOT_EXPORT_SUCCESSFUL'
if bpy.app.version < (3, 0, 0):
  print('Blender 3.0 or higher is required.', file=sys.stderr)
threading.Thread(target=xmlrpc_server).start()
while True:
  with req:
    while info is None:
      req.wait()
  method, opts = info
  if method == 'export_gltf':
    try:
      export_err = None
      bpy.ops.wm.open_mainfile(filepath=opts['path'])
      if opts['unpack_all']:
        bpy.ops.file.unpack_all(method='USE_LOCAL')
      # Applying modifiers can discard shape keys, even when the only modifier is the armature.
      if opts.get('preserve_shape_keys', False) and any(obj.type == 'MESH' and obj.data.shape_keys for obj in bpy.context.scene.objects):
        opts['gltf_options']['export_apply'] = False
      if opts.get('rig_constraints', False):
        asosdot_store_rigs(opts['gltf_options'])
      bpy.ops.export_scene.gltf(**opts['gltf_options'])
    except Exception as e:
      export_err = e
  info = None
  with res:
    res.notify()
)";

static constexpr char PYTHON_SCRIPT_DIRECT[] = R"(
import bpy, sys
opts = %s
if bpy.app.version < (3, 0, 0):
  print('Blender 3.0 or higher is required.', file=sys.stderr)
bpy.ops.wm.open_mainfile(filepath=opts['path'])
if opts['unpack_all']:
  bpy.ops.file.unpack_all(method='USE_LOCAL')
# Applying modifiers can discard shape keys, even when the only modifier is the armature.
if opts.get('preserve_shape_keys', False) and any(obj.type == 'MESH' and obj.data.shape_keys for obj in bpy.context.scene.objects):
  opts['gltf_options']['export_apply'] = False
if opts.get('rig_constraints', False):
  asosdot_store_rigs(opts['gltf_options'])
bpy.ops.export_scene.gltf(**opts['gltf_options'])
)";

String dict_to_python(const Dictionary &p_dict) {
	String entries;
	for (const KeyValue<Variant, Variant> &kv : p_dict) {
		const String &key = kv.key;
		String value;
		const Variant &raw_value = kv.value;

		switch (raw_value.get_type()) {
			case Variant::Type::BOOL: {
				value = raw_value ? "True" : "False";
				break;
			}
			case Variant::Type::STRING:
			case Variant::Type::STRING_NAME: {
				value = raw_value;
				value = vformat("'%s'", value.c_escape());
				break;
			}
			case Variant::Type::DICTIONARY: {
				value = dict_to_python(raw_value);
				break;
			}
			default: {
				ERR_FAIL_V_MSG("", vformat("Unhandled Variant type %s for python dictionary", Variant::get_type_name(raw_value.get_type())));
			}
		}

		entries += vformat("'%s': %s,", key, value);
	}
	return vformat("{%s}", entries);
}

String dict_to_xmlrpc(const Dictionary &p_dict) {
	String members;
	for (const KeyValue<Variant, Variant> &kv : p_dict) {
		const String &key = kv.key;
		String value;
		const Variant &raw_value = kv.value;

		switch (raw_value.get_type()) {
			case Variant::Type::BOOL: {
				value = vformat("<boolean>%d</boolean>", raw_value ? 1 : 0);
				break;
			}
			case Variant::Type::STRING:
			case Variant::Type::STRING_NAME: {
				value = raw_value;
				value = vformat("<string>%s</string>", value.xml_escape());
				break;
			}
			case Variant::Type::DICTIONARY: {
				value = dict_to_xmlrpc(raw_value);
				break;
			}
			default: {
				ERR_FAIL_V_MSG("", vformat("Unhandled Variant type %s for XMLRPC", Variant::get_type_name(raw_value.get_type())));
			}
		}

		members += vformat("<member><name>%s</name><value>%s</value></member>", key, value);
	}
	return vformat("<struct>%s</struct>", members);
}

Error EditorImportBlendRunner::start_blender(const String &p_python_script, bool p_blocking) {
	String blender_path = EDITOR_GET("filesystem/import/blender/blender_path");

	List<String> args;
	args.push_back("--background");
	args.push_back("--python-exit-code");
	args.push_back("1");
	args.push_back("--python-expr");
	args.push_back(p_python_script);

	Error err;
	String str;
	if (p_blocking) {
		int exitcode = 0;
		err = OS::get_singleton()->execute(blender_path, args, &str, &exitcode, true);
		if (exitcode != 0) {
			print_error(vformat("Blender import failed: %s.", str));
			return FAILED;
		}
	} else {
		err = OS::get_singleton()->create_process(blender_path, args, &blender_pid);
	}
	return err;
}

bool EditorImportBlendRunner::is_running() {
	return blender_pid != 0 && OS::get_singleton()->is_process_running(blender_pid);
}

Error EditorImportBlendRunner::do_import(const Dictionary &p_options) {
	if (is_using_rpc()) {
		Error err = do_import_rpc(p_options);
		if (err != OK) {
			// Retry without using RPC (slow, but better than the import failing completely).
			if (err == ERR_CONNECTION_ERROR) {
				// Disable RPC if the connection could not be established.
				print_error(vformat("Failed to connect to Blender via RPC, switching to direct imports of .blend files. Check your proxy and firewall settings, then RPC can be re-enabled by changing the editor setting `filesystem/import/blender/rpc_port` to %d.", rpc_port));
				EditorSettings::get_singleton()->set_manually("filesystem/import/blender/rpc_port", 0);
				rpc_port = 0;
			}
			if (err != ERR_QUERY_FAILED) {
				err = do_import_direct(p_options);
			}
		}
		return err;
	} else {
		return do_import_direct(p_options);
	}
}

HTTPClient::Status EditorImportBlendRunner::connect_blender_rpc(const Ref<HTTPClient> &p_client, int p_timeout_usecs) {
	p_client->connect_to_host("127.0.0.1", rpc_port);
	HTTPClient::Status status = p_client->get_status();

	int attempts = 1;
	int wait_usecs = 1000;

	bool done = false;
	while (!done) {
		OS::get_singleton()->delay_usec(wait_usecs);
		attempts++;
		status = p_client->get_status();
		switch (status) {
			case HTTPClient::STATUS_RESOLVING:
			case HTTPClient::STATUS_CONNECTING: {
				p_client->poll();
				break;
			}
			case HTTPClient::STATUS_CONNECTED: {
				done = true;
				break;
			}
			default: {
				if (attempts * wait_usecs < p_timeout_usecs) {
					p_client->connect_to_host("127.0.0.1", rpc_port);
				} else {
					return status;
				}
			}
		}
	}

	return status;
}

Error EditorImportBlendRunner::do_import_rpc(const Dictionary &p_options) {
	kill_timer->stop();

	// Start Blender if not already running.
	if (!is_running()) {
		// Start an XML RPC server on the given port.
		String python = String(PYTHON_STORE_RIGS) + vformat(PYTHON_SCRIPT_RPC, rpc_port);
		Error err = start_blender(python, false);
		if (err != OK || blender_pid == 0) {
			return FAILED;
		}
	}

	// Convert options to XML body.
	String xml_options = dict_to_xmlrpc(p_options);
	String xml_body = vformat("<?xml version=\"1.0\"?><methodCall><methodName>export_gltf</methodName><params><param><value>%s</value></param></params></methodCall>", xml_options);

	// Connect to RPC server.
	Ref<HTTPClient> client = HTTPClient::create();
	HTTPClient::Status status = connect_blender_rpc(client, 1000000);
	if (status != HTTPClient::STATUS_CONNECTED) {
		ERR_FAIL_V_MSG(ERR_CONNECTION_ERROR, vformat("Unexpected status during RPC connection: %d", status));
	}

	// Send XML request.
	PackedByteArray xml_buffer = xml_body.to_utf8_buffer();
	Error err = client->request(HTTPClient::METHOD_POST, "/", Vector<String>(), xml_buffer.ptr(), xml_buffer.size());
	if (err != OK) {
		ERR_FAIL_V_MSG(err, vformat("Unable to send RPC request: %d", err));
	}

	// Wait for response.
	bool done = false;
	PackedByteArray response;
	while (!done) {
		status = client->get_status();
		switch (status) {
			case HTTPClient::STATUS_REQUESTING: {
				client->poll();
				break;
			}
			case HTTPClient::STATUS_BODY: {
				client->poll();
				response.append_array(client->read_response_body_chunk());
				break;
			}
			case HTTPClient::STATUS_CONNECTED: {
				done = true;
				break;
			}
			default: {
				ERR_FAIL_V_MSG(ERR_CONNECTION_ERROR, vformat("Unexpected status during RPC response: %d", status));
			}
		}
	}

	String response_text = "No response from Blender.";
	if (response.size() > 0) {
		response_text = String::utf8((const char *)response.ptr(), response.size());
	}

	if (client->get_response_code() != HTTPClient::RESPONSE_OK) {
		ERR_FAIL_V_MSG(ERR_QUERY_FAILED, vformat("Error received from Blender - status code: %s, error: %s", client->get_response_code(), response_text));
	} else if (response_text.find("BLENDER_GODOT_EXPORT_SUCCESSFUL") < 0) {
		// Previous versions of Godot used a Python script where the RPC function did not return
		// a value, causing the error 'cannot marshal None unless allow_none is enabled'.
		// If an older version of Godot is running and has started Blender with this script,
		// we will receive the error, but there's a good chance that the import was successful.
		// We are discarding this error to maintain backward compatibility and prevent situations
		// where the user needs to close the older version of Godot or kill Blender.
		if (response_text.find("cannot marshal None unless allow_none is enabled") < 0) {
			String error_message;
			if (_extract_error_message_xml(response, error_message)) {
				ERR_FAIL_V_MSG(ERR_QUERY_FAILED, vformat("Blender exportation failed: %s", error_message));
			} else {
				ERR_FAIL_V_MSG(ERR_QUERY_FAILED, vformat("Blender exportation failed: %s", response_text));
			}
		}
	}

	return OK;
}

bool EditorImportBlendRunner::_extract_error_message_xml(const Vector<uint8_t> &p_response_data, String &r_error_message) {
	// Based on RPC Xml spec from: https://xmlrpc.com/spec.md
	Ref<XMLParser> parser = memnew(XMLParser);
	Error err = parser->open_buffer(p_response_data);
	if (err) {
		return false;
	}

	r_error_message = String();
	while (parser->read() == OK) {
		if (parser->get_node_type() == XMLParser::NODE_TEXT) {
			if (parser->get_node_data().size()) {
				if (r_error_message.size()) {
					r_error_message += " ";
				}
				r_error_message += parser->get_node_data().trim_suffix("\n");
			}
		}
	}

	return r_error_message.size();
}

Error EditorImportBlendRunner::do_import_direct(const Dictionary &p_options) {
	// Export glTF directly.
	String python = String(PYTHON_STORE_RIGS) + vformat(PYTHON_SCRIPT_DIRECT, dict_to_python(p_options));
	Error err = start_blender(python, true);
	if (err != OK) {
		return err;
	}

	return OK;
}

void EditorImportBlendRunner::_resources_reimported(const PackedStringArray &p_files) {
	if (is_running()) {
		// After a batch of imports is done, wait a few seconds before trying to kill blender,
		// in case of having multiple imports trigger in quick succession.
		kill_timer->start();
	}
}

void EditorImportBlendRunner::_kill_blender() {
	kill_timer->stop();
	if (is_running()) {
		OS::get_singleton()->kill(blender_pid);
	}
	blender_pid = 0;
}

void EditorImportBlendRunner::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_PREDELETE: {
			_kill_blender();
			break;
		}
	}
}

EditorImportBlendRunner *EditorImportBlendRunner::singleton = nullptr;

EditorImportBlendRunner::EditorImportBlendRunner() {
	ERR_FAIL_COND_MSG(singleton != nullptr, "EditorImportBlendRunner already created.");
	singleton = this;

	rpc_port = EDITOR_GET("filesystem/import/blender/rpc_port");

	kill_timer = memnew(Timer);
	add_child(kill_timer);
	kill_timer->set_one_shot(true);
	kill_timer->set_wait_time(EDITOR_GET("filesystem/import/blender/rpc_server_uptime"));
	kill_timer->connect("timeout", callable_mp(this, &EditorImportBlendRunner::_kill_blender));

	EditorFileSystem::get_singleton()->connect("resources_reimported", callable_mp(this, &EditorImportBlendRunner::_resources_reimported));
}
