// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "bindings/python/pyiree/rt/host_types.h"

#include "absl/container/inlined_vector.h"
#include "absl/memory/memory.h"
#include "bindings/python/pyiree/common/status_utils.h"
#include "bindings/python/pyiree/rt/hal.h"
#include "iree/base/signature_mangle.h"
#include "pybind11/numpy.h"

namespace iree {
namespace python {

const std::array<const char*, static_cast<unsigned>(
                                  AbiConstants::ScalarType::kMaxScalarType) +
                                  1>
    kScalarTypePyFormat = {
        "f",      // kIeeeFloat32 = 0,
        nullptr,  // kIeeeFloat16 = 1,
        "d",      // kIeeeFloat64 = 2,
        nullptr,  // kGoogleBfloat16 = 3,
        "b",      // kSint8 = 4,
        "h",      // kSint16 = 5,
        "i",      // kSint32 = 6,
        "q",      // kSint64 = 7,
        "c",      // kUint8 = 8,
        "H",      // kUint16 = 9,
        "I",      // kUint32 = 10,
        "Q",      // kUint64 = 11,
};
static_assert(kScalarTypePyFormat.size() ==
                  AbiConstants::kScalarTypeSize.size(),
              "Mismatch kScalarTypePyFormat");

namespace {

class PyMappedMemory {
 public:
  struct Description {
    size_t element_size;
    const char* format;
    absl::InlinedVector<py::ssize_t, 4> dims;
    absl::InlinedVector<py::ssize_t, 4> strides;

    static Description ForNdarray(AbiConstants::ScalarType scalar_type,
                                  absl::Span<const int> dims) {
      unsigned scalar_type_i = static_cast<unsigned>(scalar_type);
      if (scalar_type_i >
          static_cast<unsigned>(AbiConstants::ScalarType::kMaxScalarType)) {
        throw RaiseValueError("Illegal ScalarType");
      }

      Description d;
      d.element_size = AbiConstants::kScalarTypeSize[scalar_type_i];
      d.format = kScalarTypePyFormat[scalar_type_i];
      if (!d.format) {
        throw RaisePyError(PyExc_NotImplementedError,
                           "Unimplemented ScalarType");
      }
      if (!dims.empty()) {
        d.dims.resize(dims.size());
        d.strides.resize(dims.size());

        for (size_t i = 0, e = dims.size(); i < e; ++i) {
          d.dims[i] = dims[i];
        }
        d.strides[dims.size() - 1] = d.element_size;
        for (int i = dims.size() - 2; i >= 0; --i) {
          d.strides[i] = d.strides[i + 1] * dims[i + 1];
        }
      }
      return d;
    }
  };

  PyMappedMemory(Description desc, iree_hal_mapped_memory_t mapped_memory,
                 HalBuffer buffer)
      : desc_(std::move(desc)),
        mapped_memory_(mapped_memory),
        buf_(std::move(buffer)) {}
  ~PyMappedMemory() {
    if (buf_) {
      CheckApiStatus(iree_hal_buffer_unmap(buf_.raw_ptr(), &mapped_memory_),
                     "Error unmapping memory");
    }
  }
  PyMappedMemory(PyMappedMemory&& other)
      : mapped_memory_(other.mapped_memory_), buf_(std::move(other.buf_)) {}

  const Description& desc() const { return desc_; }

  static std::unique_ptr<PyMappedMemory> Read(Description desc,
                                              HalBuffer buffer) {
    iree_device_size_t byte_length =
        iree_hal_buffer_byte_length(buffer.raw_ptr());
    iree_hal_mapped_memory_t mapped_memory;
    CheckApiStatus(iree_hal_buffer_map(
                       buffer.raw_ptr(), IREE_HAL_MEMORY_ACCESS_READ,
                       0 /* element_offset */, byte_length, &mapped_memory),
                   "Could not map memory");
    return absl::make_unique<PyMappedMemory>(std::move(desc), mapped_memory,
                                             std::move(buffer));
  }

  py::buffer_info ToBufferInfo() {
    // TODO(laurenzo): py::buffer_info is a heavy-weight way to get the
    // buffer. See about implementing the lower level buffer protocol.
    // Unfortunately, this part of the pybind C++ API is all defined in terms
    // of std::vector, making it less efficient than necessary.
    return py::buffer_info(mapped_memory_.contents.data, desc_.element_size,
                           desc_.format, desc_.dims.size(), desc_.dims,
                           desc_.strides);
  }

 private:
  Description desc_;
  iree_hal_mapped_memory_t mapped_memory_;
  HalBuffer buf_;
};

class NumpyHostTypeFactory : public HostTypeFactory {
  py::object CreateImmediateNdarray(AbiConstants::ScalarType element_type,
                                    absl::Span<const int> dims,
                                    HalBuffer buffer) override {
    auto mapped_memory = PyMappedMemory::Read(
        PyMappedMemory::Description::ForNdarray(element_type, dims),
        std::move(buffer));
    // Since an immediate ndarray was requested, we can just return a native
    // ndarray directly (versus a proxy that needs to lazily map on access).
    auto buffer_info = mapped_memory->ToBufferInfo();
    auto py_mapped_memory = py::cast(mapped_memory.release(),
                                     py::return_value_policy::take_ownership);
    return py::array(py::dtype(buffer_info), buffer_info.shape,
                     buffer_info.strides, buffer_info.ptr,
                     std::move(py_mapped_memory) /* base */);
  }
};

}  // namespace

//------------------------------------------------------------------------------
// HostTypeFactory
//------------------------------------------------------------------------------

std::shared_ptr<HostTypeFactory> HostTypeFactory::GetNumpyFactory() {
  static auto global_instance = std::make_shared<NumpyHostTypeFactory>();
  return global_instance;
}

py::object HostTypeFactory::CreateImmediateNdarray(
    AbiConstants::ScalarType element_type, absl::Span<const int> dims,
    HalBuffer buffer) {
  throw RaisePyError(PyExc_NotImplementedError,
                     "CreateImmediateNdarray not implemented");
}

void SetupHostTypesBindings(pybind11::module m) {
  py::class_<HostTypeFactory, std::shared_ptr<HostTypeFactory>>(
      m, "HostTypeFactory")
      .def(py::init<>())
      .def_static("get_numpy", &HostTypeFactory::GetNumpyFactory);
  py::class_<PyMappedMemory, std::unique_ptr<PyMappedMemory>>(
      m, "PyMappedMemory", py::buffer_protocol())
      .def_buffer(&PyMappedMemory::ToBufferInfo);
}

}  // namespace python
}  // namespace iree
