/**************************************************************************/
/*  global_transmit.cpp                                                   */
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

#include "global_transmit.h"

#include "core/config/engine.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "scene/main/node.h"

GlobalTransmit *GlobalTransmit::singleton = nullptr;

bool GlobalTransmit::is_valid_channel(const Variant &p_channel) {
	return p_channel.is_string() || p_channel.get_type() == Variant::INT;
}

GlobalTransmit::Channel *GlobalTransmit::_get_channel(const Variant &p_channel, bool p_create) {
	if (p_channel.get_type() == Variant::INT) {
		const int64_t number = p_channel;
		Channel *found = numbered.getptr(number);
		if (!found && p_create) {
			found = &numbered.insert(number, Channel())->value;
		}
		return found;
	}
	const StringName name = p_channel;
	Channel *found = named.getptr(name);
	if (!found && p_create) {
		found = &named.insert(name, Channel())->value;
	}
	return found;
}

const GlobalTransmit::Channel *GlobalTransmit::_get_channel_const(const Variant &p_channel) const {
	if (p_channel.get_type() == Variant::INT) {
		return numbered.getptr((int64_t)p_channel);
	}
	return named.getptr(StringName(p_channel));
}

void GlobalTransmit::_deliver(const Variant &p_channel, const Variant **p_args, int p_argcount) {
	ERR_FAIL_COND_MSG(!is_valid_channel(p_channel), "A transmission's channel must be a name or a number.");
	ERR_FAIL_COND_MSG(depth >= MAX_DEPTH, vformat("Global Transmit: transmissions nested %d deep on channel '%s'; a receiver probably transmits back to itself.", MAX_DEPTH, p_channel));
	const Variant channel = p_channel.is_string() ? Variant(StringName(p_channel)) : p_channel;

	Array arguments;
	arguments.resize(p_argcount);
	for (int i = 0; i < p_argcount; i++) {
		arguments[i] = *p_args[i];
	}

	Channel *ch = _get_channel(channel, true);
	ch->last_arguments = arguments;
	ch->count++;
	// Listeners may start or stop listening while they are called: go over a copy.
	const LocalVector<Callable> listeners(ch->listeners);

	depth++;
	bool pruned = false;
	for (const Callable &listener : listeners) {
		if (!listener.is_valid()) {
			pruned = true; // Its object is gone.
			continue;
		}
		Variant result;
		Callable::CallError ce;
		listener.callp(p_args, p_argcount, result, ce);
		if (ce.error == Callable::CallError::CALL_ERROR_TOO_MANY_ARGUMENTS && ce.expected >= 0 && ce.expected < p_argcount) {
			// A receiver that takes fewer arguments gets the first ones.
			listener.callp(p_args, ce.expected, result, ce);
		}
		if (ce.error != Callable::CallError::CALL_OK) {
			ERR_PRINT(vformat("Global Transmit: error calling a receiver of channel '%s': %s.", channel, Variant::get_callable_error_text(listener, p_args, p_argcount, ce)));
		}
	}
	depth--;

	if (pruned) {
		ch = _get_channel(channel, false);
		if (ch) {
			for (int i = (int)ch->listeners.size() - 1; i >= 0; i--) {
				if (!ch->listeners[i].is_valid()) {
					ch->listeners.remove_at(i);
				}
			}
		}
	}
	emit_signal(SNAME("transmitted"), channel, arguments);
}

Error GlobalTransmit::_transmit(const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	if (p_argcount < 1) {
		r_error.error = Callable::CallError::CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.expected = 1;
		return ERR_INVALID_PARAMETER;
	}
	if (!is_valid_channel(*p_args[0])) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::STRING_NAME;
		return ERR_INVALID_PARAMETER;
	}
	r_error.error = Callable::CallError::CALL_OK;
	_deliver(*p_args[0], &p_args[1], p_argcount - 1);
	return OK;
}

Error GlobalTransmit::_transmit_deferred(const Variant **p_args, int p_argcount, Callable::CallError &r_error) {
	if (p_argcount < 1) {
		r_error.error = Callable::CallError::CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error.expected = 1;
		return ERR_INVALID_PARAMETER;
	}
	if (!is_valid_channel(*p_args[0])) {
		r_error.error = Callable::CallError::CALL_ERROR_INVALID_ARGUMENT;
		r_error.argument = 0;
		r_error.expected = Variant::STRING_NAME;
		return ERR_INVALID_PARAMETER;
	}
	r_error.error = Callable::CallError::CALL_OK;
	Array arguments;
	for (int i = 1; i < p_argcount; i++) {
		arguments.push_back(*p_args[i]);
	}
	callable_mp(this, &GlobalTransmit::transmit_array).call_deferred(*p_args[0], arguments);
	return OK;
}

void GlobalTransmit::transmit_array(const Variant &p_channel, const Array &p_arguments) {
	const int count = p_arguments.size();
	const Variant **pointers = (const Variant **)alloca(sizeof(Variant *) * MAX(count, 1));
	for (int i = 0; i < count; i++) {
		pointers[i] = &p_arguments[i];
	}
	_deliver(p_channel, pointers, count);
}

void GlobalTransmit::listen(const Variant &p_channel, const Callable &p_callable) {
	ERR_FAIL_COND_MSG(!is_valid_channel(p_channel), "A transmission's channel must be a name or a number.");
	ERR_FAIL_COND_MSG(!p_callable.is_valid(), "Can't listen with an invalid callable.");
	Channel *ch = _get_channel(p_channel, true);
	if (!ch->listeners.has(p_callable)) {
		ch->listeners.push_back(p_callable);
	}
}

void GlobalTransmit::stop_listening(const Variant &p_channel, const Callable &p_callable) {
	ERR_FAIL_COND_MSG(!is_valid_channel(p_channel), "A transmission's channel must be a name or a number.");
	Channel *ch = _get_channel(p_channel, false);
	if (ch) {
		ch->listeners.erase(p_callable);
	}
}

bool GlobalTransmit::is_listening(const Variant &p_channel, const Callable &p_callable) const {
	ERR_FAIL_COND_V(!is_valid_channel(p_channel), false);
	const Channel *ch = _get_channel_const(p_channel);
	return ch && ch->listeners.has(p_callable);
}

Array GlobalTransmit::get_listeners(const Variant &p_channel) const {
	Array result;
	ERR_FAIL_COND_V(!is_valid_channel(p_channel), result);
	const Channel *ch = _get_channel_const(p_channel);
	if (ch) {
		for (const Callable &listener : ch->listeners) {
			if (listener.is_valid()) {
				result.push_back(listener);
			}
		}
	}
	return result;
}

Array GlobalTransmit::get_arguments(const Variant &p_channel) const {
	ERR_FAIL_COND_V(!is_valid_channel(p_channel), Array());
	const Channel *ch = _get_channel_const(p_channel);
	return ch ? ch->last_arguments.duplicate() : Array();
}

int GlobalTransmit::get_transmission_count(const Variant &p_channel) const {
	ERR_FAIL_COND_V(!is_valid_channel(p_channel), 0);
	const Channel *ch = _get_channel_const(p_channel);
	return ch ? (int)ch->count : 0;
}

Array GlobalTransmit::get_channels() const {
	Array result;
	for (const KeyValue<StringName, Channel> &E : named) {
		result.push_back(E.key);
	}
	for (const KeyValue<int64_t, Channel> &E : numbered) {
		result.push_back(E.key);
	}
	return result;
}

void GlobalTransmit::clear_channel(const Variant &p_channel) {
	ERR_FAIL_COND_MSG(!is_valid_channel(p_channel), "A transmission's channel must be a name or a number.");
	if (p_channel.get_type() == Variant::INT) {
		numbered.erase((int64_t)p_channel);
	} else {
		named.erase(StringName(p_channel));
	}
}

void GlobalTransmit::_node_entered(Node *p_node) {
	// Only while the game runs: in the editor, scripts that aren't tools have no methods to call.
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	const Array receivers = p_node->get_meta(SNAME("_transmit_receivers"), Array());
	for (const Variant &entry : receivers) {
		const Dictionary receiver = entry;
		const Variant channel = receiver.get("channel", Variant());
		const StringName method = receiver.get("method", StringName());
		if (!is_valid_channel(channel) || method == StringName()) {
			continue;
		}
		if (!p_node->has_method(method)) {
			ERR_PRINT(vformat("Global Transmit: '%s' listens on channel '%s' with method '%s', which it doesn't have.", p_node->get_name(), channel, method));
			continue;
		}
		listen(channel, Callable(p_node, method));
	}
}

void GlobalTransmit::_node_exited(Node *p_node) {
	if (Engine::get_singleton()->is_editor_hint()) {
		return;
	}
	const Array receivers = p_node->get_meta(SNAME("_transmit_receivers"), Array());
	for (const Variant &entry : receivers) {
		const Dictionary receiver = entry;
		const Variant channel = receiver.get("channel", Variant());
		const StringName method = receiver.get("method", StringName());
		if (is_valid_channel(channel) && method != StringName()) {
			stop_listening(channel, Callable(p_node, method));
		}
	}
}

void GlobalTransmit::_bind_methods() {
	{
		MethodInfo mi("transmit", PropertyInfo(Variant::NIL, "channel", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT));
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "transmit", &GlobalTransmit::_transmit, mi, varray(), false);
	}
	{
		MethodInfo mi("transmit_deferred", PropertyInfo(Variant::NIL, "channel", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT));
		ClassDB::bind_vararg_method(METHOD_FLAGS_DEFAULT, "transmit_deferred", &GlobalTransmit::_transmit_deferred, mi, varray(), false);
	}
	ClassDB::bind_method(D_METHOD("transmit_array", "channel", "arguments"), &GlobalTransmit::transmit_array);
	ClassDB::bind_method(D_METHOD("listen", "channel", "callable"), &GlobalTransmit::listen);
	ClassDB::bind_method(D_METHOD("stop_listening", "channel", "callable"), &GlobalTransmit::stop_listening);
	ClassDB::bind_method(D_METHOD("is_listening", "channel", "callable"), &GlobalTransmit::is_listening);
	ClassDB::bind_method(D_METHOD("get_listeners", "channel"), &GlobalTransmit::get_listeners);
	ClassDB::bind_method(D_METHOD("get_arguments", "channel"), &GlobalTransmit::get_arguments);
	ClassDB::bind_method(D_METHOD("get_transmission_count", "channel"), &GlobalTransmit::get_transmission_count);
	ClassDB::bind_method(D_METHOD("get_channels"), &GlobalTransmit::get_channels);
	ClassDB::bind_method(D_METHOD("clear_channel", "channel"), &GlobalTransmit::clear_channel);

	ADD_SIGNAL(MethodInfo("transmitted", PropertyInfo(Variant::NIL, "channel", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NIL_IS_VARIANT), PropertyInfo(Variant::ARRAY, "arguments")));
}

GlobalTransmit::GlobalTransmit() {
	singleton = this;
}

GlobalTransmit::~GlobalTransmit() {
	singleton = nullptr;
}
