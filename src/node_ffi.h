#pragma once

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "base_object.h"
#include "ffi.h"
#include "node_mem.h"
#include "util.h"

#include <dlfcn.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

using v8::Array;
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Isolate;
using v8::Local;
using v8::Name;
using v8::PropertyCallbackInfo;
using v8::Object;
using v8::Value;

namespace node::ffi {

bool ToFFIType(Environment* env, const std::string& type_str, ffi_type** ret);

uint8_t ToFFIArgument(Environment* env,
                unsigned int index,
                ffi_type* type,
                v8::Local<v8::Value> arg,
                void* ret);

                Local<Value> ToJSArgument(Isolate* isolate, ffi_type* type, void* data);
                
bool ToJSReturnValue(Environment* env, const FunctionCallbackInfo<Value>& args, ffi_type* type, ffi_arg result);

Local<Value> ToJSArgument(Isolate* isolate, ffi_type* type, void* data);

bool ToFFIReturnValue(Environment *env, Local<Value> result, ffi_type *type, void* ret);

class DynamicLibrary;

struct FFIFunction {
  DynamicLibrary* owner;

  void* ptr;
  ffi_cif cif;
  std::vector<ffi_type*> args;
  ffi_type* return_type;
};

// Verify the environment is the same when invoking. See if you can support thread safety
struct FFICallback {
  DynamicLibrary* owner;
  Environment* env;
  v8::Global<v8::Function> fn;
  ffi_closure* closure;

  void* ptr;
  ffi_cif cif;
  std::vector<ffi_type*> args;
  ffi_type* return_type;

  ~FFICallback() {
    fn.Reset();
    
    if (closure != nullptr) {
      ffi_closure_free(closure);
      closure = nullptr;
    }
  }
};

class DynamicLibrary : public BaseObject {
  public:
    DynamicLibrary(Environment* env, Local<Object> object);
    ~DynamicLibrary() override;

    void MemoryInfo(MemoryTracker* tracker) const override;

    static Local<FunctionTemplate> GetConstructorTemplate(
      Environment* env);
    static void New(const FunctionCallbackInfo<Value>& args);
    static void Close(const FunctionCallbackInfo<Value>& args);
    static void InvokeFunction(const FunctionCallbackInfo<Value>& args);
    static void InvokeCallback(ffi_cif* cif, void* ret, void** args, void* user_data);

    static void GetPath(const FunctionCallbackInfo<Value>& args);
    static void GetFunction(const FunctionCallbackInfo<Value>& args);
    static void GetFunctions(const FunctionCallbackInfo<Value>& args);
    static void GetSymbol(const FunctionCallbackInfo<Value>& args);
    static void GetSymbols(const FunctionCallbackInfo<Value>& args);
    static void RegisterCallback(const FunctionCallbackInfo<Value>& args);

    SET_MEMORY_INFO_NAME(DynamicLibrary)
    SET_SELF_SIZE(DynamicLibrary)

  private:
    void Close();
    bool FindOrCreateSymbol(Environment *env, const std::string& name, void **ptr);
    bool FindOrCreateFunction(Environment *env, const std::string& name, Local<Array> signature, FFIFunction **ret);  
    Local<Function> CreateFunction(Environment* env, const std::string& name, FFIFunction* fn);

    void* handle_;
    std::string path_;
    std::unordered_map<std::string, void*> symbols_;
    std::unordered_map<std::string, std::unique_ptr<FFIFunction>> functions_;
    std::vector<std::unique_ptr<FFICallback>> callbacks_;
};

// TODO: Find a suitable name so that you can also have exports in the JS world for callbacks
static void ToString(const FunctionCallbackInfo<Value>& args);
static void ToBuffer(const FunctionCallbackInfo<Value>& args);
static void ToArrayBuffer(const FunctionCallbackInfo<Value>& args);

}  // namespace node::ffi

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS
