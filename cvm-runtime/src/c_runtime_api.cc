/*!
 *  Copyright (c) 2016 by Contributors
 * \file c_runtime_api.cc
 * \brief Device specific implementations
 */
#include <utils/thread_local.h>
#include <cvm/runtime/c_runtime_api.h>
#include <cvm/runtime/c_backend_api.h>
#include <cvm/runtime/packed_func.h>
#include <cvm/runtime/module.h>
#include <cvm/runtime/registry.h>
#include <cvm/runtime/device_api.h>
#include <array>
#include <algorithm>
#include <string>
#include <cstdlib>
#include <cctype>
#include "runtime_base.h"

namespace cvm {
namespace runtime {

class DeviceAPIManager {
 public:
  static const int kMaxDeviceAPI = 32;
  // Get API
  static DeviceAPI* Get(const CVMContext& ctx) {
    return Get(ctx.device_type);
  }
  static DeviceAPI* Get(int dev_type, bool allow_missing = false) {
    return Global()->GetAPI(dev_type, allow_missing);
  }

 private:
  std::array<DeviceAPI*, kMaxDeviceAPI> api_;
  DeviceAPI* rpc_api_{nullptr};
  std::mutex mutex_;
  // constructor
  DeviceAPIManager() {
    std::fill(api_.begin(), api_.end(), nullptr);
  }
  // Global static variable.
  static DeviceAPIManager* Global() {
    static DeviceAPIManager inst;
    return &inst;
  }
  // Get or initialize API.
  DeviceAPI* GetAPI(int type, bool allow_missing) {
    if (type < kRPCSessMask) {
      if (api_[type] != nullptr) return api_[type];
      std::lock_guard<std::mutex> lock(mutex_);
      if (api_[type] != nullptr) return api_[type];
      api_[type] = GetAPI(DeviceName(type), allow_missing);
      return api_[type];
    } else {
      if (rpc_api_ != nullptr) return rpc_api_;
      std::lock_guard<std::mutex> lock(mutex_);
      if (rpc_api_ != nullptr) return rpc_api_;
      rpc_api_ = GetAPI("rpc", allow_missing);
      return rpc_api_;
    }
  }
  DeviceAPI* GetAPI(const std::string name, bool allow_missing) {
    std::string factory = "device_api." + name;
    auto* f = Registry::Get(factory);
    if (f == nullptr) {
      CHECK(allow_missing)
          << "Device API " << name << " is not enabled.";
      return nullptr;
    }
    void* ptr = (*f)();
    return static_cast<DeviceAPI*>(ptr);
  }
};

DeviceAPI* DeviceAPI::Get(CVMContext ctx, bool allow_missing) {
  return DeviceAPIManager::Get(
      static_cast<int>(ctx.device_type), allow_missing);
}

void* DeviceAPI::AllocWorkspace(CVMContext ctx,
                                size_t size,
                                CVMType type_hint) {
  return AllocDataSpace(ctx, size, kTempAllocaAlignment, type_hint);
}

void DeviceAPI::FreeWorkspace(CVMContext ctx, void* ptr) {
  FreeDataSpace(ctx, ptr);
}

CVMStreamHandle DeviceAPI::CreateStream(CVMContext ctx) {
  LOG(FATAL) << "Device does not support stream api.";
  return 0;
}

void DeviceAPI::FreeStream(CVMContext ctx, CVMStreamHandle stream) {
  LOG(FATAL) << "Device does not support stream api.";
}

void DeviceAPI::SyncStreamFromTo(CVMContext ctx,
                                 CVMStreamHandle event_src,
                                 CVMStreamHandle event_dst) {
  LOG(FATAL) << "Device does not support stream api.";
}

#ifndef _LIBCPP_SGX_NO_IOSTREAMS
//--------------------------------------------------------
// Error handling mechanism
// -------------------------------------------------------
// Standard error message format, {} means optional
//--------------------------------------------------------
// {error_type:} {message0}
// {message1}
// {message2}
// {Stack trace:}    // stack traces follow by this line
//   {trace 0}       // two spaces in the begining.
//   {trace 1}
//   {trace 2}
//--------------------------------------------------------
/*!
 * \brief Normalize error message
 *
 *  Parse them header generated by by LOG(FATAL) and CHECK
 *  and reformat the message into the standard format.
 *
 *  This function will also merge all the stack traces into
 *  one trace and trim them.
 *
 * \param err_msg The error message.
 * \return normalized message.
 */
std::string NormalizeError(std::string err_msg) {
  // ------------------------------------------------------------------------
  // log with header, {} indicates optional
  //-------------------------------------------------------------------------
  // [timestamp] file_name:line_number: {check_msg:} {error_type:} {message0}
  // {message1}
  // Stack trace:
  //   {stack trace 0}
  //   {stack trace 1}
  //-------------------------------------------------------------------------
  // Normalzied version
  //-------------------------------------------------------------------------
  // error_type: check_msg message0
  // {message1}
  // Stack trace:
  //   File file_name, line lineno
  //   {stack trace 0}
  //   {stack trace 1}
  //-------------------------------------------------------------------------
  int line_number = 0;
  std::istringstream is(err_msg);
  std::string line, file_name, error_type, check_msg;

  // Parse log header and set the fields,
  // Return true if it the log is in correct format,
  // return false if something is wrong.
  auto parse_log_header = [&]() {
    // skip timestamp
    if (is.peek() != '[') {
      getline(is, line);
      return true;
    }
    if (!(is >> line)) return false;
    // get filename
    while (is.peek() == ' ') is.get();
    if (!getline(is, file_name, ':')) return false;
    // get line number
    if (!(is >> line_number)) return false;
    // get rest of the message.
    while (is.peek() == ' ' || is.peek() == ':') is.get();
    if (!getline(is, line)) return false;
    // detect check message, rewrite to remote extra :
    if (line.compare(0, 13, "Check failed:") == 0) {
      size_t end_pos = line.find(':', 13);
      if (end_pos == std::string::npos) return false;
      check_msg = line.substr(0, end_pos + 1) + ' ';
      line = line.substr(end_pos + 1);
    }
    return true;
  };
  // if not in correct format, do not do any rewrite.
  if (!parse_log_header()) return err_msg;
  // Parse error type.
  {
    size_t start_pos = 0, end_pos;
    for (; start_pos < line.length() && line[start_pos] == ' '; ++start_pos) {}
    for (end_pos = start_pos; end_pos < line.length(); ++end_pos) {
      char ch = line[end_pos];
      if (ch == ':') {
        error_type = line.substr(start_pos, end_pos - start_pos);
        break;
      }
      // [A-Z0-9a-z_.]
      if (!std::isalpha(ch) && !std::isdigit(ch) && ch != '_' && ch != '.') break;
    }
    if (error_type.length() != 0) {
      // if we successfully detected error_type: trim the following space.
      for (start_pos = end_pos + 1;
           start_pos < line.length() && line[start_pos] == ' '; ++start_pos) {}
      line = line.substr(start_pos);
    } else {
      // did not detect error_type, use default value.
      line = line.substr(start_pos);
      error_type = "CVMError";
    }
  }
  // Seperate out stack trace.
  std::ostringstream os;
  os << error_type << ": " << check_msg << line << '\n';

  bool trace_mode = true;
  std::vector<std::string> stack_trace;
  while (getline(is, line)) {
    if (trace_mode) {
      if (line.compare(0, 2, "  ") == 0) {
        stack_trace.push_back(line);
      } else {
        trace_mode = false;
        // remove EOL trailing stacktrace.
        if (line.length() == 0) continue;
      }
    }
    if (!trace_mode) {
      if (line.compare(0, 11, "Stack trace") == 0) {
        trace_mode = true;
      } else {
        os << line << '\n';
      }
    }
  }
  if (stack_trace.size() != 0 || file_name.length() != 0) {
    os << "Stack trace:\n";
    if (file_name.length() != 0) {
      os << "  File \"" << file_name << "\", line " << line_number << "\n";
    }
    // Print out stack traces, optionally trim the c++ traces
    // about the frontends (as they will be provided by the frontends).
    bool ffi_boundary = false;
    for (const auto& line : stack_trace) {
      // Heuristic to detect python ffi.
      if (line.find("libffi.so") != std::string::npos ||
          line.find("core.cpython") != std::string::npos) {
        ffi_boundary = true;
      }
      // If the backtrace is not c++ backtrace with the prefix "  [bt]",
      // then we can stop trimming.
      if (ffi_boundary && line.compare(0, 6, "  [bt]") != 0) {
        ffi_boundary = false;
      }
      if (!ffi_boundary) {
        os << line << '\n';
      }
      // The line after CVMFuncCall cound be in FFI.
      if (line.find("(CVMFuncCall") != std::string::npos) {
        ffi_boundary = true;
      }
    }
  }
  return os.str();
}

#else
std::string NormalizeError(std::string err_msg) {
  return err_msg;
}
#endif
}  // namespace runtime
}  // namespace cvm

using namespace cvm::runtime;

struct CVMRuntimeEntry {
  std::string ret_str;
  std::string last_error;
  CVMByteArray ret_bytes;
};

typedef utils::ThreadLocalStore<CVMRuntimeEntry> CVMAPIRuntimeStore;

const char *CVMGetLastError() {
  return CVMAPIRuntimeStore::Get()->last_error.c_str();
}

int CVMAPIHandleException(const std::runtime_error &e) {
  CVMAPISetLastError(NormalizeError(e.what()).c_str());
  return -2;
}

int CVMAPIHandleLogicException(const std::logic_error &e) {
  CVMAPISetLastError(NormalizeError(e.what()).c_str());
  return -1;
}

void CVMAPISetLastError(const char* msg) {
  CVMAPIRuntimeStore::Get()->last_error = msg;
}

int CVMModLoadFromFile(const char* file_name,
                       const char* format,
                       CVMModuleHandle* out) {
  API_BEGIN();
  Module m = Module::LoadFromFile(file_name, format);
  *out = new Module(m);
  API_END();
}

int CVMModImport(CVMModuleHandle mod,
                 CVMModuleHandle dep) {
  API_BEGIN();
  static_cast<Module*>(mod)->Import(
      *static_cast<Module*>(dep));
  API_END();
}

int CVMModGetFunction(CVMModuleHandle mod,
                      const char* func_name,
                      int query_imports,
                      CVMFunctionHandle *func) {
  API_BEGIN();
  PackedFunc pf = static_cast<Module*>(mod)->GetFunction(
      func_name, query_imports != 0);
  if (pf != nullptr) {
    *func = new PackedFunc(pf);
  } else {
    *func = nullptr;
  }
  API_END();
}

int CVMModFree(CVMModuleHandle mod) {
  API_BEGIN();
  delete static_cast<Module*>(mod);
  API_END();
}

int CVMBackendGetFuncFromEnv(void* mod_node,
                             const char* func_name,
                             CVMFunctionHandle *func) {
  API_BEGIN();
  *func = (CVMFunctionHandle)(
      static_cast<ModuleNode*>(mod_node)->GetFuncFromEnv(func_name));
  API_END();
}

void* CVMBackendAllocWorkspace(int device_type,
                               int device_id,
                               uint64_t size,
                               int dtype_code_hint,
                               int dtype_bits_hint) {
  CVMContext ctx;
  ctx.device_type = static_cast<DLDeviceType>(device_type);
  ctx.device_id = device_id;

  CVMType type_hint;
  type_hint.code = static_cast<decltype(type_hint.code)>(dtype_code_hint);
  type_hint.bits = static_cast<decltype(type_hint.bits)>(dtype_bits_hint);
  type_hint.lanes = 1;

  return DeviceAPIManager::Get(ctx)->AllocWorkspace(ctx,
                                                    static_cast<size_t>(size),
                                                    type_hint);
}

int CVMBackendFreeWorkspace(int device_type,
                            int device_id,
                            void* ptr) {
  CVMContext ctx;
  ctx.device_type = static_cast<DLDeviceType>(device_type);
  ctx.device_id = device_id;
  DeviceAPIManager::Get(ctx)->FreeWorkspace(ctx, ptr);
  return 0;
}

int CVMBackendRunOnce(void** handle,
                      int (*f)(void*),
                      void* cdata,
                      int nbytes) {
  if (*handle == nullptr) {
    *handle = reinterpret_cast<void*>(1);
    return (*f)(cdata);
  }
  return 0;
}

int CVMFuncFree(CVMFunctionHandle func) {
  API_BEGIN();
  delete static_cast<PackedFunc*>(func);
  API_END();
}

int CVMFuncCall(CVMFunctionHandle func,
                CVMValue* args,
                int* arg_type_codes,
                int num_args,
                CVMValue* ret_val,
                int* ret_type_code) {
  API_BEGIN();
  CVMRetValue rv;
  (*static_cast<const PackedFunc*>(func)).CallPacked(
      CVMArgs(args, arg_type_codes, num_args), &rv);
  // handle return string.
  if (rv.type_code() == kStr ||
     rv.type_code() == kCVMType ||
      rv.type_code() == kBytes) {
    CVMRuntimeEntry* e = CVMAPIRuntimeStore::Get();
    if (rv.type_code() != kCVMType) {
      e->ret_str = *rv.ptr<std::string>();
    } else {
      e->ret_str = rv.operator std::string();
    }
    if (rv.type_code() == kBytes) {
      e->ret_bytes.data = e->ret_str.c_str();
      e->ret_bytes.size = e->ret_str.length();
      *ret_type_code = kBytes;
      ret_val->v_handle = &(e->ret_bytes);
    } else {
      *ret_type_code = kStr;
      ret_val->v_str = e->ret_str.c_str();
    }
  } else {
    rv.MoveToCHost(ret_val, ret_type_code);
  }
  API_END();
}

int CVMCFuncSetReturn(CVMRetValueHandle ret,
                      CVMValue* value,
                      int* type_code,
                      int num_ret) {
  API_BEGIN();
  CHECK_EQ(num_ret, 1);
  CVMRetValue* rv = static_cast<CVMRetValue*>(ret);
  *rv = CVMArgValue(value[0], type_code[0]);
  API_END();
}

int CVMFuncCreateFromCFunc(CVMPackedCFunc func,
                           void* resource_handle,
                           CVMPackedCFuncFinalizer fin,
                           CVMFunctionHandle *out) {
  API_BEGIN();
  if (fin == nullptr) {
    *out = new PackedFunc(
        [func, resource_handle](CVMArgs args, CVMRetValue* rv) {
          int ret = func((CVMValue*)args.values, (int*)args.type_codes, // NOLINT(*)
                         args.num_args, rv, resource_handle);
          if (ret != 0) {
            throw utils::Error(CVMGetLastError() + ::utils::StackTrace());
          }
        });
  } else {
    // wrap it in a shared_ptr, with fin as deleter.
    // so fin will be called when the lambda went out of scope.
    std::shared_ptr<void> rpack(resource_handle, fin);
    *out = new PackedFunc(
        [func, rpack](CVMArgs args, CVMRetValue* rv) {
          int ret = func((CVMValue*)args.values, (int*)args.type_codes, // NOLINT(*)
                         args.num_args, rv, rpack.get());
          if (ret != 0) {
            throw utils::Error(CVMGetLastError() + ::utils::StackTrace());
          }
      });
  }
  API_END();
}

int CVMStreamCreate(int device_type, int device_id, CVMStreamHandle* out) {
  API_BEGIN();
  CVMContext ctx;
  ctx.device_type = static_cast<DLDeviceType>(device_type);
  ctx.device_id = device_id;
  *out = DeviceAPIManager::Get(ctx)->CreateStream(ctx);
  API_END();
}

int CVMStreamFree(int device_type, int device_id, CVMStreamHandle stream) {
  API_BEGIN();
  CVMContext ctx;
  ctx.device_type = static_cast<DLDeviceType>(device_type);
  ctx.device_id = device_id;
  DeviceAPIManager::Get(ctx)->FreeStream(ctx, stream);
  API_END();
}

int CVMSetStream(int device_type, int device_id, CVMStreamHandle stream) {
  API_BEGIN();
  CVMContext ctx;
  ctx.device_type = static_cast<DLDeviceType>(device_type);
  ctx.device_id = device_id;
  DeviceAPIManager::Get(ctx)->SetStream(ctx, stream);
  API_END();
}

int CVMSynchronize(int device_type, int device_id, CVMStreamHandle stream) {
  API_BEGIN();
  CVMContext ctx;
  ctx.device_type = static_cast<DLDeviceType>(device_type);
  ctx.device_id = device_id;
  DeviceAPIManager::Get(ctx)->StreamSync(ctx, stream);
  API_END();
}

int CVMStreamStreamSynchronize(int device_type,
                               int device_id,
                               CVMStreamHandle src,
                               CVMStreamHandle dst) {
  API_BEGIN();
  CVMContext ctx;
  ctx.device_type = static_cast<DLDeviceType>(device_type);
  ctx.device_id = device_id;
  DeviceAPIManager::Get(ctx)->SyncStreamFromTo(ctx, src, dst);
  API_END();
}

int CVMCbArgToReturn(CVMValue* value, int code) {
  API_BEGIN();
  cvm::runtime::CVMRetValue rv;
  rv = cvm::runtime::CVMArgValue(*value, code);
  int tcode;
  rv.MoveToCHost(value, &tcode);
  CHECK_EQ(tcode, code);
  API_END();
}

// set device api
CVM_REGISTER_GLOBAL(cvm::runtime::symbol::cvm_set_device)
.set_body([](CVMArgs args, CVMRetValue *ret) {
    CVMContext ctx;
    ctx.device_type = static_cast<DLDeviceType>(args[0].operator int());
    ctx.device_id = args[1];
    DeviceAPIManager::Get(ctx)->SetDevice(ctx);
  });

// set device api
CVM_REGISTER_GLOBAL("_GetDeviceAttr")
.set_body([](CVMArgs args, CVMRetValue *ret) {
    CVMContext ctx;
    ctx.device_type = static_cast<DLDeviceType>(args[0].operator int());
    ctx.device_id = args[1];

    DeviceAttrKind kind = static_cast<DeviceAttrKind>(args[2].operator int());
    if (kind == kExist) {
      DeviceAPI* api = DeviceAPIManager::Get(ctx.device_type, true);
      if (api != nullptr) {
        api->GetAttr(ctx, kind, ret);
      } else {
        *ret = 0;
      }
    } else {
      DeviceAPIManager::Get(ctx)->GetAttr(ctx, kind, ret);
    }
  });
