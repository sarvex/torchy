// Copyright (c) 2021-present The Torchy Authors.
// Distributed under the MIT license that can be found in the LICENSE file.

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

#ifdef C10_DISABLE_TENSORIMPL_EXTENSIBILITY
# error Cannot disable C10_DISABLE_TENSORIMPL_EXTENSIBILITY
#endif

using namespace at;
using namespace std;

namespace {

using TorchTensorImpl = c10::intrusive_ptr<TensorImpl, UndefinedTensorImpl>;

using UnionInputTys = c10::variant<
  IntArrayRef,
  c10::optional<int64_t>,
  Scalar,
  Tensor
>;

unsigned trace_idx(const Tensor &t);


struct TensorOp {
  TorchTensorImpl *tensor;
  const char *id;
  vector<UnionInputTys> args;
  unsigned refs;
  DispatchKeySet dispatch_key;

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

  void print(ostream &os, map<const Tensor*, unsigned> &inputs) const {
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
          auto n = inputs.emplace(t, (unsigned)inputs.size()).first->second;
          os << "in<" << n << '>';
        }
      } else if (auto s = get_if<Scalar>(&arg)) {
        os << *s;
      } else if (auto a = get_if<IntArrayRef>(&arg)) {
        os << *a;
      } else if (auto o = get_if<c10::optional<int64_t>>(&arg)) {
        if (*o)
          os << **o;
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

  template <typename T>
  void incref(T t) {}

  void incref(const Tensor &t) {
    auto idx = trace_idx(t);
    if (idx != -1u)
      ops[idx].incref();
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
  template<typename... T>
  unsigned register_tensor(TorchTensorImpl *tensor, DispatchKeySet ks,
                           const char *op_id, T&... args) {
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
    DBG(cout << "Flush trace\n" << *this;)
    for (unsigned i = 0; i < next_op; ++i) {
      auto &op = ops[i];
      if (!op.needsComputing())
        continue;

      if (!strcmp(op.id, "abs")) {
        *op.tensor
          = at::redispatch::abs(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]))
            .unsafeReleaseIntrusivePtr();
      } else if (!strcmp(op.id, "add_Tensor")) {
        *op.tensor
          = at::redispatch::add(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]),
            get<Tensor>(op.args[1]),
            get<Scalar>(op.args[2]))
            .unsafeReleaseIntrusivePtr();
      } else if (!strcmp(op.id, "as_strided")) {
        *op.tensor
          = at::redispatch::as_strided(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]),
            get<IntArrayRef>(op.args[1]),
            get<IntArrayRef>(op.args[2]),
            get<c10::optional<int64_t>>(op.args[3]))
            .unsafeReleaseIntrusivePtr();
      } else if (!strcmp(op.id, "eq_Tensor")) {
        *op.tensor
          = at::redispatch::eq(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]),
            get<Tensor>(op.args[1]))
            .unsafeReleaseIntrusivePtr();
      } else if (!strcmp(op.id, "masked_select")) {
        *op.tensor
          = at::redispatch::masked_select(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]), get<Tensor>(op.args[1]))
            .unsafeReleaseIntrusivePtr();
      } else if (!strcmp(op.id, "max")) {
        *op.tensor
          = at::redispatch::max(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]))
            .unsafeReleaseIntrusivePtr();
      } else if (!strcmp(op.id, "min")) {
        *op.tensor
          = at::redispatch::max(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]))
            .unsafeReleaseIntrusivePtr();
      } else if (!strcmp(op.id, "mul_Tensor")) {
        *op.tensor
          = at::redispatch::mul(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]),
            get<Tensor>(op.args[1]))
            .unsafeReleaseIntrusivePtr();
      } else if (!strcmp(op.id, "ne_Scalar")) {
        *op.tensor
          = at::redispatch::ne(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]),
            get<Scalar>(op.args[1]))
            .unsafeReleaseIntrusivePtr();
      } else if (!strcmp(op.id, "ne_Tensor")) {
        *op.tensor
          = at::redispatch::ne(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]),
            get<Tensor>(op.args[1]))
            .unsafeReleaseIntrusivePtr();
      } else if (!strcmp(op.id, "reshape")) {
        *op.tensor
          = at::redispatch::reshape(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]),
            get<IntArrayRef>(op.args[1]))
            .unsafeReleaseIntrusivePtr();
      } else if (!strcmp(op.id, "view")) {
        *op.tensor
          = at::redispatch::view(op.dispatch_key & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
            get<Tensor>(op.args[0]), get<IntArrayRef>(op.args[1]))
            .unsafeReleaseIntrusivePtr();
      } else {
        assert(0);
      }

       DBG(cout << '%' << i << " = " << Tensor(*op.tensor) << endl;)
    }

    next_op = 0;
    deep_copies.clear();
  }

  friend ostream& operator<<(ostream &os, const Trace &t) {
    if (t.next_op == 0)
      return os << "empty trace";

    map<const Tensor*, unsigned> inputs_map;
    for (unsigned i = 0; i < t.next_op; ++i) {
      os << '%' << i << " = ";
      t.ops[i].print(os, inputs_map);
      os << '\n';
    }
    return os << endl;
  }
};

thread_local Trace trace;


class TorchyTensor final : public TensorImpl {
  TorchTensorImpl tensor;
  unsigned trace_idx;

public:
template<typename... T>
  TorchyTensor(caffe2::TypeMeta dtype, c10::Device device, DispatchKeySet ks,
               const char *op_id, const T&... args)
    : TensorImpl(DISPATCHKEY, dtype, device) {
    trace_idx = trace.register_tensor(&tensor, ks, op_id, args...);
  }

  unsigned getTraceIdx() const { return trace_idx; }

  void ensure_tensor() const {
    if (!tensor) {
      trace.flush();
      assert(tensor);
    }
  }

  void release_resources() override {
    if (tensor)
      tensor->release_resources();
    else
      trace.set_unobservable(trace_idx);
    TensorImpl::release_resources();
  }

  IntArrayRef sizes() const override {
    ensure_tensor();
    return tensor->sizes();
  }

  IntArrayRef strides() const override {
    ensure_tensor();
    return tensor->strides();
  }

  int64_t dim() const override {
    ensure_tensor();
    return tensor->dim();
  }

  bool has_storage() const override {
    ensure_tensor();
    return tensor->has_storage();
  }

  int64_t numel() const override {
    ensure_tensor();
    return tensor->numel();
  }

  const char* tensorimpl_type_name() const override {
    return "TorchyTensor";
  }

  void set_size(int64_t dim, int64_t new_size) override {
    ensure_tensor();
    tensor->set_size(dim, new_size);
  }

  void set_stride(int64_t dim, int64_t new_stride) override {
    ensure_tensor();
    tensor->set_stride(dim, new_stride);
  }

  void set_storage_offset(int64_t storage_offset) override {
    ensure_tensor();
    tensor->set_storage_offset(storage_offset);
  }

  int64_t size(int64_t d) const override {
    ensure_tensor();
    return tensor->size(d);
  }

  int64_t stride(int64_t d) const override {
    ensure_tensor();
    return tensor->stride(d);
  }

  c10::intrusive_ptr<TensorImpl>
  shallow_copy_and_detach(const c10::VariableVersion &version_counter,
                          bool allow_tensor_metadata_change) const override {
    TORCH_CHECK_NOT_IMPLEMENTED(false,
                                "TorchyTensor::shallow_copy_and_detach(1)");
    return {};
  }

  c10::intrusive_ptr<TensorImpl>
  shallow_copy_and_detach(c10::VariableVersion &&version_counter,
                          bool allow_tensor_metadata_change) const override {
    TORCH_CHECK_NOT_IMPLEMENTED(false,
                                "TorchyTensor::shallow_copy_and_detach(2)");
    return {};
  }

  void shallow_copy_from(const c10::intrusive_ptr<TensorImpl> &impl) override {
    TORCH_CHECK_NOT_IMPLEMENTED(false, "TorchyTensor::shallow_copy_from");
  }
};

TorchyTensor* is_torchy(const Tensor &t) {
  return t.key_set().has(DISPATCHKEY) ? (TorchyTensor*)t.unsafeGetTensorImpl()
                                      : nullptr;
}

unsigned trace_idx(const Tensor &t) {
  if (auto tt = is_torchy(t))
    return tt->getTraceIdx();
  return -1u;
}

void ensure_materialized(const Tensor &t) {
  if (auto tt = is_torchy(t))
    tt->ensure_tensor();
}


Tensor abs(c10::DispatchKeySet ks, const Tensor &self) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "abs", self);
}

Tensor& abs_out(c10::DispatchKeySet ks, const Tensor &self, Tensor &out) {
  // cant delay this without changing the TensorImpl of out
  // TODO: should we?
  return
    at::redispatch::abs_out(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      out, self);
}

Tensor add_Tensor(c10::DispatchKeySet ks, const Tensor &self,
                  const Tensor &other, const Scalar &alpha) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "add_Tensor", self, other,
                                               alpha);
}

Tensor as_strided(c10::DispatchKeySet ks, const Tensor &self, IntArrayRef size,
                  IntArrayRef stride, c10::optional<int64_t> storage_offset) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "as_strided", self, size, stride,
                                               storage_offset);
}

Tensor& bitwise_and_Tensor_out(c10::DispatchKeySet ks, const Tensor &self,
                               const Tensor &other, Tensor &out) {
  // cant delay this without changing the TensorImpl of out
  // TODO: should we?
  return
    at::redispatch::bitwise_and_out(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      out, self, other);
}

Tensor& ceil_out(c10::DispatchKeySet ks, const Tensor &self, Tensor &out) {
  // cant delay this without changing the TensorImpl of out
  // TODO: should we?
  return
    at::redispatch::ceil_out(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      out, self);
}

Tensor& copy_(c10::DispatchKeySet ks, Tensor &self, const Tensor &src,
              bool non_blocking) {
  // TODO: can be made lazy?
  ensure_materialized(src);
  return
    at::redispatch::copy_(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      self, src, non_blocking);
}

Tensor& detach_(c10::DispatchKeySet ks, Tensor &self) {
  cout << "Called detach_" << endl;
  return self;
}

Tensor empty_memory_format(c10::DispatchKeySet ks, IntArrayRef size,
                           c10::optional<ScalarType> dtype,
                           c10::optional<Layout> layout,
                           c10::optional<Device> device,
                           c10::optional<bool> pin_memory,
                           c10::optional<MemoryFormat> memory_format) {
  cout << "Called empty.memory_format" << endl;
  return native::empty_cpu(size, dtype, layout, device, pin_memory,
                           memory_format);
}

Tensor empty_strided(c10::DispatchKeySet ks, IntArrayRef size,
                     IntArrayRef stride,
                     c10::optional<ScalarType> dtype,
                     c10::optional<Layout> layout,
                     c10::optional<Device> device,
                     c10::optional<bool> pin_memory) {
  cout << "Called empty_strided" << endl;
  return
    native::empty_strided_cpu(size, stride, dtype, layout, device, pin_memory);
}

Tensor eq_Tensor(c10::DispatchKeySet ks, const Tensor &self,
                 const Tensor &other) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "eq_Tensor", self, other);
}

Tensor masked_select(c10::DispatchKeySet ks, const Tensor &self,
                     const Tensor &mask) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "masked_select", self, mask);
}

Tensor max(c10::DispatchKeySet ks, const Tensor &self) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "max", self);
}

Tensor min(c10::DispatchKeySet ks, const Tensor &self) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "min", self);
}

Tensor mul_Tensor(c10::DispatchKeySet ks, const Tensor &self,
                  const Tensor &other) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "mul_Tensor", self, other);
}

Tensor ne_Scalar(c10::DispatchKeySet ks, const Tensor &self,
                 const Scalar &other) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "ne_Scalar", self, other);
}

Tensor ne_Tensor(c10::DispatchKeySet ks, const Tensor &self,
                 const Tensor &other) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "ne_Tensor", self, other);
}

Tensor reshape(c10::DispatchKeySet ks, const Tensor &self, IntArrayRef shape) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "reshape", self, shape);
}

Tensor to_device(c10::DispatchKeySet ks,
                 const Tensor &self, Device device, ScalarType dtype,
                 bool non_blocking, bool copy,
                 c10::optional<MemoryFormat> memory_format) {
  ensure_materialized(self);
  return
    at::redispatch::to(
      ks & DispatchKeySet(DispatchKeySet::FULL_AFTER, DISPATCHKEY),
      self, device, dtype, non_blocking, copy, memory_format);
}

Tensor view(c10::DispatchKeySet ks, const Tensor &self, IntArrayRef size) {
  return at::detail::make_tensor<TorchyTensor>(self.dtype(), self.device(), ks,
                                               "view", self, size);
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
  m.impl("empty.memory_format", empty_memory_format); // FIXME: not called
  m.impl("empty_strided", empty_strided); // FIXME: not called
  m.impl("eq.Tensor", eq_Tensor);
  m.impl("masked_select", masked_select);
  m.impl("max", max);
  m.impl("min", min);
  m.impl("mul.Tensor", mul_Tensor);
  m.impl("ne.Scalar", ne_Scalar);
  m.impl("ne.Tensor", ne_Tensor);
  m.impl("reshape", reshape); // FIXME: RegisterMath
  m.impl("to.device", to_device); // FIXME: RegisterMath
  m.impl("view", view);
}

}
