/**************************************************************************/
/*  global_transmit.h                                                     */
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
/* the following conditions:                                             */
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
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/variant/array.h"
#include "core/variant/callable.h"

class Node;

// Asosdot: Global Transmit. Transmissions go out on channels, a name or a number, to whoever
// listens on that channel: any callable, from code, from a Transmit track in an animation, or
// from a node set up in the editor's Transmit tab. Sender and receivers never need to know of
// each other.
class GlobalTransmit : public Object {
	GDCLASS(GlobalTransmit, Object);

	static GlobalTransmit *singleton;

	struct Channel {
		LocalVector<Callable> listeners;
		Array last_arguments;
		uint64_t count = 0;
	};
	HashMap<StringName, Channel> named;
	HashMap<int64_t, Channel> numbered;
	int depth = 0;

	Channel *_get_channel(const Variant &p_channel, bool p_create);
	const Channel *_get_channel_const(const Variant &p_channel) const;
	void _deliver(const Variant &p_channel, const Variant **p_args, int p_argcount);
	Error _transmit(const Variant **p_args, int p_argcount, Callable::CallError &r_error);
	Error _transmit_deferred(const Variant **p_args, int p_argcount, Callable::CallError &r_error);

protected:
	static void _bind_methods();

public:
	static constexpr int MAX_DEPTH = 64;

	static GlobalTransmit *get_singleton() { return singleton; }
	static bool is_valid_channel(const Variant &p_channel);

	void transmit_array(const Variant &p_channel, const Array &p_arguments);

	void listen(const Variant &p_channel, const Callable &p_callable);
	void stop_listening(const Variant &p_channel, const Callable &p_callable);
	bool is_listening(const Variant &p_channel, const Callable &p_callable) const;
	Array get_listeners(const Variant &p_channel) const;

	Array get_arguments(const Variant &p_channel) const;
	int get_transmission_count(const Variant &p_channel) const;
	Array get_channels() const;
	void clear_channel(const Variant &p_channel);

	// Receivers set up in the editor's Transmit tab, stored on the node.
	void _node_entered(Node *p_node);
	void _node_exited(Node *p_node);

	GlobalTransmit();
	~GlobalTransmit();
};
