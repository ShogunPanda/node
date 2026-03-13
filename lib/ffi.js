'use strict';
const { emitExperimentalWarning } = require('internal/util');

emitExperimentalWarning('FFI');

const { DynamicLibrary, toString, toBuffer, toArrayBuffer } = internalBinding('ffi');

module.exports = {
  DynamicLibrary,
  toString,
  toArrayBuffer,
  toBuffer
};
