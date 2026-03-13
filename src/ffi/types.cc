#if HAVE_FFI

#include "ffi.h"
#include "base_object-inl.h"
#include "v8.h"

using v8::ArrayBuffer;
using v8::ArrayBufferView;
using v8::BackingStore;
using v8::BigInt;
using v8::Context;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::NewStringType;
using v8::Number;
using v8::String;
using v8::Value;

namespace node {

namespace ffi {

bool ToFFIType(Environment* env, const std::string& type_str, ffi_type** ret) {
  if (ret == nullptr) {
    env->ThrowTypeError("ret must not be null");
    return false;
  }

  if (type_str == "void") {
    *ret = &ffi_type_void;
  } else if (type_str == "i8" || type_str == "int8") {
    *ret = &ffi_type_sint8;
  } else if (type_str == "u8" || type_str == "uint8" || type_str == "bool" || type_str == "char") {
    *ret = &ffi_type_uint8;
  } else if (type_str == "i16" || type_str == "int16") {
    *ret = &ffi_type_sint16;
  } else if (type_str == "u16" || type_str == "uint16") {
    *ret = &ffi_type_uint16;
  } else if (type_str == "i32" || type_str == "int32") {
    *ret = &ffi_type_sint32;
  } else if (type_str == "u32" || type_str == "uint32") {
    *ret = &ffi_type_uint32;
  } else if (type_str == "i64" || type_str == "int64") {
    *ret = &ffi_type_sint64;
  } else if (type_str == "u64" || type_str == "uint64") {
    *ret = &ffi_type_uint64;
  } else if (type_str == "f32" || type_str == "float") {
    *ret = &ffi_type_float;
  } else if (type_str == "f64" || type_str == "double") {
    *ret = &ffi_type_double;
  } else if (type_str == "buffer" ||
             type_str == "arraybuffer" ||
             type_str == "string" ||
             type_str == "str" ||
             type_str == "pointer" ||
             type_str == "ptr"||
             type_str == "function"
  ) {
    *ret = &ffi_type_pointer;
  } else {
    std::string msg = std::string("Unsupported FFI type: ") + type_str;
    env->ThrowTypeError(msg.c_str());
    return false;
  }

  return true;
}

uint8_t ToFFIArgument(Environment* env,
                unsigned int index,
                ffi_type* type,
                v8::Local<v8::Value> arg,
                void* ret) {
  v8::Local<v8::Context> context = env->context();

  if(type == &ffi_type_void) {
    return 1;
  } else if (type == &ffi_type_sint8) {
    if (!arg->IsInt32()) {
      env->ThrowTypeError(
          ("Argument " + std::to_string(index) + " must be an int8").c_str());
      return 0;
    }

    *static_cast<int8_t*>(ret) = static_cast<int8_t>(arg->Int32Value(context).FromJust());    
  } else if (type == &ffi_type_uint8) {
    if (!arg->IsUint32()) {
      env->ThrowTypeError(
          ("Argument " + std::to_string(index) + " must be a uint8").c_str());
      return 0;
    }
    
    *static_cast<uint8_t*>(ret) = static_cast<uint8_t>(arg->Uint32Value(context).FromJust());
  } else if (type == &ffi_type_sint16) {
    if (!arg->IsInt32()) {
      env->ThrowTypeError(
          ("Argument " + std::to_string(index) + " must be an int16").c_str());
      return 0;
    }
    
    *static_cast<int16_t*>(ret) = static_cast<int16_t>(arg->Int32Value(context).FromJust());
  } else if (type == &ffi_type_uint16) {
    if (!arg->IsUint32()) {
      env->ThrowTypeError(
          ("Argument " + std::to_string(index) + " must be a uint16").c_str());
      return 0;
    }
    
    *static_cast<uint16_t*>(ret) = static_cast<uint16_t>(arg->Uint32Value(context).FromJust());
  } else if (type == &ffi_type_sint32) {
    if (!arg->IsInt32()) {
      env->ThrowTypeError(
          ("Argument " + std::to_string(index) + " must be an int32").c_str());
      return 0;
    }
    
    *static_cast<int32_t*>(ret) = arg->Int32Value(context).FromJust();
  } else if (type == &ffi_type_uint32) {
    if (!arg->IsUint32()) {
      env->ThrowTypeError(
          ("Argument " + std::to_string(index) + " must be a uint32").c_str());
      return 0;
    }
    
    *static_cast<uint32_t*>(ret) = arg->Uint32Value(context).FromJust();
  } else if (type == &ffi_type_sint64) {
    if (!arg->IsBigInt() && !arg->IsNumber()) {
      env->ThrowTypeError(
          ("Argument " + std::to_string(index) + " must be an int64").c_str());
      return 0;
    }

    *static_cast<int64_t*>(ret) = arg->IsBigInt()
      ? arg.As<BigInt>()->Int64Value(nullptr)
      : static_cast<int64_t>(arg.As<Number>()->Value());
  } else if (type == &ffi_type_uint64) {
    if (!arg->IsBigInt() && !arg->IsNumber()) {
      env->ThrowTypeError(
          ("Argument " + std::to_string(index) + " must be a uint64").c_str());
      return 0;
    }
    
    *static_cast<uint64_t*>(ret) = arg->IsBigInt()
      ? arg.As<BigInt>()->Uint64Value(nullptr)
      : static_cast<uint64_t>(arg.As<Number>()->Value());
  } else if (type == &ffi_type_float) {
    if (!arg->IsNumber()) {
      env->ThrowTypeError(
          ("Argument " + std::to_string(index) + " must be a float").c_str());
      return 0;
    }
    
    *static_cast<float*>(ret) = static_cast<float>(arg->NumberValue(context).FromJust());
  } else if (type == &ffi_type_double) {
    if (!arg->IsNumber()) {
      env->ThrowTypeError(
          ("Argument " + std::to_string(index) + " must be a double").c_str());
      return 0;
    }
    
    *static_cast<double*>(ret) = arg->NumberValue(context).FromJust();
  } else if (type == &ffi_type_pointer) {
    if(arg->IsNull() || arg->IsUndefined()) {
      *static_cast<uint64_t*>(ret) = reinterpret_cast<uint64_t>(nullptr);
    } else if (arg->IsString()) {    
      // This will handled in Invoke so that we can free the copied string after the call
      return 2;
    } else if (arg->IsArrayBufferView()) {
      Local<ArrayBufferView> view = arg.As<ArrayBufferView>();
      std::shared_ptr<BackingStore> store = view->Buffer()->GetBackingStore();
      
      if (!store) {
        env->ThrowTypeError(("Invalid ArrayBufferView backing store for argument " + std::to_string(index)).c_str());
        return 0;
      }

      void* data = store->Data();
      size_t offset = view->ByteOffset();

      *static_cast<uint64_t*>(ret) = reinterpret_cast<uint64_t>(static_cast<char*>(data) + offset);
    } else if (arg->IsArrayBuffer()) {
      Local<ArrayBuffer> buffer = arg.As<ArrayBuffer>();
      std::shared_ptr<BackingStore> store = buffer->GetBackingStore();
      
      if (!store) {
        env->ThrowTypeError(("Invalid ArrayBuffer backing store for argument " + std::to_string(index)).c_str());
        return 0;
      }

      *static_cast<uint64_t*>(ret) = reinterpret_cast<uint64_t>(store->Data());
    } else if (arg->IsBigInt() || arg->IsNumber()) {
      *static_cast<uint64_t*>(ret) = arg->IsBigInt()
        ? arg.As<BigInt>()->Uint64Value(nullptr)
        : static_cast<uint64_t>(arg.As<Number>()->Value());
    } else {
      env->ThrowTypeError(
          ("Argument " + std::to_string(index) + " must be a buffer, an ArrayBuffer, a string, a number or a bigint").c_str());
      return 0;
    }
  } else {
    env->ThrowTypeError(
      ("Unsupported FFI type for argument " + std::to_string(index)).c_str());
    return 0;
  }

  return 1;
}

bool ToJSReturnValue(Environment* env, const FunctionCallbackInfo<Value>& args, ffi_type *type, ffi_arg result) {
  if (type ==&ffi_type_void) {
    args.GetReturnValue().SetUndefined();
  } else if (type ==&ffi_type_sint8) {
    args.GetReturnValue().Set(static_cast<int32_t>(static_cast<int8_t>(result)));
  } else if (type ==&ffi_type_uint8) {
    args.GetReturnValue().Set(static_cast<uint32_t>(static_cast<uint8_t>(result)));
  } else if (type ==&ffi_type_sint16) {
    args.GetReturnValue().Set(static_cast<int32_t>(static_cast<int16_t>(result)));
  } else if (type ==&ffi_type_uint16) {
    args.GetReturnValue().Set(static_cast<uint32_t>(static_cast<uint16_t>(result)));
  } else if (type ==&ffi_type_sint32) {
    args.GetReturnValue().Set(static_cast<int32_t>(result));
  } else if (type ==&ffi_type_uint32) {
    args.GetReturnValue().Set(static_cast<uint32_t>(result));
  } else if (type ==&ffi_type_sint64) {
    args.GetReturnValue().Set(BigInt::NewFromUnsigned(env->isolate(), result));
  } else if (type ==&ffi_type_uint64) {
    args.GetReturnValue().Set(BigInt::NewFromUnsigned(env->isolate(), result));
  } else if (type ==&ffi_type_float) {
    args.GetReturnValue().Set(*reinterpret_cast<float*>(&result));
  } else if (type ==&ffi_type_double) {
    args.GetReturnValue().Set(*reinterpret_cast<double*>(&result));
  } else if (type ==&ffi_type_pointer) {
    args.GetReturnValue().Set(BigInt::NewFromUnsigned(env->isolate(), result));     
  }

  return true;
}

Local<Value> ToJSArgument(Isolate* isolate, ffi_type* type, void* data) {
  Local<Value> ret;

  if (type == &ffi_type_sint8) {
    ret = Integer::New(isolate, *static_cast<int8_t*>(data));
  } else if (type == &ffi_type_uint8) {
    ret = Integer::NewFromUnsigned(isolate, *static_cast<uint8_t*>(data));
  } else if (type == &ffi_type_sint16) {
    ret = Integer::New(isolate, *static_cast<int16_t*>(data));
  } else if (type == &ffi_type_uint16) {
    ret = Integer::NewFromUnsigned(isolate, *static_cast<uint16_t*>(data));
  } else if (type == &ffi_type_sint32) {
    ret = Integer::New(isolate, *static_cast<int32_t*>(data));
  } else if (type == &ffi_type_uint32) {
    ret = Integer::NewFromUnsigned(isolate, *static_cast<uint32_t*>(data));
  } else if (type == &ffi_type_sint64) {
    ret = BigInt::New(isolate, *static_cast<int64_t*>(data));
  } else if (type == &ffi_type_uint64) {
    ret = BigInt::NewFromUnsigned(isolate, *static_cast<int64_t*>(data));
  } else if (type == &ffi_type_float) {
    ret = Integer::New(isolate, *static_cast<float*>(data));
  } else if (type == &ffi_type_double) {
    ret = Integer::New(isolate, *static_cast<double*>(data));
  } else if (type == &ffi_type_pointer) { 
    // All others are treated as pointer (and thus bigint), the user will use other helpers to convert    
    ret = BigInt::NewFromUnsigned(isolate, reinterpret_cast<uint64_t>(*static_cast<void**>(data)));
  } else { 
    // For anything else, return undefined to avoid problems
    ret = Undefined(isolate);
  }

  return ret;
}

bool ToFFIReturnValue(Environment *env, Local<Value> result, ffi_type *type, void* ret) {  
  Local<Context> context = env->context();

  if (type == &ffi_type_sint8) {
    MaybeLocal<v8::Int32> val = result->ToInt32(context);
    
    if (!val.IsEmpty()) {
      *static_cast<int8_t*>(ret) = static_cast<int8_t>(val.ToLocalChecked()->Value());
    }
  } else if (type == &ffi_type_uint8) {
    MaybeLocal<v8::Uint32> val = result->ToUint32(context);
    
    if (!val.IsEmpty()) {
      *static_cast<uint8_t*>(ret) = static_cast<uint8_t>(val.ToLocalChecked()->Value());
    }
  } else if (type == &ffi_type_sint16) {
    MaybeLocal<v8::Int32> val = result->ToInt32(context);
    
    if (!val.IsEmpty()) {
      *static_cast<int16_t*>(ret) = static_cast<int16_t>(val.ToLocalChecked()->Value());
    }
  } else if (type == &ffi_type_uint16) {
    MaybeLocal<v8::Uint32> val = result->ToUint32(context);
    
    if (!val.IsEmpty()) {
      *static_cast<uint16_t*>(ret) = static_cast<uint16_t>(val.ToLocalChecked()->Value());
    }
  } else if (type == &ffi_type_sint32) {
    MaybeLocal<v8::Int32> val = result->ToInt32(context);

    if (!val.IsEmpty()) {
      *static_cast<int32_t*>(ret) = val.ToLocalChecked()->Value();
    }
  } else if (type == &ffi_type_uint32) {
    MaybeLocal<v8::Uint32> val = result->ToUint32(context);

    if (!val.IsEmpty()) {
      *static_cast<uint32_t*>(ret) = val.ToLocalChecked()->Value();
    }    
  } else if (type == &ffi_type_sint64) {
    MaybeLocal<BigInt> val = result->ToBigInt(context);
    
    if (!val.IsEmpty()) {
      *static_cast<int64_t*>(ret) = val.ToLocalChecked()->Int64Value();
    }
  } else if (type == &ffi_type_uint64) {
    MaybeLocal<BigInt> val = result->ToBigInt(context);
    
    if (!val.IsEmpty()) {
      *static_cast<uint64_t*>(ret) = val.ToLocalChecked()->Uint64Value();
    }
  } else if (type == &ffi_type_float) {
    MaybeLocal<Number> val = result->ToNumber(context);
    
    if (!val.IsEmpty()) {
      *static_cast<float*>(ret) = static_cast<float>(val.ToLocalChecked()->Value());
    }
  } else if (type == &ffi_type_double) {
    MaybeLocal<Number> val = result->ToNumber(context);
    
    if (!val.IsEmpty()) {
      *static_cast<double*>(ret) = val.ToLocalChecked()->Value();
    }
  } else if (type == &ffi_type_pointer) {
    // Note that strings, buffer or ArrayBuffer are ignored
    if(!(result->IsBigInt() || result->IsNumber())) {
      *static_cast<uint64_t*>(ret) = reinterpret_cast<uint64_t>(nullptr);
    } else if (result->IsBigInt() || result->IsNumber()) {
      *static_cast<uint64_t*>(ret) = result->IsBigInt()
        ? result.As<BigInt>()->Uint64Value(nullptr)
        : static_cast<uint64_t>(result.As<Number>()->Value());
    }
  } 

  return true;
}

}  // namespace ffi

}  // namespace node
  
#endif  // HAVE_FFI