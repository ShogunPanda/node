'use strict';
const { emitExperimentalWarning } = require('internal/util');

emitExperimentalWarning('FFI');

const { DynamicLibrary, toString, toBuffer, toArrayBuffer } = internalBinding('ffi');

// TOOD: Restore the parameters / returns semantic of functions signature. 
module.exports = {
  DynamicLibrary,
  toString,
  toArrayBuffer,
  toBuffer
};
