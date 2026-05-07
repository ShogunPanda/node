'use strict';

const {
  ArrayPrototypeIncludes,
  ObjectDefineProperty,
  ReflectApply,
  StringPrototypeIncludes,
  Symbol,
  TypeError,
} = primordials;

const {
  Buffer,
} = require('buffer');

const {
  isAnyArrayBuffer,
  isArrayBufferView,
} = require('internal/util/types');

const {
  getRawPointer,
  kFastBufferInvoke,
  kFastParams,
  kSbSharedBuffer,
} = internalBinding('ffi');

// kFastBuffer is JS-local metadata. Native code only exposes whether a raw
// function has Fast API parameter metadata; this marker records that this
// wrapper has already recognized a native buffer-shaped fast path.
const kFastBuffer = Symbol('kFastBuffer');

// String conversion buffers are stored on the owning DynamicLibrary so their
// backing stores stay alive for the duration of the FFI call and can be reused
// across repeated calls to the same signature slot.
const kStringConversionBuffer = Symbol('kStringConversionBuffer');

function throwFFIArgError(msg) {
  // eslint-disable-next-line no-restricted-syntax
  const err = new TypeError(msg);
  err.code = 'ERR_INVALID_ARG_VALUE';
  throw err;
}

function throwFFIArgCountError(expected, actual) {
  throwFFIArgError(
    `Invalid argument count: expected ${expected}, got ${actual}`);
}

function needsRawPointerConversion(type, rawFn) {
  // Native Fast API buffer parameters already receive the original V8 value and
  // convert it inside the generated trampoline. Do not pre-convert those to a
  // BigInt pointer in JS or V8 would miss the buffer-shaped fast signature.
  if (rawFn !== undefined && rawFn[kFastBuffer] === true &&
      (type === 'buffer' || type === 'arraybuffer')) {
    return false;
  }
  return type === 'buffer' || type === 'arraybuffer';
}

function needsPointerLikeConversion(type) {
  // These public aliases are ABI-identical: each ultimately becomes an unsigned
  // pointer-sized integer for the scalar Fast API call.
  return type === 'pointer' || type === 'ptr' || type === 'function';
}

function needsStringPointerConversion(type) {
  // Plain string signatures and pointer-like signatures both accept JS strings.
  // The temporary NUL-terminated UTF-8 buffer must be owned by JS, not by the
  // generated native trampoline.
  return type === 'string' || type === 'str' || needsPointerLikeConversion(type);
}

function needsNullPointerConversion(type) {
  // Nullish values are accepted for all pointer-shaped parameters and are
  // normalized to the native null pointer before entering the Fast API call.
  return needsPointerLikeConversion(type) || type === 'string' || type === 'str' ||
         needsRawPointerConversion(type);
}

function needsPointerConversion(type, rawFn) {
  // This is the broad wrapper predicate. It intentionally covers string,
  // nullish, Buffer/ArrayBuffer, and raw object-to-pointer conversion cases.
  if (rawFn !== undefined && rawFn[kFastBuffer] === true &&
      (type === 'buffer' || type === 'arraybuffer')) {
    return false;
  }
  return needsRawPointerConversion(type, rawFn) ||
         needsNullPointerConversion(type) || needsStringPointerConversion(type);
}

function hasStringPointerArg(type, value) {
  // SharedBuffer uses this helper too, so keep the runtime value check separate
  // from the type-level predicate.
  return typeof value === 'string' && needsStringPointerConversion(type);
}

function hasPointerMemoryArg(type, value) {
  // Buffer and ArrayBuffer-backed values can be converted to a backing-store
  // address for raw pointer-like signatures.
  return (needsRawPointerConversion(type) || needsStringPointerConversion(type)) &&
         (isArrayBufferView(value) || isAnyArrayBuffer(value));
}

function getStringConversionPointer(owner, value, index) {
  // Allocate pessimistically for UTF-8 (`3 * length`) plus the trailing NUL.
  // The exact byte count is known only after encoding.
  const size = value.length * 3 + 1;
  let buffers = owner[kStringConversionBuffer];
  if (buffers === undefined) {
    // Keep one conversion slot per argument index. This avoids per-call Symbol
    // lookups on the wrapper function and keeps buffers tied to the library
    // lifetime rather than a temporary wrapper invocation.
    buffers = [];
    ObjectDefineProperty(owner, kStringConversionBuffer, {
      __proto__: null,
      configurable: false,
      enumerable: false,
      writable: false,
      value: buffers,
    });
  }
  let entry = buffers[index];
  if (entry !== undefined && entry.string === value) {
    // Single-entry same-string reuse handles hot loops such as strlen("foo")
    // without introducing an unbounded string-to-buffer cache.
    return entry.pointer;
  }
  if (StringPrototypeIncludes(value, '\0')) {
    throwFFIArgError(`Argument ${index} must not contain null bytes`);
  }
  if (entry === undefined || entry.buffer.length < size) {
    // Grow-only reuse: keep the existing backing store when it is large enough,
    // otherwise allocate a bigger Buffer and cache its raw pointer once.
    const buffer = Buffer.allocUnsafe(size);
    entry = {
      __proto__: null,
      buffer,
      pointer: getRawPointer(buffer),
      string: undefined,
    };
    buffers[index] = entry;
  }

  const buffer = entry.buffer;
  // Encode directly into the reusable Buffer and append the C terminator that
  // native string/pointer consumers expect.
  const written = buffer.write(value, 0, size - 1, 'utf8');
  buffer[written] = 0;
  entry.string = value;
  return entry.pointer;
}

function convertPointerArg(type, value, owner, index) {
  // Preserve the conversion order used by public FFI semantics: nullish first,
  // then strings with temporary ownership, then memory-backed objects, then the
  // generic raw pointer extraction path for buffer/arraybuffer declarations.
  if (needsNullPointerConversion(type) &&
      (value === null || value === undefined)) {
    return 0n;
  }
  if (hasStringPointerArg(type, value)) {
    return getStringConversionPointer(owner, value, index);
  }
  if (hasPointerMemoryArg(type, value)) {
    return getRawPointer(value);
  }
  if (needsRawPointerConversion(type)) {
    return getRawPointer(value);
  }
  return value;
}

function getPointerConversionIndexes(parameters, rawFn) {
  // Return null instead of [] so callers can cheaply detect that no wrapper is
  // needed and leave the native Fast API function untouched.
  let indexes = null;
  for (let i = 0; i < parameters.length; i++) {
    if (!needsPointerConversion(parameters[i], rawFn)) {
      continue;
    }
    if (indexes === null) {
      indexes = [];
    }
    indexes.push(i);
  }
  return indexes;
}

function initializeFastBufferMetadata(rawFn, parameters) {
  // This function annotates raw Fast API functions after `lib/ffi.js` has the
  // user-facing signature. It deliberately skips SharedBuffer functions because
  // those are routed by `wrapWithSharedBuffer()` instead.
  if (rawFn === undefined || rawFn === null || parameters === undefined) {
    return;
  }
  if (rawFn[kSbSharedBuffer] !== undefined) {
    return;
  }

  const nativeParams = rawFn[kFastParams];
  if (nativeParams !== undefined) {
    // If native metadata exists and the user signature contains buffer-shaped
    // parameters, keep them as V8 values so the generated trampoline can use
    // node_ffi_fast_buffer_data().
    for (let i = 0; i < parameters.length; i++) {
      const type = parameters[i];
      if (type === 'buffer' || type === 'arraybuffer') {
        rawFn[kFastBuffer] = true;
        break;
      }
    }
  }

  // The native side attaches `kFastBufferInvoke` for monomorphic pointer-like
  // signatures when a secondary buffer trampoline exists.
}

// The `pointer` descriptor mirrors the raw function's so user code that
// reassigns `.pointer` keeps working through the wrapper.
function inheritMetadata(wrapper, rawFn, nargs) {
  ObjectDefineProperty(wrapper, 'name', {
    __proto__: null, value: rawFn.name, configurable: true,
  });
  ObjectDefineProperty(wrapper, 'length', {
    __proto__: null, value: nargs, configurable: true,
  });
  ObjectDefineProperty(wrapper, 'pointer', {
    __proto__: null, value: rawFn.pointer,
    writable: true, configurable: true, enumerable: true,
  });
  return wrapper;
}

function wrapWithRawPointerConversions(rawFn, parameters, owner) {
  // The raw function is returned unchanged when there is no Fast API metadata or
  // no argument requires JS-side pointer conversion.
  if (rawFn === undefined || rawFn === null) {
    return rawFn;
  }
  if (parameters === undefined) {
    parameters = rawFn[kFastParams];
  }
  if (parameters === undefined) {
    return rawFn;
  }

  const indexes = getPointerConversionIndexes(parameters, rawFn);
  if (indexes === null) {
    return rawFn;
  }

  const nargs = parameters.length;
  let wrapper;
  if (nargs === 1 && indexes.length === 1 && indexes[0] === 0) {
    // The monomorphic one-argument wrapper is the hot path for pointer/string
    // helpers. It avoids allocating a rest-args array and can dispatch directly
    // to the secondary buffer-shaped Fast API function when available.
    const t0 = parameters[0];
    const string0 = needsStringPointerConversion(t0);
    const memory0 = needsRawPointerConversion(t0) || string0;
    const fastBufferInvoke = needsPointerLikeConversion(t0) ?
      rawFn[kFastBufferInvoke] : undefined;
    wrapper = function(a0) {
      if (arguments.length !== 1) {
        throwFFIArgCountError(1, arguments.length);
      }
      let arg = a0;
      if (needsNullPointerConversion(t0) &&
          (arg === null || arg === undefined)) {
        arg = 0n;
      } else if (string0 && typeof arg === 'string') {
        arg = getStringConversionPointer(owner, arg, 0);
      } else if (memory0 && (isArrayBufferView(arg) || isAnyArrayBuffer(arg))) {
        if (fastBufferInvoke !== undefined) {
          // Keep two Fast API representations for single pointer-like
          // signatures: scalar u64 for BigInt/null/string-converted pointers,
          // and buffer-shaped for Buffer/ArrayBuffer inputs. Replacing one with
          // the other would make one of those cases fall back to a slower path.
          return fastBufferInvoke(arg);
        }
        arg = getRawPointer(arg);
      }
      const result = rawFn(arg);
      return result;
    };
  } else if (nargs === 2) {
    // Small fixed-arity wrappers preserve the raw function's call shape and
    // avoid rest-args allocation while still converting only the needed slots.
    const c0 = ArrayPrototypeIncludes(indexes, 0);
    const c1 = ArrayPrototypeIncludes(indexes, 1);
    const t0 = parameters[0];
    const t1 = parameters[1];
    wrapper = function(a0, a1) {
      if (arguments.length !== 2) {
        throwFFIArgCountError(2, arguments.length);
      }
      return rawFn(c0 ? convertPointerArg(t0, a0, owner, 0) : a0,
                   c1 ? convertPointerArg(t1, a1, owner, 1) : a1);
    };
  } else if (nargs === 3) {
    // Three arguments is the last fixed specialization currently worth keeping;
    // larger signatures use the generic loop below to avoid code bloat.
    const c0 = ArrayPrototypeIncludes(indexes, 0);
    const c1 = ArrayPrototypeIncludes(indexes, 1);
    const c2 = ArrayPrototypeIncludes(indexes, 2);
    const t0 = parameters[0];
    const t1 = parameters[1];
    const t2 = parameters[2];
    wrapper = function(a0, a1, a2) {
      if (arguments.length !== 3) {
        throwFFIArgCountError(3, arguments.length);
      }
      return rawFn(c0 ? convertPointerArg(t0, a0, owner, 0) : a0,
                   c1 ? convertPointerArg(t1, a1, owner, 1) : a1,
                   c2 ? convertPointerArg(t2, a2, owner, 2) : a2);
    };
  } else {
    // Generic fallback for larger signatures. Mutating the rest-args array is
    // safe because it is invocation-local, then ReflectApply forwards the final
    // converted argument list to the raw Fast API function.
    wrapper = function(...args) {
      if (args.length !== nargs) {
        throwFFIArgCountError(nargs, args.length);
      }
      for (let i = 0; i < indexes.length; i++) {
        const index = indexes[i];
        args[index] = convertPointerArg(
          parameters[index], args[index], owner, index);
      }
      return ReflectApply(rawFn, undefined, args);
    };
  }

  return inheritMetadata(wrapper, rawFn, nargs);
}

module.exports = {
  convertPointerArg,
  hasPointerMemoryArg,
  hasStringPointerArg,
  initializeFastBufferMetadata,
  wrapWithRawPointerConversions,
};
