#if HAVE_FFI

#include "node_ffi.h"
#include "base_object-inl.h"
#include "env-inl.h"
#include "node_errors.h"

namespace node {

using v8::Array;
using v8::ArrayBuffer;
using v8::BackingStore;
using v8::BigInt;
using v8::Context;
using v8::DontDelete;
using v8::External;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Global;
using v8::HandleScope;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::LocalVector;
using v8::MaybeLocal;
using v8::Name;
using v8::NewStringType;
using v8::Number;
using v8::Object;
using v8::ObjectTemplate;
using v8::PropertyAttribute;
using v8::PropertyCallbackInfo;
using v8::ReadOnly;
using v8::TryCatch;
using v8::String;
using v8::Value;

namespace ffi {

  DynamicLibrary::DynamicLibrary(Environment* env, Local<Object> object)
    : BaseObject(env, object), handle_(nullptr), symbols_() {
  MakeWeak();
}

DynamicLibrary::~DynamicLibrary() {
  this->Close();
}

void DynamicLibrary::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackFieldWithSize("path", path_.capacity() + 1, "std::string");

  size_t symbols_size = 0;
  for (const auto& [name, ptr] : symbols_) {
    symbols_size += name.capacity() + 1;
    symbols_size += sizeof(ptr);
    symbols_size += sizeof(decltype(symbols_)::value_type);
  }

  tracker->TrackFieldWithSize("symbols", symbols_size,
                              "std::unordered_map<std::string, void*>");
}

void DynamicLibrary::Close() {   
  dlclose(handle_);
  handle_ = nullptr;
  symbols_.clear();
  functions_.clear();
  callbacks_.clear();
}

bool DynamicLibrary::FindOrCreateSymbol(Environment *env, const std::string& name, void **ret) {  
  if (handle_ == nullptr) {
    env->ThrowError("Library is closed");
    return false;
  }

  auto existing = symbols_.find(name);
  void* ptr;

  if (existing == symbols_.end()) {
    dlerror();
    
    ptr = dlsym(handle_, name.c_str());
    const char* err = dlerror();
    
    if (err) {
      std::string msg = std::string("dlsym failed: ") + err;
      env->ThrowError(msg.c_str());  
      return false;
    }

    symbols_[name] = ptr;
  } else {
    ptr = existing->second;
  }

  *ret = ptr;
  return true;
}

bool DynamicLibrary::FindOrCreateFunction(Environment *env, const std::string& name, Local<Array> signature, FFIFunction **ret) {
  FFIFunction* fn;
  auto existing = functions_.find(name);
  
  if (existing == functions_.end()) {
    void *ptr;
    
    if (!FindOrCreateSymbol(env, name, &ptr)) {
      return false;
    }

    fn = new FFIFunction{.owner = this, .ptr = ptr, .return_type = &ffi_type_void};

    // Compile the function arguments  
    if(signature->Length() > 0) {
      Local<Value> return_type;
      
      if (!signature->Get(env->context(), 0).ToLocal(&return_type)) {
        delete fn;
        return false;
      }

      if (!return_type->IsString()) {
        env->ThrowTypeError("Return value type must be a string");
        return false;
      }

      Isolate *isolate = env->isolate();
      node::Utf8Value return_type_str(isolate, return_type);
      if (!ToFFIType(env, *return_type_str, &fn->return_type)) {
        delete fn;
        return false;
      }

      unsigned int argn = signature->Length();
      for(unsigned int i = 1; i < argn; i++) {
        Local<Value> arg;
      
        if (!signature->Get(env->context(), i).ToLocal(&arg)) {
          delete fn;
          return false;
        }

        if (!arg->IsString()) {
          std::string msg = "Argument " + std::to_string(i - 1) + " type must be a string";
          env->ThrowTypeError(msg.c_str());
          delete fn;
          return false;
        }

        node::Utf8Value arg_str(isolate, arg);
        ffi_type* arg_type;
        if (!ToFFIType(env, *arg_str, &arg_type)) {
          delete fn;
          return false;
        }

        fn->args.push_back(arg_type);
      }
    }

    ffi_status status = ffi_prep_cif(&fn->cif, FFI_DEFAULT_ABI, fn->args.size(), fn->return_type, fn->args.data());
    if (status != FFI_OK) {
      const char* msg = "ffi_prep_cif failed";
      switch (status) {
        case FFI_BAD_TYPEDEF:
          msg = "ffi_prep_cif failed: bad typedef";
          break;
        case FFI_BAD_ABI:
          msg = "ffi_prep_cif failed: bad ABI";
          break;
        default:
          msg = "ffi_prep_cif failed: unknown error";
          break;
      }

      env->ThrowError(msg);
      delete fn;
      return false;
    }

    functions_.emplace(name, fn);
  } else {
    fn = existing->second.get();
  }

  *ret = fn;
  return true;
}

Local<Function> DynamicLibrary::CreateFunction(Environment* env, const std::string& name, FFIFunction* fn) {
  Isolate *isolate = env->isolate();

  Local<External> data = External::New(isolate, fn);
  Local<Function> ret = Function::New(env->context(), DynamicLibrary::InvokeFunction, data).ToLocalChecked();
  ret->SetName(String::NewFromUtf8(isolate, name.c_str(), NewStringType::kNormal).ToLocalChecked());
  ret->Set(env->context(),
        FIXED_ONE_BYTE_STRING(env->isolate(), "pointer"),
        BigInt::NewFromUnsigned(isolate, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(fn->ptr))))
    .Check();

  return ret;
}

void DynamicLibrary::New(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate *isolate = env->isolate();
  Local<Context> context = env->context();

  THROW_IF_INSUFFICIENT_PERMISSIONS(env, permission::PermissionScope::kFFI, "");
  
  if (args.Length() < 1 || !args[0]->IsString()) {
    env->ThrowTypeError("Library path must be a string");
    return;
  }

  DynamicLibrary* lib = new DynamicLibrary(env, args.This());
  node::Utf8Value filename(env->isolate(), args[0]);
  int flags = RTLD_LAZY;
  lib->path_ = std::string(*filename);

  // Open the library
  dlerror();
  lib->handle_ = dlopen(*filename, flags);
  const char* err = dlerror();

  if (lib->handle_ == nullptr) {
    std::string msg = std::string("dlopen failed: ") + err;
    env->ThrowError(msg.c_str());    
  }

  if(args.Length() > 1) {
    if(!args[1]->IsObject()) {
      env->ThrowTypeError("Functions signatures must be an object");
      return;
    }
  
    Local<Object> signatures = args[1].As<Object>();
    Local<Array> keys;
    if (!signatures->GetOwnPropertyNames(context).ToLocal(&keys)) {
      return;
    }

    for (uint32_t i = 0; i < keys->Length(); i++) {
      Local<Value> key;
      Local<Value> signature;

      if (!keys->Get(context, i).ToLocal(&key)) {
        return;
      }

      node::Utf8Value name(isolate, key);

      if (!signatures->Get(env->context(), key).ToLocal(&signature)) {
        return;
      }

      if(!signature->IsArray()) {
        std::string msg = std::string("Signature of function ") + *name + " must be an array";
        lib->Close();
        env->ThrowTypeError(msg.c_str());
      }

      FFIFunction* fn;
      
      Local<Array> signature_array = signature.As<Array>();
      if (!lib->FindOrCreateFunction(env, *name, signature_array, &fn)) {
        return;
      }
    }    
  }
}

void DynamicLibrary::Close(const FunctionCallbackInfo<Value>& args) {
  DynamicLibrary* lib = Unwrap<DynamicLibrary>(args.This());
  lib->Close();
}

void DynamicLibrary::InvokeFunction(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  FFIFunction* fn =
      static_cast<FFIFunction*>(args.Data().As<External>()->Value());
  
  if(fn->owner->handle_ == nullptr || fn->ptr == nullptr) {
    env->ThrowError("Library is closed");
    return;
  }
  
  // Convert arguments
  unsigned int expected_args = fn->args.size();
  unsigned int provided_args = args.Length();

  if (provided_args < expected_args) {
    std::string msg = "Too few arguments: expected " + std::to_string(expected_args) + ", got " + std::to_string(provided_args);
    env->ThrowError(msg.c_str());
    return;
  }

  std::vector<uint64_t> values(expected_args, 0);
  std::vector<void*> ffi_args(expected_args, nullptr);
  std::vector<std::string> strings;
  strings.reserve(expected_args);  

  for(unsigned int i = 0; i < expected_args; i++) {
    uint8_t res = ToFFIArgument(env, i, fn->args[i], args[i], &values[i]);

    if (!res) {
      return;
    }

    // The argument is a string, we need to copy
    if(res == 2) {
      String::Utf8Value str(env->isolate(), args[i]);

      if (*str == nullptr) {
        env->ThrowTypeError(
          ("Argument " + std::to_string(i) + " must be a string").c_str());
        return;
      }

      strings.push_back(*str);
      values[i] = reinterpret_cast<uint64_t>(strings.back().c_str());
      ffi_args[i] = &values[i];
    } else {
      ffi_args[i] = &values[i];
    }
  }

  ffi_arg result;
  ffi_call(&fn->cif, FFI_FN(fn->ptr), &result, ffi_args.data());

  // Return result back to Javascript
  ToJSReturnValue(env, args, fn->return_type, result);
}

void DynamicLibrary::InvokeCallback(ffi_cif* cif, void* ret, void** args, void* user_data) {
  FFICallback* cb = static_cast<FFICallback*>(user_data);
  Environment* env = cb->env;
  Isolate* isolate = env->isolate();

  HandleScope handle_scope(isolate);
  Local<Context> context = env->context();

  if(cb->owner->handle_ == nullptr || cb->ptr == nullptr) {
    env->ThrowError("Library is closed");
    return;
  }

  size_t expected_args = cb->args.size();
  LocalVector<Value> callback_args(isolate, expected_args);
  
  for (size_t i = 0; i < expected_args; i++) {
    if(args[i] == nullptr) {
      callback_args[i] = Null(isolate);
      continue;
    } else {
      callback_args[i] = ToJSArgument(isolate, cb->args[i], args[i]);
    }
  }
  
  TryCatch try_catch(isolate);
  Local<Function> callback = Local<Function>::New(isolate, cb->fn);
  MaybeLocal<Value> result = callback->Call(context, Undefined(isolate), expected_args, callback_args.data());

  // Handle exceptions by logging (can't propagate across FFI boundary)
  if (try_catch.HasCaught()) {
    try_catch.ReThrow();
    return;
  }

  ToFFIReturnValue(env, result.ToLocalChecked(), cb->return_type, ret);
}

void DynamicLibrary::GetPath(const FunctionCallbackInfo<Value>& args) {
  DynamicLibrary* lib = Unwrap<DynamicLibrary>(args.This());

  args.GetReturnValue().Set(
      String::NewFromUtf8(args.GetIsolate(),
                              lib->path_.c_str(),
                              NewStringType::kNormal)
          .ToLocalChecked());
}

void DynamicLibrary::GetFunction(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate *isolate = env->isolate();

  if (args.Length() < 1 || !args[0]->IsString()) {
    env->ThrowTypeError("Symbol name must be a string");
    return;
  }
  
  DynamicLibrary* lib = Unwrap<DynamicLibrary>(args.This());
  node::Utf8Value name(isolate, args[0]);
  FFIFunction* fn;

  Local<Array> signature = Array::New(env->isolate());
  for (int32_t i = 1; i < args.Length(); i++) {
    signature->Set(env->context(), i - 1, args[i]).Check();
  }

  if (!lib->FindOrCreateFunction(env, *name, signature, &fn)) {
    return;
  }

  Local<Function> ret = lib->CreateFunction(env, *name, fn);
  args.GetReturnValue().Set(ret);
}

void DynamicLibrary::GetFunctions(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate *isolate = env->isolate();
  Local<Context> context = env->context();
  DynamicLibrary *lib = Unwrap<DynamicLibrary>(args.This());

  if (lib->handle_ == nullptr) {
    env->ThrowError("Library is closed");
    return;
  }

  Local<Object> functions = Object::New(isolate);
  
  if(args.Length() > 0) {
    if(!args[0]->IsObject()) {
      env->ThrowTypeError("Functions signatures must be an object");
      return;
    }
  
    Local<Object> signatures = args[0].As<Object>();
    Local<Array> keys;
    if (!signatures->GetOwnPropertyNames(context).ToLocal(&keys)) {
      return;
    }

    for (uint32_t i = 0; i < keys->Length(); i++) {
      Local<Value> key;
      Local<Value> signature;

      if (!keys->Get(context, i).ToLocal(&key)) {
        return;
      }

      node::Utf8Value name(isolate, key);

      if (!signatures->Get(env->context(), key).ToLocal(&signature)) {
        return;
      }

      if(!signature->IsArray()) {
        std::string msg = std::string("Signature of function ") + *name + " must be an array";
        env->ThrowTypeError(msg.c_str());
      }

      FFIFunction* fn;
      
      Local<Array> signature_array = signature.As<Array>();
      if (!lib->FindOrCreateFunction(env, *name, signature_array, &fn)) {
        return;
      }

      Local<Function> ret = lib->CreateFunction(env, *name, fn);
      functions->Set(context,
        String::NewFromUtf8(isolate, *name, NewStringType::kNormal).ToLocalChecked(),
        ret)
        .Check();      
    }
  } else {
    for (const auto& entry : lib->functions_) {
      Local<Function> fn = lib->CreateFunction(env, entry.first, entry.second.get());

      functions->Set(context,
        String::NewFromUtf8(isolate, entry.first.c_str(), NewStringType::kNormal).ToLocalChecked(),
        fn)
        .Check();
    }
  }

  args.GetReturnValue().Set(functions);
}

void DynamicLibrary::GetSymbol(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate *isolate = env->isolate();

  if (args.Length() < 1 || !args[0]->IsString()) {
    env->ThrowTypeError("Symbol name must be a string");
    return;
  }

  DynamicLibrary* lib = Unwrap<DynamicLibrary>(args.This());
  node::Utf8Value name(isolate, args[0]);
  void *ptr;

  if (!lib->FindOrCreateSymbol(env, *name, &ptr)) {
    return;
  }

  args.GetReturnValue().Set(
    BigInt::NewFromUnsigned(isolate, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ptr))));
}

void DynamicLibrary::GetSymbols(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate *isolate = env->isolate();
  Local<Context> context = env->context();
  DynamicLibrary *lib = Unwrap<DynamicLibrary>(args.This());

  if (lib->handle_ == nullptr) {
    env->ThrowError("Library is closed");
    return;
  }

  Local<Object> symbols = Object::New(isolate);
  for (const auto& entry : lib->symbols_) {
    symbols->Set(context,
      String::NewFromUtf8(isolate, entry.first.c_str(), NewStringType::kNormal).ToLocalChecked(),
      BigInt::NewFromUnsigned(isolate, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(entry.second))))
      .Check(); 
  }

  args.GetReturnValue().Set(symbols);
}

void DynamicLibrary::RegisterCallback(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate *isolate = env->isolate();

  Local<Array> signature;
  Local<Function> fn;

  if(args.Length() < 1 || (!args[0]->IsFunction() && !args[0]->IsArray())) {
    env->ThrowTypeError("First argument must be a function or an array");
    return;
  }

  if(args[0]->IsFunction()) {
    fn = args[0].As<Function>();
    signature = Array::New(env->isolate());
  } else {
    signature = args[0].As<Array>();

    if(args.Length() < 2 || !args[1]->IsFunction()) {
      env->ThrowTypeError("Second argument must be a function");
      return;
    }

    fn = args[1].As<Function>();
  }

  DynamicLibrary* lib = Unwrap<DynamicLibrary>(args.This());
  if (lib->handle_ == nullptr) {
    env->ThrowError("Library is closed");
    return;
  }

  FFICallback* callback = new FFICallback{.owner = lib, .env = env, .fn = Global<Function>(isolate, fn), .return_type = &ffi_type_void};
  
  if(signature->Length() > 0) {
    Local<Value> return_type;
    
    if (!signature->Get(env->context(), 0).ToLocal(&return_type)) {
      delete callback;
      return;
    }

    if (!return_type->IsString()) {
      env->ThrowTypeError("Return value type must be a string");
      delete callback;
      return;
    }

    Isolate *isolate = env->isolate();
    node::Utf8Value return_type_str(isolate, return_type);
    if (!ToFFIType(env, *return_type_str, &callback->return_type)) {
      delete callback;
      return;
    }

    unsigned int argn = signature->Length();
    for(unsigned int i = 1; i < argn; i++) {
      Local<Value> arg;
    
      if (!signature->Get(env->context(), i).ToLocal(&arg)) {
        delete callback;
        return;
      }

      if (!arg->IsString()) {
        std::string msg = "Argument " + std::to_string(i - 1) + " type must be a string";
        env->ThrowTypeError(msg.c_str());
        delete callback;
        return;
      }

      node::Utf8Value arg_str(isolate, arg);
      ffi_type* arg_type;
      if (!ToFFIType(env, *arg_str, &arg_type)) {
        delete callback;
        return;
      }

      callback->args.push_back(arg_type);
    }
  }

  callback->closure = static_cast<ffi_closure*>(ffi_closure_alloc(sizeof(ffi_closure), &callback->ptr));

  if(callback->closure == nullptr) {
    env->ThrowError("ffi_closure_alloc failed");
    delete callback;
    return;
  }

  ffi_status status;
  status = ffi_prep_cif(&callback->cif, FFI_DEFAULT_ABI, callback->args.size(), callback->return_type, callback->args.data());
  if (status != FFI_OK) {
    const char* msg = "ffi_prep_cif failed";
    switch (status) {
      case FFI_BAD_TYPEDEF:
        msg = "ffi_prep_cif failed: bad typedef";
        break;
      case FFI_BAD_ABI:
        msg = "ffi_prep_cif failed: bad ABI";
        break;
      default:
        msg = "ffi_prep_cif failed: unknown error";
        break;
    }

    env->ThrowError(msg);
    delete callback;
    return;
  }

  status = ffi_prep_closure_loc(callback->closure, &callback->cif, DynamicLibrary::InvokeCallback, callback, callback->ptr);
  if (status != FFI_OK) {
    const char* msg = "ffi_prep_closure_loc failed";
    switch (status) {
      case FFI_BAD_TYPEDEF:
        msg = "ffi_prep_closure_loc failed: bad typedef";
        break;
      case FFI_BAD_ABI:
        msg = "ffi_prep_closure_loc failed: bad ABI";
        break;
      default:
        msg = "ffi_prep_closure_loc failed: unknown error";
        break;
    }

    env->ThrowError(msg);
    delete callback;
    return;
  }

  lib->callbacks_.emplace_back(callback);
  args.GetReturnValue().Set(
    BigInt::NewFromUnsigned(isolate, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(callback->ptr))));  
}

void AsString(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate *isolate = env->isolate();

  if (args.Length() < 1 || !args[0]->IsBigInt()) {
    env->ThrowTypeError("The first argument must be a bigint");
    return;
  }

  uint64_t ptr = args[0].As<BigInt>()->Uint64Value();
  const char* str = reinterpret_cast<const char*>(ptr);
  args.GetReturnValue().Set(
      String::NewFromUtf8(isolate, str, NewStringType::kNormal).ToLocalChecked());
}

void AsBuffer(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate *isolate = env->isolate();

  if (args.Length() < 1 || !args[0]->IsBigInt()) {
    env->ThrowTypeError("The first argument must be a bigint");
    return;
  }

  if (args.Length() < 2 || !args[1]->IsNumber()) {
    env->ThrowTypeError("The second argument must be a number");
    return;
  }

  uint64_t ptr = args[0].As<BigInt>()->Uint64Value();
  size_t len = args[1].As<Number>()->IntegerValue(env->context()).FromJust();

  Local<Object> buf;
  if (args.Length() < 3 || args[2]->BooleanValue(isolate)) {
    buf = Buffer::Copy(isolate, reinterpret_cast<char*>(ptr), len).ToLocalChecked();
  } else {
    buf = Buffer::New(
      isolate, 
      reinterpret_cast<char*>(ptr), len,
      [](char* data, void* hint) {},
      nullptr
    ).ToLocalChecked();
  }

  args.GetReturnValue().Set(buf);
}

void AsArrayBuffer(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();

  if (args.Length() < 1 || !args[0]->IsBigInt()) {
    env->ThrowTypeError("The first argument must be a bigint");
    return;
  }

  if (args.Length() < 2 || !args[1]->IsNumber()) {
    env->ThrowTypeError("The second argument must be a number");
    return;
  }

  uint64_t ptr = args[0].As<BigInt>()->Uint64Value();
  size_t len = args[1].As<Number>()->IntegerValue(env->context()).FromJust();

  Local<ArrayBuffer> ab;

  if (args.Length() < 3 || args[2]->BooleanValue(isolate)) {
    std::unique_ptr<BackingStore> store = ArrayBuffer::NewBackingStore(isolate, len);
    memcpy(store->Data(), reinterpret_cast<void*>(ptr), len);
    ab = ArrayBuffer::New(isolate, std::move(store));
  } else {
    std::unique_ptr<BackingStore> store = ArrayBuffer::NewBackingStore(
      reinterpret_cast<void*>(ptr), len, 
      [](void* data, size_t length, void* deleter_data) {},
      nullptr);

    ab = ArrayBuffer::New(isolate, std::move(store));
  }

  args.GetReturnValue().Set(ab);
}

Local<FunctionTemplate> DynamicLibrary::GetConstructorTemplate(
    Environment* env) {
  Local<FunctionTemplate> tmpl =
      env->ffi_dynamic_library_constructor_template();

  if (tmpl.IsEmpty()) {
    Isolate* isolate = env->isolate();
    enum PropertyAttribute attributes =
      static_cast<PropertyAttribute>(ReadOnly | DontDelete);

    tmpl = NewFunctionTemplate(isolate, DynamicLibrary::New);
    tmpl->InstanceTemplate()->SetInternalFieldCount(
        DynamicLibrary::kInternalFieldCount);

    tmpl->InstanceTemplate()->SetAccessorProperty(
        FIXED_ONE_BYTE_STRING(isolate, "path"),
        FunctionTemplate::New(env->isolate(), DynamicLibrary::GetPath),
        Local<FunctionTemplate>(),
        attributes);

    tmpl->InstanceTemplate()->SetAccessorProperty(
        FIXED_ONE_BYTE_STRING(isolate, "symbols"),
        FunctionTemplate::New(env->isolate(), DynamicLibrary::GetSymbols),
        Local<FunctionTemplate>(),      
        attributes);

    tmpl->InstanceTemplate()->SetAccessorProperty(
        FIXED_ONE_BYTE_STRING(isolate, "functions"),
        FunctionTemplate::New(env->isolate(), DynamicLibrary::GetFunctions),
        Local<FunctionTemplate>(),      
        attributes);        

    SetProtoMethod(isolate, tmpl, "close", DynamicLibrary::Close);
    SetProtoMethod(isolate, tmpl, "getFunction", DynamicLibrary::GetFunction);
    SetProtoMethod(isolate, tmpl, "getFunctions", DynamicLibrary::GetFunctions);
    SetProtoMethod(isolate, tmpl, "getSymbol", DynamicLibrary::GetSymbol);
    SetProtoMethod(isolate, tmpl, "getSymbols", DynamicLibrary::GetSymbols);
    SetProtoMethod(isolate, tmpl, "registerCallback", DynamicLibrary::RegisterCallback);

    env->set_ffi_dynamic_library_constructor_template(tmpl);
  }

  return tmpl;
}

// Module initialization.
static void Initialize(Local<Object> target,
                       Local<Value> unused,
                       Local<Context> context,
                       void* priv) {
  Environment* env = Environment::GetCurrent(context);
  
  // Create the DynamicLibrary template
  Local<FunctionTemplate> dl_tmpl = DynamicLibrary::GetConstructorTemplate(env);
  SetConstructorFunction(context, target, "DynamicLibrary", dl_tmpl);
  SetMethod(context, target, "toString", ToString);
  SetMethod(context, target, "toBuffer", ToBuffer);  
  SetMethod(context, target, "toArrayBuffer", ToArrayBuffer);  
}

}  // namespace ffi
}  // namespace node

NODE_BINDING_CONTEXT_AWARE_INTERNAL(ffi, node::ffi::Initialize)

#endif  // HAVE_FFI

// TODO: Struct as input and output