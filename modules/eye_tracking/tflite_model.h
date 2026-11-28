/**************************************************************************/
/*  tflite_model.h                                                        */
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

#include "core/error/error_list.h"
#include "core/string/ustring.h"
#include "core/templates/local_vector.h"
#include "core/templates/vector.h"

// Runs a small TensorFlow Lite model on the CPU, in 32 bit floats. Only what
// the eye tracker's two MediaPipe models use is here: 2D convolutions (normal
// and depthwise), max pooling, padding, add, ReLU, PReLU, reshape, concatenate,
// and float16 weights unpacked when the model loads. Shapes are fixed by the
// model, so every tensor gets its memory once, at load.
//
// Not thread-safe: one model, one thread at a time.
class TFLiteModel {
public:
	enum Op {
		OP_ADD = 0,
		OP_CONCATENATION = 2,
		OP_CONV_2D = 3,
		OP_DEPTHWISE_CONV_2D = 4,
		OP_DEQUANTIZE = 6,
		OP_MAX_POOL_2D = 17,
		OP_RELU = 19,
		OP_RELU6 = 21,
		OP_RESHAPE = 22,
		OP_PAD = 34,
		OP_PRELU = 54,
	};

private:
	enum TensorType {
		TYPE_FLOAT32 = 0,
		TYPE_FLOAT16 = 1,
		TYPE_INT32 = 2,
	};

	enum Activation {
		ACTIVATION_NONE = 0,
		ACTIVATION_RELU = 1,
		ACTIVATION_RELU_N1_TO_1 = 2,
		ACTIVATION_RELU6 = 3,
	};

	struct Tensor {
		LocalVector<int> shape;
		int element_count = 0;
		int type = TYPE_FLOAT32;
		bool constant = false;
		LocalVector<float> data;
		LocalVector<int32_t> int_data;
		String name;
	};

	struct Operator {
		int code = 0;
		LocalVector<int> inputs;
		LocalVector<int> outputs;
		bool same_padding = true;
		int stride_w = 1;
		int stride_h = 1;
		int dilation_w = 1;
		int dilation_h = 1;
		int filter_w = 1;
		int filter_h = 1;
		int activation = ACTIVATION_NONE;
		int axis = 0;
		bool skip = false;
		// Convolution weights reordered so the innermost loop runs over output
		// channels: [kernel y][kernel x][input channel][output channel].
		LocalVector<float> weights;
	};

	Vector<uint8_t> file;
	LocalVector<Tensor> tensors;
	LocalVector<Operator> operators;
	LocalVector<int> input_tensors;
	LocalVector<int> output_tensors;
	bool loaded = false;

	Error _parse();
	Error _prepare();
	void _run_conv(const Operator &p_op);
	void _run_depthwise(const Operator &p_op);
	void _run_max_pool(const Operator &p_op);
	void _run_add(const Operator &p_op);
	void _run_prelu(const Operator &p_op);
	void _run_pad(const Operator &p_op);
	void _run_concatenation(const Operator &p_op);
	static void _activate(float *p_data, int p_count, int p_activation);

public:
	Error load(const uint8_t *p_data, int p_size);
	bool is_loaded() const { return loaded; }

	int get_input_count() const { return input_tensors.size(); }
	int get_output_count() const { return output_tensors.size(); }
	Vector<int> get_input_shape(int p_index) const;
	Vector<int> get_output_shape(int p_index) const;

	// Fill this, then run().
	float *get_input(int p_index);
	void run();
	const float *get_output(int p_index) const;
	int get_output_size(int p_index) const;
};
