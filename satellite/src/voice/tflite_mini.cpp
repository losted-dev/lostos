#include "voice/tflite_mini.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <numeric>

namespace cs {

namespace {

// ---- a reader of FlatBuffers (what the format of a .tflite file is made of): tables, vectors and the offsets between them. Every read is checked against the size of the file.
struct Fb {
  const uint8_t *b;
  size_t n;
  mutable bool bad = false;
  bool in(size_t o, size_t len) const {
    if (o + len > n || o + len < o) {
      bad = true;
      return false;
    }
    return true;
  }
  uint32_t u32(size_t o) const { uint32_t v = 0; if (in(o, 4)) memcpy(&v, b + o, 4); return v; }
  int32_t i32(size_t o) const { return int32_t(u32(o)); }
  uint16_t u16(size_t o) const { uint16_t v = 0; if (in(o, 2)) memcpy(&v, b + o, 2); return v; }
  int64_t i64(size_t o) const { int64_t v = 0; if (in(o, 8)) memcpy(&v, b + o, 8); return v; }
  uint8_t u8(size_t o) const { return in(o, 1) ? b[o] : 0; }
  float f32(size_t o) const { float v = 0; if (in(o, 4)) memcpy(&v, b + o, 4); return v; }
  // the position of a field of a table (0: the field is absent, the default applies)
  size_t field(size_t tbl, int idx) const {
    const size_t vt = tbl - size_t(i32(tbl));
    if (bad || vt >= n) return 0;
    const uint16_t vlen = u16(vt);
    const size_t slot = 4 + 2 * size_t(idx);
    if (slot + 2 > vlen) return 0;
    const uint16_t off = u16(vt + slot);
    return off ? tbl + off : 0;
  }
  size_t follow(size_t pos) const { return pos + u32(pos); }   // an offset to a table, a vector or a string
  uint32_t vlen(size_t vec) const { return u32(vec); }
  size_t vdata(size_t vec) const { return vec + 4; }
  // a vector that a field holds: its position, 0 when absent
  size_t vec(size_t tbl, int idx) const { const size_t f = field(tbl, idx); return f ? follow(f) : 0; }
  int32_t geti(size_t tbl, int idx, int32_t def) const { const size_t f = field(tbl, idx); return f ? i32(f) : def; }
  int geti8(size_t tbl, int idx, int def) const { const size_t f = field(tbl, idx); return f ? int(int8_t(u8(f))) : def; }
};

enum Code {
  ADD = 0, CONCATENATION = 2, CONV_2D = 3, FULLY_CONNECTED = 9, LOGISTIC = 14, MUL = 18, RESHAPE = 22, SOFTMAX = 25, TANH = 28, TRANSPOSE = 39, MEAN = 40, SUB = 41, DIV = 42,
  MAXIMUM = 55, EXPAND_DIMS = 70, SUM = 74, SQRT = 75, RSQRT = 76,
};

bool supported(int c) {
  switch (c) {
    case ADD: case CONCATENATION: case CONV_2D: case FULLY_CONNECTED: case LOGISTIC: case MUL: case RESHAPE: case SOFTMAX: case TANH: case TRANSPOSE: case MEAN: case SUB:
    case DIV: case MAXIMUM: case EXPAND_DIMS: case SUM: case SQRT: case RSQRT:
      return true;
  }
  return false;
}

size_t count(const std::vector<int> &s) {
  size_t n = 1;
  for (int d : s) n *= size_t(d);
  return n;
}

float activate(float v, int act) {
  if (act == 1) return v < 0.f ? 0.f : v;
  if (act == 3) return std::min(6.f, std::max(0.f, v));
  return v;
}

}  // namespace

bool TfliteMini::load(const std::string &path, std::string *err) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  Fb fb{file.data(), file.size()};
  auto fail = [&](const std::string &why) {
    if (err) *err = why;
    ops_.clear();
    return false;
  };
  if (file.size() < 16 || memcmp(file.data() + 4, "TFL3", 4) != 0) return fail("not a TensorFlow Lite file");
  const size_t model = fb.follow(0);
  // operator codes
  std::vector<int> codes;
  const size_t ocs = fb.vec(model, 1);
  if (!ocs) return fail("no operator codes");
  for (uint32_t i = 0; i < fb.vlen(ocs); i++) {
    const size_t oc = fb.follow(fb.vdata(ocs) + 4 * i);
    const int dep = fb.geti8(oc, 0, 0), code = fb.geti(oc, 3, 0);
    codes.push_back(std::max(dep, code));
  }
  const size_t sgs = fb.vec(model, 2);
  if (!sgs || fb.vlen(sgs) < 1) return fail("no subgraph");
  const size_t sg = fb.follow(fb.vdata(sgs));
  const size_t bufs = fb.vec(model, 4);
  // tensors
  const size_t tens = fb.vec(sg, 0);
  if (!tens) return fail("no tensors");
  tensors_.assign(fb.vlen(tens), Tensor());
  for (uint32_t i = 0; i < fb.vlen(tens); i++) {
    const size_t t = fb.follow(fb.vdata(tens) + 4 * i);
    Tensor &T = tensors_[i];
    const size_t shp = fb.vec(t, 0);
    if (shp)
      for (uint32_t k = 0; k < fb.vlen(shp); k++) T.shape.push_back(fb.i32(fb.vdata(shp) + 4 * k));
    const int type = fb.geti8(t, 1, 0);
    const uint32_t bi = uint32_t(fb.geti(t, 2, 0));
    if (!bi || !bufs || bi >= fb.vlen(bufs)) continue;                       // an activation: nothing stored
    const size_t buf = fb.follow(fb.vdata(bufs) + 4 * bi);
    const size_t data = fb.vec(buf, 0);
    if (!data || !fb.vlen(data)) continue;
    const uint32_t len = fb.vlen(data);
    const uint8_t *p = file.data() + fb.vdata(data);
    if (!fb.in(fb.vdata(data), len)) return fail("a buffer is cut short");
    T.is_const = true;
    if (type == 0) {                                                         // float32
      T.f.resize(len / 4);
      memcpy(T.f.data(), p, T.f.size() * 4);
    } else if (type == 2) {                                                  // int32 (axes, shapes, permutations)
      T.i.resize(len / 4);
      memcpy(T.i.data(), p, T.i.size() * 4);
    } else if (type == 9) {                                                  // int8 weights: floats once, here
      std::vector<float> scale;
      int qdim = 0;
      const size_t q = fb.field(t, 4) ? fb.follow(fb.field(t, 4)) : 0;
      if (q) {
        const size_t sc = fb.vec(q, 2);
        if (sc)
          for (uint32_t k = 0; k < fb.vlen(sc); k++) scale.push_back(fb.f32(fb.vdata(sc) + 4 * k));
        qdim = fb.geti(q, 6, 0);
      }
      if (scale.empty()) return fail("an int8 tensor without scales");
      size_t inner = 1;
      for (size_t d = size_t(qdim) + 1; d < T.shape.size(); d++) inner *= size_t(T.shape[d]);
      const size_t chans = scale.size() > 1 && size_t(qdim) < T.shape.size() ? size_t(T.shape[qdim]) : 1;
      T.f.resize(len);
      for (uint32_t k = 0; k < len; k++) T.f[k] = float(int8_t(p[k])) * scale[chans > 1 ? (k / inner) % chans : 0];   // zero point 0 (symmetric weights)
    } else {
      return fail("a constant of an unsupported type: " + std::to_string(type));
    }
  }
  auto ints = [&](size_t vecpos, std::vector<int> *out) {
    if (!vecpos) return;
    for (uint32_t k = 0; k < fb.vlen(vecpos); k++) out->push_back(fb.i32(fb.vdata(vecpos) + 4 * k));
  };
  ints(fb.vec(sg, 1), &inputs_);
  ints(fb.vec(sg, 2), &outputs_);
  // operators
  const size_t opv = fb.vec(sg, 3);
  if (!opv) return fail("no operators");
  ops_.clear();
  for (uint32_t i = 0; i < fb.vlen(opv); i++) {
    const size_t o = fb.follow(fb.vdata(opv) + 4 * i);
    Op op;
    const uint32_t ci = uint32_t(fb.geti(o, 0, 0));
    if (ci >= codes.size()) return fail("bad operator code");
    op.code = codes[ci];
    if (!supported(op.code)) return fail("unsupported operation " + std::to_string(op.code));
    ints(fb.vec(o, 1), &op.in);
    ints(fb.vec(o, 2), &op.out);
    const size_t opt = fb.field(o, 4) ? fb.follow(fb.field(o, 4)) : 0;
    if (opt) {
      switch (op.code) {
        case CONV_2D:
          op.padding = fb.geti8(opt, 0, 0);
          op.stride_w = fb.geti(opt, 1, 1);
          op.stride_h = fb.geti(opt, 2, 1);
          op.act = fb.geti8(opt, 3, 0);
          op.dil_w = fb.geti(opt, 4, 1);
          op.dil_h = fb.geti(opt, 5, 1);
          break;
        case FULLY_CONNECTED:
          op.act = fb.geti8(opt, 0, 0);
          op.keep_dims = fb.geti8(opt, 2, 0) != 0;
          break;
        case ADD: case MUL: case SUB: case DIV:
          op.act = fb.geti8(opt, 0, 0);
          break;
        case MEAN: case SUM:
          op.keep_dims = fb.geti8(opt, 0, 0) != 0;
          break;
        case SOFTMAX:
          op.beta = fb.field(opt, 0) ? fb.f32(fb.field(opt, 0)) : 1.f;
          break;
        case CONCATENATION:
          op.axis = fb.geti(opt, 0, 0);
          op.act = fb.geti8(opt, 1, 0);
          break;
      }
    }
    if (op.code == RESHAPE && op.in.size() < 2 && opt) {                      // the shape in the options (older files)
      std::vector<int> shp;
      ints(fb.vec(opt, 0), &shp);
      Tensor t;
      t.is_const = true;
      t.i.assign(shp.begin(), shp.end());
      tensors_.push_back(t);
      op.in.push_back(int(tensors_.size()) - 1);
    }
    ops_.push_back(op);
  }
  if (fb.bad) return fail("the file is damaged");
  if (inputs_.size() != 1 || outputs_.empty()) return fail("expected one input");
  return true;
}

bool TfliteMini::run(const std::vector<int> &in_shape, const float *in, std::vector<float> *out, std::vector<int> *out_shape, std::string *err) {
  std::vector<Tensor> act(tensors_.size());
  act[inputs_[0]].shape = in_shape;
  act[inputs_[0]].f.assign(in, in + count(in_shape));
  for (const Op &op : ops_)
    if (!exec(op, act, err)) return false;
  const Tensor &o = tensors_[outputs_[0]].is_const ? tensors_[outputs_[0]] : act[outputs_[0]];
  *out = o.f;
  if (out_shape) *out_shape = o.shape;
  return true;
}

bool TfliteMini::exec(const Op &op, std::vector<Tensor> &act, std::string *err) const {
  auto T = [&](int idx) -> const Tensor & { return tensors_[idx].is_const ? tensors_[idx] : act[idx]; };
  auto bad = [&](const char *why) {
    if (err) *err = std::string(why) + " (operation " + std::to_string(op.code) + ")";
    return false;
  };
  Tensor &out = act[op.out[0]];
  const Tensor &a = T(op.in[0]);
  switch (op.code) {
    case SQRT: case RSQRT: case TANH: case LOGISTIC: {
      out.shape = a.shape;
      out.f.resize(a.f.size());
      for (size_t k = 0; k < a.f.size(); k++) {
        const float v = a.f[k];
        out.f[k] = op.code == SQRT ? std::sqrt(v) : op.code == RSQRT ? 1.f / std::sqrt(v) : op.code == TANH ? std::tanh(v) : 1.f / (1.f + std::exp(-v));
      }
      return true;
    }
    case ADD: case SUB: case MUL: case DIV: case MAXIMUM: {
      const Tensor &b = T(op.in[1]);
      const size_t rank = std::max(a.shape.size(), b.shape.size());
      std::vector<int> sa(rank, 1), sb(rank, 1), so(rank);
      std::copy(a.shape.begin(), a.shape.end(), sa.begin() + (rank - a.shape.size()));
      std::copy(b.shape.begin(), b.shape.end(), sb.begin() + (rank - b.shape.size()));
      for (size_t d = 0; d < rank; d++) {
        if (sa[d] != sb[d] && sa[d] != 1 && sb[d] != 1) return bad("shapes do not broadcast");
        so[d] = std::max(sa[d], sb[d]);
      }
      std::vector<size_t> sta(rank), stb(rank);
      size_t pa = 1, pb = 1;
      for (size_t d = rank; d-- > 0;) {
        sta[d] = sa[d] == 1 ? 0 : pa;
        stb[d] = sb[d] == 1 ? 0 : pb;
        pa *= size_t(sa[d]);
        pb *= size_t(sb[d]);
      }
      const size_t total = count(so);
      std::vector<float> r(total);
      std::vector<int> idx(rank, 0);
      size_t ia = 0, ib = 0;
      for (size_t k = 0; k < total; k++) {
        const float x = a.f[ia], y = b.f[ib];
        float v = op.code == ADD ? x + y : op.code == SUB ? x - y : op.code == MUL ? x * y : op.code == DIV ? x / y : std::max(x, y);
        r[k] = activate(v, op.act);
        for (size_t d = rank; d-- > 0;) {                                   // the next position of the output, the inputs follow by their strides
          idx[d]++;
          ia += sta[d];
          ib += stb[d];
          if (idx[d] < so[d]) break;
          ia -= sta[d] * size_t(so[d]);
          ib -= stb[d] * size_t(so[d]);
          idx[d] = 0;
        }
      }
      out.shape = so;
      out.f.swap(r);
      return true;
    }
    case MEAN: case SUM: {
      const std::vector<int> &axes = T(op.in[1]).i;
      const size_t rank = a.shape.size();
      std::vector<bool> red(rank, false);
      for (int ax : axes) red[size_t(ax < 0 ? ax + int(rank) : ax)] = true;
      std::vector<int> oshape;
      std::vector<size_t> ost(rank, 0);
      size_t reduced = 1;
      for (size_t d = 0; d < rank; d++) {
        if (red[d]) {
          reduced *= size_t(a.shape[d]);
          if (op.keep_dims) oshape.push_back(1);
        } else {
          oshape.push_back(a.shape[d]);
        }
      }
      size_t p = 1;
      for (size_t d = rank; d-- > 0;) {
        if (!red[d]) {
          ost[d] = p;
          p *= size_t(a.shape[d]);
        }
      }
      std::vector<float> r(p, 0.f);
      std::vector<int> idx(rank, 0);
      size_t io = 0;
      for (size_t k = 0; k < a.f.size(); k++) {
        r[io] += a.f[k];
        for (size_t d = rank; d-- > 0;) {
          idx[d]++;
          io += ost[d];
          if (idx[d] < a.shape[d]) break;
          io -= ost[d] * size_t(a.shape[d]);
          idx[d] = 0;
        }
      }
      if (op.code == MEAN)
        for (float &v : r) v /= float(reduced);
      out.shape = oshape;
      out.f.swap(r);
      return true;
    }
    case RESHAPE: {
      std::vector<int> shp = T(op.in[1]).i;
      size_t known = 1;
      int unknown = -1;
      for (size_t d = 0; d < shp.size(); d++) {
        if (shp[d] == -1) unknown = int(d);
        else known *= size_t(shp[d]);
      }
      if (unknown >= 0) shp[unknown] = int(known ? a.f.size() / known : 0);
      if (count(shp) != a.f.size()) return bad("reshape does not fit");
      out.shape = shp;
      out.f = a.f;
      return true;
    }
    case EXPAND_DIMS: {
      int ax = T(op.in[1]).i.empty() ? 0 : T(op.in[1]).i[0];
      std::vector<int> shp = a.shape;
      if (ax < 0) ax += int(shp.size()) + 1;
      shp.insert(shp.begin() + ax, 1);
      out.shape = shp;
      out.f = a.f;
      return true;
    }
    case TRANSPOSE: {
      const std::vector<int> &perm = T(op.in[1]).i;
      const size_t rank = a.shape.size();
      std::vector<int> oshape(rank);
      std::vector<size_t> in_st(rank);
      size_t p = 1;
      for (size_t d = rank; d-- > 0;) {
        in_st[d] = p;
        p *= size_t(a.shape[d]);
      }
      std::vector<size_t> st(rank);
      for (size_t d = 0; d < rank; d++) {
        oshape[d] = a.shape[size_t(perm[d])];
        st[d] = in_st[size_t(perm[d])];
      }
      std::vector<float> r(a.f.size());
      std::vector<int> idx(rank, 0);
      size_t ia = 0;
      for (size_t k = 0; k < r.size(); k++) {
        r[k] = a.f[ia];
        for (size_t d = rank; d-- > 0;) {
          idx[d]++;
          ia += st[d];
          if (idx[d] < oshape[d]) break;
          ia -= st[d] * size_t(oshape[d]);
          idx[d] = 0;
        }
      }
      out.shape = oshape;
      out.f.swap(r);
      return true;
    }
    case CONCATENATION: {
      const size_t rank = a.shape.size();
      const size_t ax = size_t(op.axis < 0 ? op.axis + int(rank) : op.axis);
      std::vector<int> oshape = a.shape;
      oshape[ax] = 0;
      for (int in_idx : op.in) oshape[ax] += T(in_idx).shape[ax];
      size_t outer = 1, inner = 1;
      for (size_t d = 0; d < ax; d++) outer *= size_t(a.shape[d]);
      for (size_t d = ax + 1; d < rank; d++) inner *= size_t(a.shape[d]);
      std::vector<float> r;
      r.reserve(count(oshape));
      for (size_t o = 0; o < outer; o++)
        for (int in_idx : op.in) {
          const Tensor &t = T(in_idx);
          const size_t chunk = size_t(t.shape[ax]) * inner;
          r.insert(r.end(), t.f.begin() + long(o * chunk), t.f.begin() + long((o + 1) * chunk));
        }
      out.shape = oshape;
      out.f.swap(r);
      return true;
    }
    case SOFTMAX: {
      out.shape = a.shape;
      out.f = a.f;
      const size_t last = size_t(a.shape.back()), rows = a.f.size() / last;
      for (size_t r = 0; r < rows; r++) {
        float *p = out.f.data() + r * last, mx = p[0], sum = 0.f;
        for (size_t k = 1; k < last; k++) mx = std::max(mx, p[k]);
        for (size_t k = 0; k < last; k++) {
          p[k] = std::exp((p[k] - mx) * op.beta);
          sum += p[k];
        }
        for (size_t k = 0; k < last; k++) p[k] /= sum;
      }
      return true;
    }
    case FULLY_CONNECTED: {
      const Tensor &w = T(op.in[1]);
      const int O = w.shape[0], I = w.shape[1];
      if (a.f.size() % size_t(I)) return bad("input does not fit the weights");
      const size_t B = a.f.size() / size_t(I);
      const Tensor *bias = op.in.size() > 2 && op.in[2] >= 0 ? &T(op.in[2]) : nullptr;
      out.f.assign(B * size_t(O), 0.f);
      for (size_t b = 0; b < B; b++)
        for (int o = 0; o < O; o++) {
          float s = bias ? bias->f[size_t(o)] : 0.f;
          const float *x = &a.f[b * size_t(I)], *wr = &w.f[size_t(o) * size_t(I)];
          for (int i = 0; i < I; i++) s += x[i] * wr[i];
          out.f[b * size_t(O) + size_t(o)] = activate(s, op.act);
        }
      if (op.keep_dims) {
        out.shape = a.shape;
        out.shape.back() = O;
      } else {
        out.shape = {int(B), O};
      }
      return true;
    }
    case CONV_2D: {
      const Tensor &w = T(op.in[1]);
      if (a.shape.size() != 4 || w.shape.size() != 4) return bad("convolution needs 4 dimensions");
      const int N = a.shape[0], H = a.shape[1], W = a.shape[2], C = a.shape[3];
      const int O = w.shape[0], KH = w.shape[1], KW = w.shape[2], FI = w.shape[3];
      if (FI <= 0 || C % FI || O % (C / FI)) return bad("convolution groups do not fit");
      const int groups = C / FI, opg = O / groups;
      const Tensor *bias = op.in.size() > 2 && op.in[2] >= 0 ? &T(op.in[2]) : nullptr;
      const int eh = (KH - 1) * op.dil_h + 1, ew = (KW - 1) * op.dil_w + 1;
      int OH, OW, pt, pl;
      if (op.padding == 0) {                                                  // SAME: the output has as many places as the input (per stride), the missing edge splits (the extra one at the end)
        OH = (H + op.stride_h - 1) / op.stride_h;
        OW = (W + op.stride_w - 1) / op.stride_w;
        pt = std::max(0, (OH - 1) * op.stride_h + eh - H) / 2;
        pl = std::max(0, (OW - 1) * op.stride_w + ew - W) / 2;
      } else {                                                                // VALID
        OH = (H - eh) / op.stride_h + 1;
        OW = (W - ew) / op.stride_w + 1;
        pt = pl = 0;
      }
      if (OH <= 0 || OW <= 0) return bad("input too short for the convolution");
      out.shape = {N, OH, OW, O};
      out.f.assign(size_t(N) * OH * OW * O, 0.f);
      for (int n = 0; n < N; n++)
        for (int oh = 0; oh < OH; oh++)
          for (int ow = 0; ow < OW; ow++)
            for (int o = 0; o < O; o++) {
              const int g = o / opg;
              float s = bias ? bias->f[size_t(o)] : 0.f;
              for (int kh = 0; kh < KH; kh++) {
                const int ih = oh * op.stride_h - pt + kh * op.dil_h;
                if (ih < 0 || ih >= H) continue;
                for (int kw = 0; kw < KW; kw++) {
                  const int iw = ow * op.stride_w - pl + kw * op.dil_w;
                  if (iw < 0 || iw >= W) continue;
                  const float *x = &a.f[((size_t(n) * H + ih) * W + iw) * C + size_t(g) * FI];
                  const float *wr = &w.f[((size_t(o) * KH + kh) * KW + kw) * FI];
                  for (int ci = 0; ci < FI; ci++) s += x[ci] * wr[ci];
                }
              }
              out.f[((size_t(n) * OH + oh) * OW + ow) * O + o] = activate(s, op.act);
            }
      return true;
    }
  }
  return bad("not implemented");
}

}  // namespace cs
