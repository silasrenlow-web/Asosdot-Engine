/**************************************************************************/
/*  tflite_model.cpp                                                      */
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

#include "tflite_model.h"

#include "core/error/error_macros.h"
#include "core/math/math_funcs.h"
#include "core/variant/variant.h"

#include <cstring>

namespace {

// Reads the FlatBuffers layout of a .tflite file: tables found through a
// vtable of field offsets, references stored as offsets from where they sit.
// Every read is checked against the file's size, so a broken file fails to
// load instead of reading past its end.
class FlatReader {
	const uint8_t *data = nullptr;
	uint32_t size = 0;

public:
	bool failed = false;

	FlatReader(const uint8_t *p_data, uint32_t p_size) :
			data(p_data), size(p_size) {}

	bool fits(uint32_t p_pos, uint32_t p_bytes) {
		if (uint64_t(p_pos) + p_bytes > size) {
			failed = true;
			return false;
		}
		return true;
	}

	uint32_t u32(uint32_t p_pos) {
		if (!fits(p_pos, 4)) {
			return 0;
		}
		uint32_t v;
		memcpy(&v, data + p_pos, 4);
		return v;
	}

	int32_t i32(uint32_t p_pos) { return int32_t(u32(p_pos)); }

	uint16_t u16(uint32_t p_pos) {
		if (!fits(p_pos, 2)) {
			return 0;
		}
		uint16_t v;
		memcpy(&v, data + p_pos, 2);
		return v;
	}

	uint8_t u8(uint32_t p_pos) {
		if (!fits(p_pos, 1)) {
			return 0;
		}
		return data[p_pos];
	}

	uint32_t root() { return u32(0); }

	// Where field p_index of the table at p_table is, or 0 when it is not set.
	uint32_t field(uint32_t p_table, int p_index) {
		if (p_table == 0) {
			return 0;
		}
		const uint32_t vtable = uint32_t(int64_t(p_table) - i32(p_table));
		const uint16_t vtable_size = u16(vtable);
		const uint32_t entry = 4 + 2 * p_index;
		if (entry >= vtable_size) {
			return 0;
		}
		const uint16_t offset = u16(vtable + entry);
		return offset ? p_table + offset : 0;
	}

	int32_t field_i32(uint32_t p_table, int p_index, int32_t p_default) {
		const uint32_t p = field(p_table, p_index);
		return p ? i32(p) : p_default;
	}

	uint32_t field_u32(uint32_t p_table, int p_index, uint32_t p_default) {
		const uint32_t p = field(p_table, p_index);
		return p ? u32(p) : p_default;
	}

	int8_t field_i8(uint32_t p_table, int p_index, int8_t p_default) {
		const uint32_t p = field(p_table, p_index);
		return p ? int8_t(u8(p)) : p_default;
	}

	uint32_t ref(uint32_t p_table, int p_index) {
		const uint32_t p = field(p_table, p_index);
		return p ? p + u32(p) : 0;
	}

	// A vector: where its elements start and how many there are.
	uint32_t vector(uint32_t p_table, int p_index, uint32_t &r_count) {
		const uint32_t p = ref(p_table, p_index);
		r_count = p ? u32(p) : 0;
		return p ? p + 4 : 0;
	}

	uint32_t table_at(uint32_t p_element) { return p_element + u32(p_element); }

	const uint8_t *bytes(uint32_t p_pos, uint32_t p_count) {
		if (!fits(p_pos, p_count)) {
			return nullptr;
		}
		return data + p_pos;
	}
};

// Model, SubGraph, Tensor, Operator, OperatorCode and Buffer fields, as
// numbered in TensorFlow Lite's schema.fbs.
enum {
	MODEL_OPERATOR_CODES = 1,
	MODEL_SUBGRAPHS = 2,
	MODEL_BUFFERS = 4,
	SUBGRAPH_TENSORS = 0,
	SUBGRAPH_INPUTS = 1,
	SUBGRAPH_OUTPUTS = 2,
	SUBGRAPH_OPERATORS = 3,
	TENSOR_SHAPE = 0,
	TENSOR_TYPE = 1,
	TENSOR_BUFFER = 2,
	TENSOR_NAME = 3,
	OPERATOR_OPCODE_INDEX = 0,
	OPERATOR_INPUTS = 1,
	OPERATOR_OUTPUTS = 2,
	OPERATOR_BUILTIN_OPTIONS = 4,
	OPCODE_DEPRECATED_BUILTIN_CODE = 0,
	OPCODE_BUILTIN_CODE = 3,
	BUFFER_DATA = 0,
};

int same_padding_before(int p_in, int p_kernel, int p_stride, int p_dilation, int p_out) {
	const int effective = (p_kernel - 1) * p_dilation + 1;
	const int total = MAX((p_out - 1) * p_stride + effective - p_in, 0);
	return total / 2;
}

} // namespace

Error TFLiteModel::load(const uint8_t *p_data, int p_size) {
	loaded = false;
	tensors.clear();
	operators.clear();
	input_tensors.clear();
	output_tensors.clear();
	ERR_FAIL_COND_V(p_data == nullptr || p_size < 8, ERR_INVALID_DATA);
	ERR_FAIL_COND_V_MSG(memcmp(p_data + 4, "TFL3", 4) != 0, ERR_FILE_UNRECOGNIZED, "Not a TensorFlow Lite model.");
	file.resize(p_size);
	memcpy(file.ptrw(), p_data, p_size);

	Error err = _parse();
	if (err == OK) {
		err = _prepare();
	}
	// The weights are unpacked into the tensors and operators; the file is no
	// longer needed.
	file.clear();
	loaded = err == OK;
	return err;
}

Error TFLiteModel::_parse() {
	FlatReader fb(file.ptr(), file.size());
	const uint32_t model = fb.root();

	LocalVector<int> opcodes;
	uint32_t count = 0;
	uint32_t start = fb.vector(model, MODEL_OPERATOR_CODES, count);
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t code_table = fb.table_at(start + 4 * i);
		const int deprecated = fb.field_i8(code_table, OPCODE_DEPRECATED_BUILTIN_CODE, 0);
		const int code = fb.field_i32(code_table, OPCODE_BUILTIN_CODE, 0);
		opcodes.push_back(MAX(deprecated, code));
	}

	LocalVector<uint32_t> buffers;
	start = fb.vector(model, MODEL_BUFFERS, count);
	for (uint32_t i = 0; i < count; i++) {
		buffers.push_back(fb.table_at(start + 4 * i));
	}

	uint32_t subgraph_count = 0;
	const uint32_t subgraphs = fb.vector(model, MODEL_SUBGRAPHS, subgraph_count);
	ERR_FAIL_COND_V(subgraph_count < 1, ERR_INVALID_DATA);
	const uint32_t subgraph = fb.table_at(subgraphs);

	start = fb.vector(subgraph, SUBGRAPH_TENSORS, count);
	tensors.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t table = fb.table_at(start + 4 * i);
		Tensor &t = tensors[i];
		uint32_t dims = 0;
		const uint32_t shape = fb.vector(table, TENSOR_SHAPE, dims);
		t.element_count = 1;
		for (uint32_t d = 0; d < dims; d++) {
			const int v = fb.i32(shape + 4 * d);
			t.shape.push_back(v);
			t.element_count *= MAX(v, 1);
		}
		t.type = fb.field_i8(table, TENSOR_TYPE, TYPE_FLOAT32);
		uint32_t name_length = 0;
		const uint32_t name = fb.vector(table, TENSOR_NAME, name_length);
		if (name_length) {
			const uint8_t *chars = fb.bytes(name, name_length);
			if (chars) {
				t.name = String::utf8((const char *)chars, name_length);
			}
		}

		const uint32_t buffer_index = fb.field_u32(table, TENSOR_BUFFER, 0);
		uint32_t byte_count = 0;
		const uint32_t bytes_at = buffer_index < buffers.size() ? fb.vector(buffers[buffer_index], BUFFER_DATA, byte_count) : 0;
		if (byte_count > 0) {
			const uint8_t *bytes = fb.bytes(bytes_at, byte_count);
			ERR_FAIL_NULL_V(bytes, ERR_INVALID_DATA);
			t.constant = true;
			if (t.type == TYPE_FLOAT32) {
				ERR_FAIL_COND_V(byte_count != uint32_t(t.element_count) * 4, ERR_INVALID_DATA);
				t.data.resize(t.element_count);
				memcpy(t.data.ptr(), bytes, byte_count);
			} else if (t.type == TYPE_FLOAT16) {
				ERR_FAIL_COND_V(byte_count != uint32_t(t.element_count) * 2, ERR_INVALID_DATA);
				t.data.resize(t.element_count);
				for (int e = 0; e < t.element_count; e++) {
					uint16_t half;
					memcpy(&half, bytes + e * 2, 2);
					t.data[e] = Math::half_to_float(half);
				}
			} else if (t.type == TYPE_INT32) {
				ERR_FAIL_COND_V(byte_count != uint32_t(t.element_count) * 4, ERR_INVALID_DATA);
				t.int_data.resize(t.element_count);
				memcpy(t.int_data.ptr(), bytes, byte_count);
			} else {
				ERR_FAIL_V_MSG(ERR_UNAVAILABLE, vformat("Tensor type %d is not supported.", t.type));
			}
		}
	}

	auto read_indices = [&](uint32_t p_table, int p_field, LocalVector<int> &r_out) {
		uint32_t n = 0;
		const uint32_t at = fb.vector(p_table, p_field, n);
		for (uint32_t k = 0; k < n; k++) {
			r_out.push_back(fb.i32(at + 4 * k));
		}
	};
	read_indices(subgraph, SUBGRAPH_INPUTS, input_tensors);
	read_indices(subgraph, SUBGRAPH_OUTPUTS, output_tensors);

	start = fb.vector(subgraph, SUBGRAPH_OPERATORS, count);
	operators.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t table = fb.table_at(start + 4 * i);
		Operator &op = operators[i];
		const uint32_t opcode_index = fb.field_u32(table, OPERATOR_OPCODE_INDEX, 0);
		ERR_FAIL_COND_V(opcode_index >= opcodes.size(), ERR_INVALID_DATA);
		op.code = opcodes[opcode_index];
		read_indices(table, OPERATOR_INPUTS, op.inputs);
		read_indices(table, OPERATOR_OUTPUTS, op.outputs);

		const uint32_t options = fb.ref(table, OPERATOR_BUILTIN_OPTIONS);
		switch (op.code) {
			case OP_CONV_2D:
				// Conv2DOptions: padding, stride_w, stride_h, activation, dilation_w, dilation_h.
				op.same_padding = fb.field_i8(options, 0, 0) == 0;
				op.stride_w = fb.field_i32(options, 1, 1);
				op.stride_h = fb.field_i32(options, 2, 1);
				op.activation = fb.field_i8(options, 3, 0);
				op.dilation_w = fb.field_i32(options, 4, 1);
				op.dilation_h = fb.field_i32(options, 5, 1);
				break;
			case OP_DEPTHWISE_CONV_2D:
				// DepthwiseConv2DOptions: padding, stride_w, stride_h, depth_multiplier,
				// activation, dilation_w, dilation_h.
				op.same_padding = fb.field_i8(options, 0, 0) == 0;
				op.stride_w = fb.field_i32(options, 1, 1);
				op.stride_h = fb.field_i32(options, 2, 1);
				op.activation = fb.field_i8(options, 4, 0);
				op.dilation_w = fb.field_i32(options, 5, 1);
				op.dilation_h = fb.field_i32(options, 6, 1);
				break;
			case OP_MAX_POOL_2D:
				// Pool2DOptions: padding, stride_w, stride_h, filter_w, filter_h, activation.
				op.same_padding = fb.field_i8(options, 0, 0) == 0;
				op.stride_w = fb.field_i32(options, 1, 1);
				op.stride_h = fb.field_i32(options, 2, 1);
				op.filter_w = fb.field_i32(options, 3, 1);
				op.filter_h = fb.field_i32(options, 4, 1);
				op.activation = fb.field_i8(options, 5, 0);
				break;
			case OP_ADD:
				// AddOptions: activation.
				op.activation = fb.field_i8(options, 0, 0);
				break;
			case OP_CONCATENATION:
				// ConcatenationOptions: axis, activation.
				op.axis = fb.field_i32(options, 0, 0);
				op.activation = fb.field_i8(options, 1, 0);
				break;
			case OP_DEQUANTIZE:
			case OP_RELU:
			case OP_RELU6:
			case OP_RESHAPE:
			case OP_PAD:
			case OP_PRELU:
				break;
			default:
				ERR_FAIL_V_MSG(ERR_UNAVAILABLE, vformat("TensorFlow Lite operator %d is not supported.", op.code));
		}
	}

	ERR_FAIL_COND_V_MSG(fb.failed, ERR_FILE_CORRUPT, "TensorFlow Lite model is cut short or broken.");
	return OK;
}

Error TFLiteModel::_prepare() {
	const int tensor_count = tensors.size();
	for (Operator &op : operators) {
		for (int index : op.inputs) {
			ERR_FAIL_COND_V(index >= tensor_count, ERR_INVALID_DATA);
		}
		for (int index : op.outputs) {
			ERR_FAIL_COND_V(index < 0 || index >= tensor_count, ERR_INVALID_DATA);
		}
		ERR_FAIL_COND_V(op.outputs.is_empty() || op.inputs.is_empty() || op.inputs[0] < 0, ERR_INVALID_DATA);
		Tensor &out = tensors[op.outputs[0]];

		if (op.code == OP_DEQUANTIZE && tensors[op.inputs[0]].constant) {
			// Weights stored as float16: unpacked once, here.
			out.data = tensors[op.inputs[0]].data;
			out.constant = true;
			op.skip = true;
			continue;
		}

		if (op.code == OP_CONV_2D) {
			ERR_FAIL_COND_V(op.inputs.size() < 2, ERR_INVALID_DATA);
			const Tensor &w = tensors[op.inputs[1]];
			ERR_FAIL_COND_V_MSG(!w.constant || w.shape.size() != 4, ERR_UNAVAILABLE, "Convolution weights must be constant.");
			const int o_count = w.shape[0];
			const int kh = w.shape[1];
			const int kw = w.shape[2];
			const int i_count = w.shape[3];
			op.weights.resize(w.element_count);
			for (int o = 0; o < o_count; o++) {
				for (int y = 0; y < kh; y++) {
					for (int x = 0; x < kw; x++) {
						for (int i = 0; i < i_count; i++) {
							op.weights[((y * kw + x) * i_count + i) * o_count + o] = w.data[((o * kh + y) * kw + x) * i_count + i];
						}
					}
				}
			}
		} else if (op.code == OP_DEPTHWISE_CONV_2D) {
			ERR_FAIL_COND_V(op.inputs.size() < 2, ERR_INVALID_DATA);
			const Tensor &w = tensors[op.inputs[1]];
			ERR_FAIL_COND_V_MSG(!w.constant || w.shape.size() != 4, ERR_UNAVAILABLE, "Convolution weights must be constant.");
			op.weights = w.data;
		}

		if (!out.constant && out.data.size() != uint32_t(out.element_count)) {
			out.data.resize(out.element_count);
		}
	}
	for (int index : input_tensors) {
		ERR_FAIL_INDEX_V(index, tensor_count, ERR_INVALID_DATA);
		tensors[index].data.resize(tensors[index].element_count);
	}
	for (int index : output_tensors) {
		ERR_FAIL_INDEX_V(index, tensor_count, ERR_INVALID_DATA);
	}
	return OK;
}

Vector<int> TFLiteModel::get_input_shape(int p_index) const {
	Vector<int> shape;
	ERR_FAIL_INDEX_V(p_index, (int)input_tensors.size(), shape);
	for (int v : tensors[input_tensors[p_index]].shape) {
		shape.push_back(v);
	}
	return shape;
}

Vector<int> TFLiteModel::get_output_shape(int p_index) const {
	Vector<int> shape;
	ERR_FAIL_INDEX_V(p_index, (int)output_tensors.size(), shape);
	for (int v : tensors[output_tensors[p_index]].shape) {
		shape.push_back(v);
	}
	return shape;
}

float *TFLiteModel::get_input(int p_index) {
	ERR_FAIL_INDEX_V(p_index, (int)input_tensors.size(), nullptr);
	return tensors[input_tensors[p_index]].data.ptr();
}

const float *TFLiteModel::get_output(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)output_tensors.size(), nullptr);
	return tensors[output_tensors[p_index]].data.ptr();
}

int TFLiteModel::get_output_size(int p_index) const {
	ERR_FAIL_INDEX_V(p_index, (int)output_tensors.size(), 0);
	return tensors[output_tensors[p_index]].element_count;
}

void TFLiteModel::_activate(float *p_data, int p_count, int p_activation) {
	switch (p_activation) {
		case ACTIVATION_RELU:
			for (int i = 0; i < p_count; i++) {
				p_data[i] = MAX(p_data[i], 0.0f);
			}
			break;
		case ACTIVATION_RELU_N1_TO_1:
			for (int i = 0; i < p_count; i++) {
				p_data[i] = CLAMP(p_data[i], -1.0f, 1.0f);
			}
			break;
		case ACTIVATION_RELU6:
			for (int i = 0; i < p_count; i++) {
				p_data[i] = CLAMP(p_data[i], 0.0f, 6.0f);
			}
			break;
		default:
			break;
	}
}

void TFLiteModel::run() {
	ERR_FAIL_COND(!loaded);
	for (const Operator &op : operators) {
		if (op.skip) {
			continue;
		}
		Tensor &out = tensors[op.outputs[0]];
		const Tensor &in = tensors[op.inputs[0]];
		switch (op.code) {
			case OP_CONV_2D:
				_run_conv(op);
				break;
			case OP_DEPTHWISE_CONV_2D:
				_run_depthwise(op);
				break;
			case OP_MAX_POOL_2D:
				_run_max_pool(op);
				break;
			case OP_ADD:
				_run_add(op);
				break;
			case OP_PRELU:
				_run_prelu(op);
				break;
			case OP_PAD:
				_run_pad(op);
				break;
			case OP_CONCATENATION:
				_run_concatenation(op);
				break;
			case OP_DEQUANTIZE:
			case OP_RESHAPE:
				ERR_FAIL_COND(in.data.size() != out.data.size());
				memcpy(out.data.ptr(), in.data.ptr(), sizeof(float) * out.data.size());
				break;
			case OP_RELU:
				for (uint32_t i = 0; i < out.data.size(); i++) {
					out.data[i] = MAX(in.data[i], 0.0f);
				}
				break;
			case OP_RELU6:
				for (uint32_t i = 0; i < out.data.size(); i++) {
					out.data[i] = CLAMP(in.data[i], 0.0f, 6.0f);
				}
				break;
			default:
				break;
		}
	}
}

void TFLiteModel::_run_conv(const Operator &p_op) {
	const Tensor &in = tensors[p_op.inputs[0]];
	const Tensor &w = tensors[p_op.inputs[1]];
	const float *bias = p_op.inputs.size() > 2 && p_op.inputs[2] >= 0 ? tensors[p_op.inputs[2]].data.ptr() : nullptr;
	Tensor &out = tensors[p_op.outputs[0]];
	ERR_FAIL_COND(in.shape.size() != 4 || out.shape.size() != 4);

	const int in_h = in.shape[1], in_w = in.shape[2], in_c = in.shape[3];
	const int out_h = out.shape[1], out_w = out.shape[2], out_c = out.shape[3];
	const int kh = w.shape[1], kw = w.shape[2];
	ERR_FAIL_COND(w.shape[3] != in_c || w.shape[0] != out_c);
	const int pad_top = p_op.same_padding ? same_padding_before(in_h, kh, p_op.stride_h, p_op.dilation_h, out_h) : 0;
	const int pad_left = p_op.same_padding ? same_padding_before(in_w, kw, p_op.stride_w, p_op.dilation_w, out_w) : 0;
	const float *src = in.data.ptr();
	const float *weights = p_op.weights.ptr();
	float *dst = out.data.ptr();

	for (int oy = 0; oy < out_h; oy++) {
		for (int ox = 0; ox < out_w; ox++) {
			float *o = dst + (size_t(oy) * out_w + ox) * out_c;
			if (bias) {
				memcpy(o, bias, sizeof(float) * out_c);
			} else {
				memset(o, 0, sizeof(float) * out_c);
			}
			for (int ky = 0; ky < kh; ky++) {
				const int iy = oy * p_op.stride_h - pad_top + ky * p_op.dilation_h;
				if (iy < 0 || iy >= in_h) {
					continue;
				}
				for (int kx = 0; kx < kw; kx++) {
					const int ix = ox * p_op.stride_w - pad_left + kx * p_op.dilation_w;
					if (ix < 0 || ix >= in_w) {
						continue;
					}
					const float *pixel = src + (size_t(iy) * in_w + ix) * in_c;
					const float *kernel = weights + size_t(ky * kw + kx) * in_c * out_c;
					float *__restrict acc = o;
					// Four input channels a pass: the output row is read and written a
					// quarter as often, which is what limits this loop.
					int i = 0;
					for (; i + 4 <= in_c; i += 4) {
						const float v0 = pixel[i];
						const float v1 = pixel[i + 1];
						const float v2 = pixel[i + 2];
						const float v3 = pixel[i + 3];
						if (v0 == 0.0f && v1 == 0.0f && v2 == 0.0f && v3 == 0.0f) {
							// After a ReLU, many are.
							continue;
						}
						const float *__restrict r0 = kernel + size_t(i) * out_c;
						const float *__restrict r1 = r0 + out_c;
						const float *__restrict r2 = r1 + out_c;
						const float *__restrict r3 = r2 + out_c;
						for (int c = 0; c < out_c; c++) {
							acc[c] += v0 * r0[c] + v1 * r1[c] + v2 * r2[c] + v3 * r3[c];
						}
					}
					for (; i < in_c; i++) {
						const float v = pixel[i];
						if (v == 0.0f) {
							continue;
						}
						const float *__restrict row = kernel + size_t(i) * out_c;
						for (int c = 0; c < out_c; c++) {
							acc[c] += v * row[c];
						}
					}
				}
			}
			_activate(o, out_c, p_op.activation);
		}
	}
}

void TFLiteModel::_run_depthwise(const Operator &p_op) {
	const Tensor &in = tensors[p_op.inputs[0]];
	const Tensor &w = tensors[p_op.inputs[1]];
	const float *bias = p_op.inputs.size() > 2 && p_op.inputs[2] >= 0 ? tensors[p_op.inputs[2]].data.ptr() : nullptr;
	Tensor &out = tensors[p_op.outputs[0]];
	ERR_FAIL_COND(in.shape.size() != 4 || out.shape.size() != 4);

	const int in_h = in.shape[1], in_w = in.shape[2], in_c = in.shape[3];
	const int out_h = out.shape[1], out_w = out.shape[2], out_c = out.shape[3];
	const int kh = w.shape[1], kw = w.shape[2];
	ERR_FAIL_COND(w.shape[3] != out_c || out_c % in_c != 0);
	const int multiplier = out_c / in_c;
	const int pad_top = p_op.same_padding ? same_padding_before(in_h, kh, p_op.stride_h, p_op.dilation_h, out_h) : 0;
	const int pad_left = p_op.same_padding ? same_padding_before(in_w, kw, p_op.stride_w, p_op.dilation_w, out_w) : 0;
	const float *src = in.data.ptr();
	const float *weights = p_op.weights.ptr();
	float *dst = out.data.ptr();

	for (int oy = 0; oy < out_h; oy++) {
		for (int ox = 0; ox < out_w; ox++) {
			float *o = dst + (size_t(oy) * out_w + ox) * out_c;
			if (bias) {
				memcpy(o, bias, sizeof(float) * out_c);
			} else {
				memset(o, 0, sizeof(float) * out_c);
			}
			for (int ky = 0; ky < kh; ky++) {
				const int iy = oy * p_op.stride_h - pad_top + ky * p_op.dilation_h;
				if (iy < 0 || iy >= in_h) {
					continue;
				}
				for (int kx = 0; kx < kw; kx++) {
					const int ix = ox * p_op.stride_w - pad_left + kx * p_op.dilation_w;
					if (ix < 0 || ix >= in_w) {
						continue;
					}
					const float *pixel = src + (size_t(iy) * in_w + ix) * in_c;
					const float *kernel = weights + size_t(ky * kw + kx) * out_c;
					if (multiplier == 1) {
						for (int c = 0; c < out_c; c++) {
							o[c] += pixel[c] * kernel[c];
						}
					} else {
						for (int c = 0; c < out_c; c++) {
							o[c] += pixel[c / multiplier] * kernel[c];
						}
					}
				}
			}
			_activate(o, out_c, p_op.activation);
		}
	}
}

void TFLiteModel::_run_max_pool(const Operator &p_op) {
	const Tensor &in = tensors[p_op.inputs[0]];
	Tensor &out = tensors[p_op.outputs[0]];
	ERR_FAIL_COND(in.shape.size() != 4 || out.shape.size() != 4);
	const int in_h = in.shape[1], in_w = in.shape[2], channels = in.shape[3];
	const int out_h = out.shape[1], out_w = out.shape[2];
	ERR_FAIL_COND(out.shape[3] != channels);
	const int pad_top = p_op.same_padding ? same_padding_before(in_h, p_op.filter_h, p_op.stride_h, 1, out_h) : 0;
	const int pad_left = p_op.same_padding ? same_padding_before(in_w, p_op.filter_w, p_op.stride_w, 1, out_w) : 0;

	for (int oy = 0; oy < out_h; oy++) {
		for (int ox = 0; ox < out_w; ox++) {
			float *o = out.data.ptr() + (size_t(oy) * out_w + ox) * channels;
			for (int c = 0; c < channels; c++) {
				o[c] = -1e30f;
			}
			for (int ky = 0; ky < p_op.filter_h; ky++) {
				const int iy = oy * p_op.stride_h - pad_top + ky;
				if (iy < 0 || iy >= in_h) {
					continue;
				}
				for (int kx = 0; kx < p_op.filter_w; kx++) {
					const int ix = ox * p_op.stride_w - pad_left + kx;
					if (ix < 0 || ix >= in_w) {
						continue;
					}
					const float *pixel = in.data.ptr() + (size_t(iy) * in_w + ix) * channels;
					for (int c = 0; c < channels; c++) {
						o[c] = MAX(o[c], pixel[c]);
					}
				}
			}
			_activate(o, channels, p_op.activation);
		}
	}
}

void TFLiteModel::_run_add(const Operator &p_op) {
	ERR_FAIL_COND(p_op.inputs.size() < 2);
	const Tensor &a = tensors[p_op.inputs[0]];
	const Tensor &b = tensors[p_op.inputs[1]];
	Tensor &out = tensors[p_op.outputs[0]];
	const int n = out.data.size();
	// Same shapes, or one of them repeating along the last axis (a bias).
	const int a_count = a.data.size();
	const int b_count = b.data.size();
	ERR_FAIL_COND(a_count == 0 || b_count == 0 || n % a_count != 0 || n % b_count != 0);
	for (int i = 0; i < n; i++) {
		out.data[i] = a.data[i % a_count] + b.data[i % b_count];
	}
	_activate(out.data.ptr(), n, p_op.activation);
}

void TFLiteModel::_run_prelu(const Operator &p_op) {
	ERR_FAIL_COND(p_op.inputs.size() < 2);
	const Tensor &in = tensors[p_op.inputs[0]];
	const Tensor &alpha = tensors[p_op.inputs[1]];
	Tensor &out = tensors[p_op.outputs[0]];
	const int n = out.data.size();
	const int alpha_count = alpha.data.size();
	ERR_FAIL_COND(alpha_count == 0 || n % alpha_count != 0 || in.data.size() != out.data.size());
	for (int i = 0; i < n; i++) {
		const float v = in.data[i];
		out.data[i] = v >= 0.0f ? v : v * alpha.data[i % alpha_count];
	}
}

void TFLiteModel::_run_pad(const Operator &p_op) {
	ERR_FAIL_COND(p_op.inputs.size() < 2);
	const Tensor &in = tensors[p_op.inputs[0]];
	const Tensor &paddings = tensors[p_op.inputs[1]];
	Tensor &out = tensors[p_op.outputs[0]];
	ERR_FAIL_COND(in.shape.size() != 4 || out.shape.size() != 4 || paddings.int_data.size() != 8);

	int in_dims[4];
	int out_dims[4];
	int before[4];
	for (int d = 0; d < 4; d++) {
		in_dims[d] = in.shape[d];
		out_dims[d] = out.shape[d];
		before[d] = paddings.int_data[d * 2];
	}
	memset(out.data.ptr(), 0, sizeof(float) * out.data.size());
	for (int n = 0; n < in_dims[0]; n++) {
		for (int y = 0; y < in_dims[1]; y++) {
			for (int x = 0; x < in_dims[2]; x++) {
				const float *src = in.data.ptr() + ((size_t(n) * in_dims[1] + y) * in_dims[2] + x) * in_dims[3];
				float *dst = out.data.ptr() + ((size_t(n + before[0]) * out_dims[1] + y + before[1]) * out_dims[2] + x + before[2]) * out_dims[3] + before[3];
				memcpy(dst, src, sizeof(float) * in_dims[3]);
			}
		}
	}
}

void TFLiteModel::_run_concatenation(const Operator &p_op) {
	Tensor &out = tensors[p_op.outputs[0]];
	const int dims = out.shape.size();
	const int axis = p_op.axis < 0 ? p_op.axis + dims : p_op.axis;
	ERR_FAIL_INDEX(axis, dims);
	int outer = 1;
	for (int d = 0; d < axis; d++) {
		outer *= out.shape[d];
	}
	const int out_inner = out.data.size() / MAX(outer, 1);
	int offset = 0;
	for (int index : p_op.inputs) {
		const Tensor &in = tensors[index];
		const int inner = in.data.size() / MAX(outer, 1);
		for (int o = 0; o < outer; o++) {
			memcpy(out.data.ptr() + size_t(o) * out_inner + offset, in.data.ptr() + size_t(o) * inner, sizeof(float) * inner);
		}
		offset += inner;
	}
	_activate(out.data.ptr(), out.data.size(), p_op.activation);
}
