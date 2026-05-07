# FFI Fast API internals

This document describes the internal implementation of the `node:ffi` Fast API
path. It is intended for contributors working on the FFI implementation, not for
users of the public API.

The Fast API path is an optimization layer for FFI calls whose signatures can be
represented by V8 Fast API metadata and a generated native trampoline. It does
not replace the generic libffi path. Instead, Node.js creates the fastest
available callable for each signature and keeps the generic path available for
unsupported call shapes, deoptimized V8 calls, and validation behavior that must
match the public FFI API.

## Goals

The Fast API implementation is designed around these goals:

* Keep hot scalar FFI calls out of the generic `v8::FunctionCallbackInfo` path.
* Avoid per-call allocation for common numeric and pointer-like signatures.
* Preserve the public `node:ffi` behavior and error shape.
* Keep string lifetime management in JavaScript, where temporary buffers can be
  owned explicitly.
* Keep SharedBuffer and Fast API routing separate, with `lib/ffi.js` composing
  the two wrapper layers.
* Use generated per-signature native code instead of runtime loops inside the
  trampoline.

## Main files

The implementation is split across these files:

* `src/ffi/fast.h` declares the Fast API type model, metadata containers, and
  platform trampoline hooks.
* `src/ffi/fast.cc` maps public FFI type names to Fast API types, creates V8
  `CFunctionInfo` metadata, and exposes buffer conversion helpers used by
  generated trampolines.
* `src/ffi/platforms/*.cc` contains the platform trampoline generators. These
  files follow the contract exposed by `node_ffi_create_fast_trampoline()`.
* `src/node_ffi.cc` decides whether a function gets a Fast API callable,
  SharedBuffer callable, or generic callable, and attaches hidden metadata used
  by JavaScript wrappers.
* `src/node_ffi.h` stores `FastFFIMetadata` objects in `FFIFunctionInfo` so V8
  metadata and generated executable code stay alive with the JavaScript function.
* `lib/ffi.js` is the public module wrapper. It patches `DynamicLibrary` methods
  and composes SharedBuffer and Fast API wrappers.
* `lib/internal/ffi/fast-api.js` performs JavaScript-side pointer conversions
  for Fast API calls.
* `lib/internal/ffi-shared-buffer.js` contains only SharedBuffer-specific
  argument packing and result unpacking.

## Native metadata creation

`DynamicLibrary::CreateFunction()` creates one `FFIFunctionInfo` per generated
JavaScript function. That object owns the parsed `FFIFunction`, the persistent
function handle, the owning library reference, and any optimized invocation
metadata.

The creation flow is:

1. Call `CreateFastFFIMetadata(*fn)`.
2. If Fast API metadata is created, bind the JavaScript function with a V8
   `FunctionTemplate` that contains both the conventional callback and the Fast
   API `v8::CFunction`.
3. If Fast API metadata cannot be created, try the SharedBuffer path for
   eligible signatures.
4. If neither optimized path applies, bind the generic `InvokeFunction` path.

Returning `nullptr` from `CreateFastFFIMetadata()` is not a public signature
error. It means only that the current Fast API implementation cannot optimize
that signature. The caller then falls back to another invocation strategy.

## FastFFIType

The internal `FastFFIType` enum is intentionally smaller than the public FFI type
surface. It models the ABI categories that the generated trampoline knows how to
marshal directly:

* `kVoid`
* `kBool`
* signed and unsigned 8-bit, 16-bit, 32-bit, and 64-bit integers
* `kFloat32`
* `kFloat64`
* `kPointer`
* `kBuffer`

Public aliases are normalized in `FastScalarTypeFromName()` and
`FastArgTypeFromName()`.

`pointer`, `ptr`, `function`, `string`, `str`, `buffer`, and `arraybuffer` all
represent pointer-sized native values at the target ABI boundary. They differ in
how JavaScript values are accepted and converted before the target function is
called.

## V8 CFunction metadata

`CreateFastFFIMetadata()` creates a `FastFFIMetadata` object. That object owns:

* `FastFFITrampoline trampoline`, the executable bridge called by V8.
* `std::vector<v8::CTypeInfo> arg_info`, the V8 type list.
* `std::unique_ptr<v8::CFunctionInfo> c_function_info`, the V8 signature object.
* `v8::CFunction c_function`, the handle attached to the `FunctionTemplate`.

The metadata object must own `arg_info` and `c_function_info` because V8 keeps
pointers into that storage. Destroying or moving that storage while the function
is alive would leave V8 with dangling pointers.

The first V8 argument is always the JavaScript receiver. For that reason,
`CreateFastFFIMetadata()` prepends a `v8::CTypeInfo::Type::kV8Value` entry before
the public FFI arguments.

If any argument or return value needs a 64-bit integer or pointer, the V8
`CFunctionInfo` is configured with BigInt representation. This avoids precision
loss for pointer-sized values and 64-bit integers.

## Generated trampolines

The generated trampoline bridges V8's Fast API calling convention to the native
ABI expected by the library symbol. Its responsibilities are:

* Move incoming V8 Fast API arguments into the registers expected by the target
  native function.
* Narrow 8-bit and 16-bit integer arguments after V8 has widened them.
* Convert `kBuffer` arguments from V8 values to backing-store pointers.
* Call the target symbol.
* Normalize narrow integer return values before V8 observes them.
* Return control to V8 using the platform ABI.

The trampoline is generated per signature. It does not loop over arguments at
runtime using metadata tables. The code generator may loop while emitting
instructions, but the emitted code is a straight-line bridge specialized for the
signature.

Executable memory is allocated writable, populated with instructions, flushed
from the instruction cache as required by the platform, and then marked
executable. `FastFFIMetadata` releases that memory through
`node_ffi_free_fast_trampoline()` when the JavaScript function is collected.

## Buffer and ArrayBuffer arguments

Fast API buffer arguments are represented internally as `FastFFIType::kBuffer`.
In V8 metadata they are described as `v8::Local<v8::Value>`, not as `uint64_t`.
This lets the generated trampoline receive the original JavaScript object and
call `node_ffi_fast_buffer_data()` immediately before the native target call.

`node_ffi_fast_buffer_data()` accepts:

* `Buffer` and other ArrayBuffer views
* `ArrayBuffer`
* `SharedArrayBuffer`

For views, the pointer is the backing store plus `byteOffset`. For ArrayBuffer
and SharedArrayBuffer, the pointer is the start of the backing store.

Invalid values cause the helper to throw through `FastApiCallbackOptions` and
return a sentinel value. The generated trampoline checks for that sentinel and
returns zero without calling the native target. This prevents native code from
observing an invalid pointer after JavaScript validation has failed.

## Pointer-like arguments

Pointer-like public types include `pointer`, `ptr`, and `function`. They are
represented as raw unsigned pointer values in the scalar Fast API signature.

JavaScript wrappers in `lib/internal/ffi/fast-api.js` convert accepted non-BigInt
values before entering the scalar Fast API function:

* `null` and `undefined` become `0n`.
* strings become temporary NUL-terminated UTF-8 buffers.
* Buffer and ArrayBuffer-backed values become backing-store pointers.

Keeping these conversions in JavaScript preserves public FFI semantics and keeps
temporary object lifetime explicit.

## String arguments

String conversion intentionally stays in JavaScript. A string accepted by a
`string`, `str`, or pointer-like parameter is encoded into a temporary Buffer
with a trailing NUL byte. The pointer to that Buffer is passed to the scalar Fast
API function.

Each owning `DynamicLibrary` keeps a hidden array of string conversion entries.
Each argument index gets one reusable entry. If the same string is passed again
at the same argument index, the wrapper reuses the existing encoded buffer and
pointer. This is a single-entry reuse strategy, not an unbounded cache.

This design avoids native lifetime ambiguity. The generated trampoline never
allocates temporary string storage and never has to guess how long a pointer to a
converted string must stay alive.

## Secondary buffer invoke for pointer signatures

A single pointer-like function can be called efficiently in two different ways:

* with a scalar pointer value, such as `BigInt` or `null`
* with a memory object, such as `Buffer` or `ArrayBuffer`

These two cases require different V8 Fast API representations. The scalar case
uses a `uint64_t`/BigInt-shaped argument. The memory-object case uses a
`v8::Local<v8::Value>` argument so the generated trampoline can extract the
backing-store pointer.

For monomorphic single-argument pointer-like signatures, native code may attach
a secondary function under the hidden `kFastBufferInvoke` symbol. This secondary
function uses a cloned signature where the pointer-like argument is described as
`buffer` for Fast API metadata purposes, while still calling the same native
target symbol.

The JavaScript wrapper dispatches to this secondary function only when the
runtime argument is Buffer or ArrayBuffer-backed memory. Other pointer inputs use
the primary scalar Fast API function.

This keeps both important call shapes fast. Replacing the primary scalar Fast
API function with the buffer-shaped function would simplify the machinery, but
it would force BigInt, null, and string-converted pointer calls onto a slower
fallback path. Keeping both entrypoints preserves performance for both pointer
representations.

## Hidden symbols

Native code attaches internal metadata to raw generated functions using
per-isolate Symbols. These symbols are exported through `internalBinding('ffi')`
and are not part of the public API.

SharedBuffer metadata uses:

* `kSbSharedBuffer`
* `kSbInvokeSlow`
* `kSbParams`
* `kSbResult`

Fast API metadata uses:

* `kFastParams`
* `kFastBufferInvoke`

The two groups are intentionally separate. SharedBuffer wrappers should not need
Fast API metadata, and Fast API wrappers should not need SharedBuffer metadata.
`lib/ffi.js` is the composition layer that reads both groups and decides which
wrapper to apply.

## JavaScript wrapper routing

`lib/ffi.js` patches the native `DynamicLibrary` methods that expose generated
functions:

* `DynamicLibrary.prototype.getFunction`
* `DynamicLibrary.prototype.getFunctions`
* the `DynamicLibrary.prototype.functions` accessor

All three routes call `wrapFFIFunction()` before returning functions to user
code.

`wrapFFIFunction()` applies wrappers in this order:

1. Initialize Fast API buffer metadata for raw Fast API functions.
2. Try `wrapWithSharedBuffer()`.
3. If SharedBuffer did not apply, try `wrapWithRawPointerConversions()`.
4. Return the original raw function unchanged if no wrapper is needed.

This ordering keeps SharedBuffer-specific behavior inside
`lib/internal/ffi-shared-buffer.js`, Fast API pointer conversion behavior inside
`lib/internal/ffi/fast-api.js`, and public wrapper orchestration inside
`lib/ffi.js`.

## SharedBuffer fallback

SharedBuffer remains a separate optimized path for signatures that are not using
Fast API but can still avoid per-argument native conversion overhead. The
SharedBuffer wrapper writes arguments into a fixed 8-byte slot layout, calls a
raw native function with no JavaScript arguments, and reads the return value from
slot zero.

Pointer signatures using SharedBuffer have a slow invoker attached under
`kSbInvokeSlow`. The wrapper uses the SharedBuffer path for BigInt and nullish
pointer values, and falls back to the slow invoker for values that require the
generic native conversion path.

Fast API and SharedBuffer are independent. A function uses either the Fast API
path, the SharedBuffer path, or the generic path as its primary native callable.
`lib/ffi.js` only composes the JavaScript wrappers needed to preserve public
argument behavior.

## Generic fallback behavior

Every Fast API function is also bound with the conventional native callback.
V8 can call that callback when a JavaScript call site is not eligible for the
Fast API path. Node.js also uses the generic path directly when metadata creation
rejects a signature.

The generic path remains responsible for complete validation and public error
compatibility. Fast wrappers should match those errors for conversions they
perform in JavaScript.

## Argument count and signature limits

The Fast API path intentionally supports only a bounded number of function
arguments. This keeps V8 metadata, wrapper specialization, and generated
trampolines simple and predictable. Signatures outside that bound fall back to
SharedBuffer or the generic path.

This is an optimization boundary, not a public FFI signature boundary. User code
can still call supported public FFI signatures through the fallback paths.

## Wrapper metadata preservation

JavaScript wrappers preserve selected public function metadata:

* `name`
* `length`
* `pointer`

The `pointer` property mirrors the raw function's pointer descriptor so user
code that reads or reassigns it continues to work through wrappers. Internal
Symbol-keyed metadata is not forwarded to wrappers.