// A small runner for the TensorFlow Lite models of the voiceprint of the speaker (the files are read from the firmware of the speaker at run time; they are never copied into
// this project). It knows only what these models use: float activations, int8 weights (per tensor or per channel) that are turned into floats once when the file is loaded,
// and the operations RESHAPE, CONV_2D (also grouped), FULLY_CONNECTED, MEAN, SUM, ADD, SUB, MUL, DIV, MAXIMUM, SQRT, RSQRT, TANH, LOGISTIC, SOFTMAX, TRANSPOSE,
// EXPAND_DIMS and CONCATENATION. Anything else fails the load with a message. Shapes are worked out at every run, so the length of the input may change (the head of the
// voiceprint takes a window of frames of any length).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cs {

class TfliteMini {
 public:
  struct Tensor {
    std::vector<int> shape;
    std::vector<float> f;       // the values (activations and float or dequantized constants)
    std::vector<int32_t> i;     // constants of integer type (axes, shapes, permutations)
    bool is_const = false;
  };
  struct Op {
    int code = 0;
    std::vector<int> in, out;
    int padding = 0, stride_w = 1, stride_h = 1, dil_w = 1, dil_h = 1, act = 0, axis = 0;
    bool keep_dims = false;
    float beta = 1.f;
  };

  bool load(const std::string &path, std::string *err);
  // Runs with one input of the given shape; the first output goes to `out` (values and shape). False with `err` on a problem.
  bool run(const std::vector<int> &in_shape, const float *in, std::vector<float> *out, std::vector<int> *out_shape, std::string *err);
  bool loaded() const { return !ops_.empty(); }

 private:
  std::vector<Tensor> tensors_;
  std::vector<Op> ops_;
  std::vector<int> inputs_, outputs_;
  bool exec(const Op &op, std::vector<Tensor> &t, std::string *err) const;
};

}  // namespace cs
