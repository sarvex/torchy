// Copyright (c) 2021-present The Torchy Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

// TODO: deep copy input tensors modified in place
// TODO: lazy in-place modification for torchy tensors. copy otherwise

#undef NDEBUG
#include "dispatch.h"
#include <ATen/NativeFunctions.h>
#include <ATen/Tensor.h>
#include <c10/util/variant.h>
#include <torch/library.h>
#include <iostream>
#include <map>
#include <type_traits>

// FIXME: for the interpreter
#include <ATen/RedispatchFunctions.h>

#define MAX_TRACE_LENGTH 64

#if 1
#define DBG(x) x
#else
#define DBG(x)
#endif

#define ENTER(f) cout << "Enter " f << endl

#ifdef C10_DISABLE_TENSORIMPL_EXTENSIBILITY
# error Cannot disable C10_DISABLE_TENSORIMPL_EXTENSIBILITY
#endif

using namespace at;
using namespace std;

namespace {

class TorchyTensor;
using TorchTensorImpl = c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl>;

using UnionInputTys = c10::variant<
  int64_t,
  IntArrayRef,
  c10::optional<int64_t>,
  c10::optional<ScalarType>,
  Scalar,
  Tensor
>;

unsigned trace_idx(const Tensor &t);
void print(ostream &os, const TorchyTensor &tt);
void set(TorchyTensor *tt, Tensor &&t);


struct TensorOp {
  TorchyTensor *tensor;
  const char *id;
  vector<UnionInputTys> args;
  DispatchKeySet dispatch_key;
  unsigned refs;

  void incref() {
    assert(isObservable());
    ++refs;
  }

  void decref(TensorOp *ops) {
    assert(refs > 0);
    --refs;

    if (refs == 0) {
      for (auto &arg : args) {
        if (auto t = get_if<Tensor>(&arg)) {
          auto idx = trace_idx(*t);
          if (idx != -1u)
            ops[idx].decref(ops);
        }
      }
    }
  }

  bool isObservable() const {
    assert(!tensor || refs > 0);
    return tensor;
  }

  bool needsComputing() const {
    return refs > 0;
  }

  void print(ostream &os, map<const TensorImpl*, unsigned> &inputs) const {
    if (!needsComputing()) {
      os << "[dead]";
      return;
    }

    os << id;
    bool first = true;
    for (auto &arg : args) {
      os << (first ? " " : ", ");
      first = false;
      if (auto t = get_if<Tensor>(&arg)) {
        auto idx = trace_idx(*t);
        if (idx != -1u) {
          os << '%' << idx;
        } else {
          auto n = inputs.emplace(t->unsafeGetTensorImpl(),
                                  (unsigned)inputs.size()).first->second;
          os << "in<" << n << '>';
        }
      } else if (auto s = get_if<Scalar>(&arg)) {
        os << *s;
      } else if (auto a = get_if<IntArrayRef>(&arg)) {
        os << *a;
      } else if (auto i = get_if<int64_t>(&arg)) {
        os << *i;
      } else if (auto o = get_if<c10::optional<int64_t>>(&arg)) {
        if (*o)
          os << **o;
        else
          os << "(null)";
      } else if (auto s = get_if<c10::optional<ScalarType>>(&arg)) {
        if (*s)
          os << **s;
        else
          os << "(null)";
      } else {
        assert(false);
      }
    }

  if (refs > 1)
    os << " #refs=" << (refs-1);

  if (isObservable())
    os << " #output";
  }
};


class Trace {
  TensorOp ops[MAX_TRACE_LENGTH];
  unsigned next_op = 0;
  bool flushing = false;

  template <typename T>
  void incref(T t) {}

  void incref(const Tensor &t) {
    auto idx = trace_idx(t);
    if (idx != -1u) {
      assert(idx < next_op);
      ops[idx].incref();
    }
  }

  vector<unique_ptr<unsigned char[]>> deep_copies;
  IntArrayRef deep_copy(IntArrayRef arr) {
    size_t size = arr.size() * sizeof(int64_t);
    auto ptr = new unsigned char[size];
    memcpy(ptr, arr.data(), size);
    deep_copies.emplace_back(ptr);
    return { (int64_t*)ptr, arr.size() };
  }

  template<typename A>
  void registerOpArg(TensorOp &op, A arg) {
    op.args.emplace_back(move(arg));
  }

  void registerOpArg(TensorOp &op, IntArrayRef arg) {
    op.args.emplace_back(deep_copy(arg));
  }

  template<typename A, typename... T>
  void registerOpArgs(TensorOp &op, const A &arg, T&... args) {
    registerOpArg(op, arg);
    incref(arg);
    registerOpArgs(op, args...);
  }

  void registerOpArgs(TensorOp &op) {}

public:
  bool is_flushing() const {
    return flushing;
  }

  void set_flushing(bool val) {
    flushing = val;
  }

  template<typename... T>
  unsigned register_tensor(TorchyTensor *tensor, DispatchKeySet ks,
                           const char *op_id, T&... args) {
    assert(!flushing);
    if (next_op == MAX_TRACE_LENGTH)
      flush();

    auto &op = ops[next_op];
    op.tensor = tensor;
    op.id = op_id;
    op.args.clear();
    registerOpArgs(op, args...);
    op.refs = 1;
    op.dispatch_key = ks;
    return next_op++;
  }

  void set_unobservable(unsigned idx) {
    auto &op = ops[idx];
    assert(op.tensor);
    op.tensor = nullptr;
    op.decref(ops);

    // reclaim slot if this was the last created tensor
    if (op.refs == 0 && idx+1 == next_op) {
      --next_op;
    }
  }

  void flush() {
    assert(!flushing);
    flushing = true;
    DBG(cout << "Flush trace\n" << *this;)

    for (unsigned i = 0; i < next_op; ++i) {
      auto &op = ops[i];
      if (!op.needsComputing())
        continue;

      auto dispatch_key = op.dispatch_key;
      for (auto &arg : op.args) {
        if (auto t = get_if<Tensor>(&arg)) {
          dispatch_key = dispatch_key | t->key_set();
        }
      }
      dispatch_key
       = dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY);

      if (!strcmp(op.id, "abs")) {
        set(op.tensor,
            at::redispatch::abs(dispatch_key, get<Tensor>(op.args[0])));
      } else if (!strcmp(op.id, "add_Tensor")) {
        set(op.tensor,
            at::redispatch::add(dispatch_key, get<Tensor>(op.args[0]),
                                get<Tensor>(op.args[1]),
                                get<Scalar>(op.args[2])));
      } else if (!strcmp(op.id, "as_strided")) {
        set(op.tensor,
            at::redispatch::as_strided(dispatch_key,
              get<Tensor>(op.args[0]), get<IntArrayRef>(op.args[1]),
              get<IntArrayRef>(op.args[2]),
              get<c10::optional<int64_t>>(op.args[3])));
      } else if (!strcmp(op.id, "eq_Tensor")) {
        set(op.tensor, at::redispatch::eq(dispatch_key, get<Tensor>(op.args[0]),
                                          get<Tensor>(op.args[1])));
      } else if (!strcmp(op.id, "gt_Scalar")) {
        set(op.tensor, at::redispatch::gt(dispatch_key, get<Tensor>(op.args[0]),
                                          get<Scalar>(op.args[1])));
      } else if (!strcmp(op.id, "isfinite")) {
        set(op.tensor,
            at::redispatch::isfinite(dispatch_key, get<Tensor>(op.args[0])));
      } else if (!strcmp(op.id, "masked_select")) {
        set(op.tensor,
            at::redispatch::masked_select(dispatch_key,
                                          get<Tensor>(op.args[0]),
                                          get<Tensor>(op.args[1])));
      } else if (!strcmp(op.id, "max")) {
        set(op.tensor, at::redispatch::max(dispatch_key,
                                           get<Tensor>(op.args[0])));
      } else if (!strcmp(op.id, "min")) {
        set(op.tensor, at::redispatch::min(dispatch_key,
                                           get<Tensor>(op.args[0])));
      } else if (!strcmp(op.id, "mul_Tensor")) {
        set(op.tensor,
            at::redispatch::mul(dispatch_key, get<Tensor>(op.args[0]),
                                get<Tensor>(op.args[1])));
      } else if (!strcmp(op.id, "ne_Scalar")) {
        set(op.tensor,
            at::redispatch::ne(dispatch_key, get<Tensor>(op.args[0]),
                              get<Scalar>(op.args[1])));
      } else if (!strcmp(op.id, "ne_Tensor")) {
        set(op.tensor,
          at::redispatch::ne(dispatch_key, get<Tensor>(op.args[0]),
                             get<Tensor>(op.args[1])));
      } else if (!strcmp(op.id, "reshape")) {
        set(op.tensor,
            at::redispatch::reshape(dispatch_key, get<Tensor>(op.args[0]),
                                    get<IntArrayRef>(op.args[1])));
      } else if (!strcmp(op.id, "select_int")) {
        set(op.tensor,
            at::redispatch::select(dispatch_key, get<Tensor>(op.args[0]),
                                   get<int64_t>(op.args[1]),
                                   get<int64_t>(op.args[2])));
      } else if (!strcmp(op.id, "sum")) {
        set(op.tensor,
            at::redispatch::sum(dispatch_key, get<Tensor>(op.args[0]),
                                get<c10::optional<ScalarType>>(op.args[1])));
      } else if (!strcmp(op.id, "view")) {
        set(op.tensor,
            at::redispatch::view(dispatch_key, get<Tensor>(op.args[0]),
                                get<IntArrayRef>(op.args[1])));
      } else {
        assert(0);
      }

      DBG(cout << '%' << i << " = "; print(cout, *op.tensor); cout << endl;)
    }

    next_op = 0;
    flushing = false;
    deep_copies.clear();
  }

  friend ostream& operator<<(ostream &os, const Trace &t) {
    if (t.next_op == 0)
      os << "(empty)\n";

    map<const TensorImpl*, unsigned> inputs_map;
    for (unsigned i = 0; i < t.next_op; ++i) {
      os << '%' << i << " = ";
      t.ops[i].print(os, inputs_map);
      os << '\n';
    }
    return os << endl;
  }
};

thread_local Trace trace;


class ScopedAutoFlush {
  bool was_flushing;
public:
  ScopedAutoFlush() : was_flushing(trace.is_flushing()) {
    if (!was_flushing) {
      trace.flush();
      trace.set_flushing(true);
    }
  }

  ~ScopedAutoFlush() {
    trace.set_flushing(was_flushing);
  }
};


class TorchyTensor final : public TensorImpl {
  TorchTensorImpl tensor;
  unsigned trace_idx;
  bool materialized = false;

public:
template<typename... T>
  TorchyTensor(caffe2::TypeMeta dtype, c10::Device device, DispatchKeySet ks,
               const char *op_id, const T&... args)
    : TensorImpl(DISPATCHKEY, dtype, device) {
    trace_idx = trace.register_tensor(this, ks, op_id, args...);
  }

  unsigned getTraceIdx() const { return trace_idx; }

  void set(Tensor &&t) {
    assert(!materialized && !tensor);
    assert(dtype() == t.dtype());
    trace_idx    = -1u;
    materialized = true;
    tensor       = t.unsafeReleaseIntrusivePtr();

    auto my_ks = key_set_;
    TensorImpl::shallow_copy_from(tensor);
    key_set_ = key_set_ | my_ks;
  }

  void ensure_materialized() const {
    if (!materialized) {
      trace.flush();
      assert(materialized);
    }
    assert(tensor);
  }

  friend ostream& operator<<(ostream &os, const TorchyTensor &tt) {
    return os << Tensor(tt.tensor);
  }

  void release_resources() override {
    if (tensor)
      tensor->release_resources();
    if (trace_idx != -1u)
      trace.set_unobservable(trace_idx);
    TensorImpl::release_resources();
  }

  IntArrayRef sizes() const override {
    ensure_materialized();
    return tensor->sizes();
  }

  IntArrayRef strides() const override {
    ensure_materialized();
    return tensor->strides();
  }

  int64_t dim() const override {
    ensure_materialized();
    return tensor->dim();
  }

  int64_t numel() const override {
    ensure_materialized();
    return tensor->numel();
  }

  bool is_contiguous(at::MemoryFormat memory_format) const override {
    ensure_materialized();
    return tensor->is_contiguous(memory_format);
  }

  int64_t storage_offset() const override {
    ensure_materialized();
    return tensor->storage_offset();
  }

  const char* tensorimpl_type_name() const override {
    return "TorchyTensor";
  }

  void set_size(int64_t dim, int64_t new_size) override {
    ensure_materialized();
    tensor->set_size(dim, new_size);
    TensorImpl::set_size(dim, new_size);
  }

  void set_stride(int64_t dim, int64_t new_stride) override {
    ensure_materialized();
    tensor->set_stride(dim, new_stride);
    TensorImpl::set_stride(dim, new_stride);
  }

  void set_storage_offset(int64_t storage_offset) override {
    ensure_materialized();
    tensor->set_storage_offset(storage_offset);
    TensorImpl::set_storage_offset(storage_offset);
  }

  int64_t size(int64_t d) const override {
    ensure_materialized();
    return tensor->size(d);
  }

  int64_t stride(int64_t d) const override {
    ensure_materialized();
    return tensor->stride(d);
  }

  c10::intrusive_ptr<TensorImpl>
  shallow_copy_and_detach(const c10::VariableVersion &version_counter,
                          bool allow_tensor_metadata_change) const override {
    assert(0 && "TorchyTensor::shallow_copy_and_detach(1)");
    return {};
  }

  c10::intrusive_ptr<TensorImpl>
  shallow_copy_and_detach(c10::VariableVersion &&version_counter,
                          bool allow_tensor_metadata_change) const override {
    assert(0 && "TorchyTensor::shallow_copy_and_detach(2)");
    return {};
  }

  void shallow_copy_from(const c10::intrusive_ptr<TensorImpl> &impl) override {
    assert(0 && "TorchyTensor::shallow_copy_from");
  }
};

TorchyTensor* is_torchy(const Tensor &t) {
  return dynamic_cast<TorchyTensor*>(t.unsafeGetTensorImpl());
}

unsigned trace_idx(const Tensor &t) {
  if (auto tt = is_torchy(t))
    return tt->getTraceIdx();
  return -1u;
}

void print(ostream &os, const TorchyTensor &tensor) {
  os << tensor;
}

void set(TorchyTensor *tt, Tensor &&t) {
  tt->set(move(t));
}

void ensure_materialized() {}

template<typename... T>
void ensure_materialized(const Tensor &t, T&... args) {
  if (auto tt = is_torchy(t))
    tt->ensure_materialized();
  ensure_materialized(args...);
}

void will_override(const Tensor &t) {
  // TODO: refine this to account only for unprocessed references
  if (is_torchy(t)) {
    if (!trace.is_flushing())
      trace.flush();
  }
}

#define MK_TORCHY(type, device, op, ...) \
  at::detail::make_tensor<TorchyTensor>(type, device, ks, op, __VA_ARGS__)


Tensor abs(c10::DispatchKeySet ks, const Tensor &self) {
  ENTER("abs");
  if (trace.is_flushing()) {
    ensure_materialized(self);
    return
      at::redispatch::abs(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY), self);
  }
  return MK_TORCHY(self.dtype(), self.device(), "abs", self);
}

Tensor& abs_out(c10::DispatchKeySet ks, const Tensor &self, Tensor &out) {
  ENTER("abs_out");
  will_override(out);
  ensure_materialized(self, out);
  return
    at::redispatch::abs_out(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      out, self);
}

Tensor add_Tensor(c10::DispatchKeySet ks, const Tensor &self,
                  const Tensor &other, const Scalar &alpha) {
  ENTER("add_Tensor");
  if (trace.is_flushing()) {
    ensure_materialized(self, other);
    return
      at::redispatch::add(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
        self, other, alpha);
  }
  return MK_TORCHY(self.dtype(), self.device(), "add_Tensor", self, other,
                   alpha);
}

Tensor as_strided(c10::DispatchKeySet ks, const Tensor &self, IntArrayRef size,
                  IntArrayRef stride, c10::optional<int64_t> storage_offset) {
  ENTER("as_strided");
  if (trace.is_flushing()) {
    ensure_materialized(self);
    return
      at::redispatch::as_strided(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
        self, size, stride, storage_offset);
  }
  return MK_TORCHY(self.dtype(), self.device(), "as_strided", self, size,
                   stride, storage_offset);
}

Tensor& bitwise_and_Tensor_out(c10::DispatchKeySet ks, const Tensor &self,
                               const Tensor &other, Tensor &out) {
  ENTER("bitwise_and_Tensor_out");
  will_override(out);
  ensure_materialized(self, other, out);
  return
    at::redispatch::bitwise_and_out(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      out, self, other);
}

Tensor& ceil_out(c10::DispatchKeySet ks, const Tensor &self, Tensor &out) {
  ENTER("ceil_out");
  will_override(out);
  ensure_materialized(self, out);
  return
    at::redispatch::ceil_out(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      out, self);
}

Tensor& copy_(c10::DispatchKeySet ks, Tensor &self, const Tensor &src,
              bool non_blocking) {
  //ENTER("copy_");
  will_override(self);
  ensure_materialized(self, src);
  return
    at::redispatch::copy_(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      self, src, non_blocking);
}

Tensor& detach_(c10::DispatchKeySet ks, Tensor &self) {
  ENTER("detach_");
  will_override(self);
  ensure_materialized(self);
  return at::redispatch::detach_(
           ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY), self);
}

Tensor& div_out(c10::DispatchKeySet ks, const Tensor &self, const Tensor &other,
                Tensor &out) {
  ENTER("div_out");
  will_override(out);
  ensure_materialized(self, other, out);
  return at::redispatch::div_out(
           ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
           out, self, other);
}

Tensor empty_memory_format(c10::DispatchKeySet ks, IntArrayRef size,
                           c10::optional<ScalarType> dtype,
                           c10::optional<Layout> layout,
                           c10::optional<Device> device,
                           c10::optional<bool> pin_memory,
                           c10::optional<MemoryFormat> memory_format) {
  //return native::empty_cpu(size, dtype, layout, device, pin_memory,
  //                         memory_format);
  ENTER("empty_memory_format");
  return
    at::redispatch::empty(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      size, dtype, layout, device, pin_memory, memory_format);
}

Tensor empty_strided(c10::DispatchKeySet ks, IntArrayRef size,
                     IntArrayRef stride,
                     c10::optional<ScalarType> dtype,
                     c10::optional<Layout> layout,
                     c10::optional<Device> device,
                     c10::optional<bool> pin_memory) {
  //return
  //  native::empty_strided_cpu(size, stride, dtype, layout, device, pin_memory);
  ENTER("empty_strided");
  return
    at::redispatch::empty_strided(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      size, stride, dtype, layout, device, pin_memory);
}

Tensor eq_Tensor(c10::DispatchKeySet ks, const Tensor &self,
                 const Tensor &other) {
  ENTER("eq_Tensor");
  if (trace.is_flushing()) {
    ensure_materialized(self, other);
    return
      at::redispatch::eq(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
        self, other);
  }
  return MK_TORCHY(scalarTypeToTypeMeta(kBool), self.device(), "eq_Tensor",
                   self, other);
}

Tensor& eq_Tensor_out(c10::DispatchKeySet ks, const Tensor &self,
                      const Tensor &other, Tensor &out) {
  ENTER("eq_Tensor_out");
  will_override(out);
  ensure_materialized(self, other, out);
  return at::redispatch::eq_out(
           ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
           out, self, other);
}

Tensor& fill__Scalar(c10::DispatchKeySet ks, Tensor &self,
                     const Scalar &value) {
  ENTER("fill__Scalar");
  will_override(self);
  ensure_materialized(self);
  return
    at::redispatch::fill_(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      self, value);
}

Tensor gt_Scalar(c10::DispatchKeySet ks, const Tensor &self,
                 const Scalar &other) {
  ENTER("gt_Scalar");
  if (trace.is_flushing()) {
    ensure_materialized(self);
    return
      at::redispatch::gt(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
        self, other);
  }
  return MK_TORCHY(scalarTypeToTypeMeta(kBool), self.device(), "gt_Scalar",
                   self, other);
}

Tensor& gt_Tensor_out(c10::DispatchKeySet ks, const Tensor &self,
                      const Tensor &other, Tensor &out) {
  ENTER("gt_Tensor_out");
  will_override(out);
  ensure_materialized(self, other, out);
  return
    at::redispatch::gt_out(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      out, self, other);
}

Tensor isfinite(c10::DispatchKeySet ks, const Tensor &self) {
  ENTER("isfinite");
  if (trace.is_flushing()) {
    ensure_materialized(self);
    return
      at::redispatch::isfinite(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY), self);
  }
  return MK_TORCHY(scalarTypeToTypeMeta(kBool), self.device(), "isfinite",
                   self);
}

Scalar _local_scalar_dense(c10::DispatchKeySet ks, const Tensor &self) {
  ENTER("_local_scalar_dense");
  ensure_materialized(self);
  return at::redispatch::_local_scalar_dense(
           ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY), self);
}

Tensor masked_select(c10::DispatchKeySet ks, const Tensor &self,
                     const Tensor &mask) {
  ENTER("masked_select");
  if (trace.is_flushing()) {
    ensure_materialized(self, mask);
    return
      at::redispatch::masked_select(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
        self, mask);
  }
  return MK_TORCHY(self.dtype(), self.device(), "masked_select", self, mask);
}

Tensor max(c10::DispatchKeySet ks, const Tensor &self) {
  ENTER("max");
  if (trace.is_flushing()) {
    ensure_materialized(self);
    return
      at::redispatch::max(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY), self);
  }
  return MK_TORCHY(self.dtype(), self.device(), "max", self);
}

Tensor min(c10::DispatchKeySet ks, const Tensor &self) {
  ENTER("min");
  if (trace.is_flushing()) {
    ensure_materialized(self);
    return
      at::redispatch::min(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY), self);
  }
  return MK_TORCHY(self.dtype(), self.device(), "min", self);
}

Tensor& mul_out(c10::DispatchKeySet ks, const Tensor &self,
                const Tensor &other, Tensor &out) {
  ENTER("mul_out");
  will_override(out);
  ensure_materialized(self, other, out);
  return at::redispatch::mul_out(
    ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
    out, self, other);
}

Tensor mul_Tensor(c10::DispatchKeySet ks, const Tensor &self,
                  const Tensor &other) {
  ENTER("mul_Tensor");
  if (trace.is_flushing()) {
    ensure_materialized(self, other);
    return
      at::redispatch::mul(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
        self, other);
  }
  return MK_TORCHY(self.dtype(), self.device(), "mul_Tensor", self, other);
}

Tensor ne_Scalar(c10::DispatchKeySet ks, const Tensor &self,
                 const Scalar &other) {
  ENTER("ne_Scalar");
  if (trace.is_flushing()) {
    ensure_materialized(self);
    return
      at::redispatch::ne(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
        self, other);
  }
  return MK_TORCHY(scalarTypeToTypeMeta(kBool), self.device(), "ne_Scalar",
                   self, other);
}

Tensor ne_Tensor(c10::DispatchKeySet ks, const Tensor &self,
                 const Tensor &other) {
  ENTER("ne_Tensor");
  if (trace.is_flushing()) {
    ensure_materialized(self, other);
    return
      at::redispatch::ne(
        ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
        self, other);
  }
  return MK_TORCHY(scalarTypeToTypeMeta(kBool), self.device(), "ne_Tensor",
                   self, other);
}

Tensor& ne_Tensor_out(c10::DispatchKeySet ks, const Tensor &self,
                      const Tensor &other, Tensor &out) {
  ENTER("ne_Tensor_out");
  will_override(out);
  ensure_materialized(self, other, out);
  return
    at::redispatch::ne_out(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      out, self, other);
}

Tensor reshape(c10::DispatchKeySet ks, const Tensor &self, IntArrayRef shape) {
  ENTER("reshape");
  if (trace.is_flushing()) {
    ensure_materialized(self);
    return
      at::redispatch::reshape(
         ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
         self, shape);
  }
  return MK_TORCHY(self.dtype(), self.device(), "reshape", self, shape);
}

const Tensor& resize_(c10::DispatchKeySet ks, const Tensor &self,
                      IntArrayRef size,
                      c10::optional<MemoryFormat> memory_format) {
  ENTER("resize_");
  will_override(self);
  ensure_materialized(self);
  return
    at::redispatch::resize_(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      self, size, memory_format);
}

Tensor select_int(c10::DispatchKeySet ks, const Tensor &self, int64_t dim,
                  int64_t index) {
  ENTER("select_int");
  if (trace.is_flushing()) {
    ensure_materialized(self);
    return
      at::redispatch::select(
         ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
         self, dim, index);
  }
  return MK_TORCHY(self.dtype(), self.device(), "select_int", self, dim, index);
}

Tensor sum(c10::DispatchKeySet ks, const Tensor &self,
           c10::optional<ScalarType> dtype) {
  ENTER("sum");
  if (trace.is_flushing()) {
    ensure_materialized(self);
    return
      at::redispatch::sum(
         ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
         self, dtype);
  }
  auto ty = self.dtype();
  if (ty == kBool)
    ty = scalarTypeToTypeMeta(kLong);
  return MK_TORCHY(dtype ? scalarTypeToTypeMeta(*dtype) : ty, self.device(),
                   "sum", self, dtype);
}

Tensor to_device(c10::DispatchKeySet ks, const Tensor &self,
                 Device device, ScalarType dtype, bool non_blocking, bool copy,
                 c10::optional<MemoryFormat> memory_format) {
  //ENTER("to_device");
  ensure_materialized(self);
  return
    at::redispatch::to(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      self, device, dtype, non_blocking, copy, memory_format);
}

Tensor view(c10::DispatchKeySet ks, const Tensor &self, IntArrayRef size) {
  ENTER("view");
  if (trace.is_flushing()) {
    ensure_materialized(self);
    return
      at::redispatch::view(
         ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
         self, size);
  }
  return MK_TORCHY(self.dtype(), self.device(), "view", self, size);
}

TORCH_LIBRARY_IMPL(aten, DISPATCHKEY_NO_NS, m) {
  m.impl("abs", abs);
  m.impl("abs.out", abs_out);
  m.impl("add.Tensor", add_Tensor);
  m.impl("as_strided", as_strided);
  m.impl("bitwise_and.Tensor_out", bitwise_and_Tensor_out);
  m.impl("ceil.out", ceil_out);
  m.impl("copy_", copy_);
  m.impl("detach_", detach_); // FIXME: RegisterDefaultBackend
  m.impl("div.out", div_out);
  m.impl("empty.memory_format", empty_memory_format); // FIXME: not called
  m.impl("empty_strided", empty_strided); // FIXME: not called
  m.impl("eq.Tensor", eq_Tensor);
  m.impl("eq.Tensor_out", eq_Tensor_out);
  m.impl("fill_.Scalar", fill__Scalar);
  m.impl("gt.Scalar", gt_Scalar);
  m.impl("gt.Tensor_out", gt_Tensor_out);
  m.impl("isfinite", isfinite);
  m.impl("_local_scalar_dense", _local_scalar_dense);
  m.impl("masked_select", masked_select);
  m.impl("max", max);
  m.impl("min", min);
  m.impl("mul.out", mul_out);
  m.impl("mul.Tensor", mul_Tensor);
  m.impl("ne.Scalar", ne_Scalar);
  m.impl("ne.Tensor", ne_Tensor);
  m.impl("ne.Tensor_out", ne_Tensor_out);
  m.impl("reshape", reshape); // FIXME: RegisterMath
  m.impl("resize_", resize_);
  m.impl("select.int", select_int);
  m.impl("sum", sum);
  m.impl("to.device", to_device); // FIXME: RegisterMath
  m.impl("view", view);
}

TORCH_LIBRARY_IMPL(aten, AutogradPrivateUse1, m) {
  m.impl("isfinite", isfinite);
  m.impl("reshape", reshape);
  m.impl("to.device", to_device);
}

}
