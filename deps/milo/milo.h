#ifndef MILO_H
#define MILO_H

#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <new>

#define MILO_VERSION "0.1.0"
#define MILO_VERSION_MAJOR 0
#define MILO_VERSION_MINOR 1
#define MILO_VERSION_PATCH 0

#define MILO_METHODS_MAP(EACH) \
  EACH(0, ACL, ACL) \
  EACH(1, BASELINE_CONTROL, BASELINE_CONTROL) \
  EACH(2, BIND, BIND) \
  EACH(3, CHECKIN, CHECKIN) \
  EACH(4, CHECKOUT, CHECKOUT) \
  EACH(5, CONNECT, CONNECT) \
  EACH(6, COPY, COPY) \
  EACH(7, DELETE, DELETE) \
  EACH(8, GET, GET) \
  EACH(9, HEAD, HEAD) \
  EACH(10, LABEL, LABEL) \
  EACH(11, LINK, LINK) \
  EACH(12, LOCK, LOCK) \
  EACH(13, MERGE, MERGE) \
  EACH(14, MKACTIVITY, MKACTIVITY) \
  EACH(15, MKCALENDAR, MKCALENDAR) \
  EACH(16, MKCOL, MKCOL) \
  EACH(17, MKREDIRECTREF, MKREDIRECTREF) \
  EACH(18, MKWORKSPACE, MKWORKSPACE) \
  EACH(19, MOVE, MOVE) \
  EACH(20, OPTIONS, OPTIONS) \
  EACH(21, ORDERPATCH, ORDERPATCH) \
  EACH(22, PATCH, PATCH) \
  EACH(23, POST, POST) \
  EACH(24, PRI, PRI) \
  EACH(25, PROPFIND, PROPFIND) \
  EACH(26, PROPPATCH, PROPPATCH) \
  EACH(27, PUT, PUT) \
  EACH(28, REBIND, REBIND) \
  EACH(29, REPORT, REPORT) \
  EACH(30, SEARCH, SEARCH) \
  EACH(31, TRACE, TRACE) \
  EACH(32, UNBIND, UNBIND) \
  EACH(33, UNCHECKOUT, UNCHECKOUT) \
  EACH(34, UNLINK, UNLINK) \
  EACH(35, UNLOCK, UNLOCK) \
  EACH(36, UPDATE, UPDATE) \
  EACH(37, UPDATEREDIRECTREF, UPDATEREDIRECTREF) \
  EACH(38, VERSION_CONTROL, VERSION_CONTROL) \
  EACH(39, DESCRIBE, DESCRIBE) \
  EACH(40, GET_PARAMETER, GET_PARAMETER) \
  EACH(41, PAUSE, PAUSE) \
  EACH(42, PLAY, PLAY) \
  EACH(43, PLAY_NOTIFY, PLAY_NOTIFY) \
  EACH(44, REDIRECT, REDIRECT) \
  EACH(45, SETUP, SETUP) \
  EACH(46, SET_PARAMETER, SET_PARAMETER) \
  EACH(47, TEARDOWN, TEARDOWN) \
  EACH(48, PURGE, PURGE) \

namespace milo {

struct Parser;

constexpr static const uint8_t AUTODETECT = 0;

constexpr static const uint8_t REQUEST = 1;

constexpr static const uint8_t RESPONSE = 2;

constexpr static const uint8_t CONNECTION_KEEPALIVE = 0;

constexpr static const uint8_t CONNECTION_CLOSE = 1;

constexpr static const uint8_t CONNECTION_UPGRADE = 2;

constexpr static const uint8_t ERROR_NONE = 0;

constexpr static const uint8_t ERROR_UNEXPECTED_DATA = 1;

constexpr static const uint8_t ERROR_UNEXPECTED_EOF = 2;

constexpr static const uint8_t ERROR_CALLBACK_ERROR = 3;

constexpr static const uint8_t ERROR_UNEXPECTED_CHARACTER = 4;

constexpr static const uint8_t ERROR_UNEXPECTED_CONTENT_LENGTH = 5;

constexpr static const uint8_t ERROR_UNEXPECTED_TRANSFER_ENCODING = 6;

constexpr static const uint8_t ERROR_UNEXPECTED_CONTENT = 7;

constexpr static const uint8_t ERROR_UNTRAILERS = 8;

constexpr static const uint8_t ERROR_INVALID_VERSION = 9;

constexpr static const uint8_t ERROR_INVALID_STATUS = 10;

constexpr static const uint8_t ERROR_INVALID_CONTENT_LENGTH = 11;

constexpr static const uint8_t ERROR_INVALID_TRANSFER_ENCODING = 12;

constexpr static const uint8_t ERROR_INVALID_CHUNK_SIZE = 13;

constexpr static const uint8_t ERROR_MISSING_CONNECTION_UPGRADE = 14;

constexpr static const uint8_t ERROR_UNSUPPORTED_HTTP_VERSION = 15;

constexpr static const uint8_t METHOD_ACL = 0;

constexpr static const uint8_t METHOD_BASELINE_CONTROL = 1;

constexpr static const uint8_t METHOD_BIND = 2;

constexpr static const uint8_t METHOD_CHECKIN = 3;

constexpr static const uint8_t METHOD_CHECKOUT = 4;

constexpr static const uint8_t METHOD_CONNECT = 5;

constexpr static const uint8_t METHOD_COPY = 6;

constexpr static const uint8_t METHOD_DELETE = 7;

constexpr static const uint8_t METHOD_GET = 8;

constexpr static const uint8_t METHOD_HEAD = 9;

constexpr static const uint8_t METHOD_LABEL = 10;

constexpr static const uint8_t METHOD_LINK = 11;

constexpr static const uint8_t METHOD_LOCK = 12;

constexpr static const uint8_t METHOD_MERGE = 13;

constexpr static const uint8_t METHOD_MKACTIVITY = 14;

constexpr static const uint8_t METHOD_MKCALENDAR = 15;

constexpr static const uint8_t METHOD_MKCOL = 16;

constexpr static const uint8_t METHOD_MKREDIRECTREF = 17;

constexpr static const uint8_t METHOD_MKWORKSPACE = 18;

constexpr static const uint8_t METHOD_MOVE = 19;

constexpr static const uint8_t METHOD_OPTIONS = 20;

constexpr static const uint8_t METHOD_ORDERPATCH = 21;

constexpr static const uint8_t METHOD_PATCH = 22;

constexpr static const uint8_t METHOD_POST = 23;

constexpr static const uint8_t METHOD_PRI = 24;

constexpr static const uint8_t METHOD_PROPFIND = 25;

constexpr static const uint8_t METHOD_PROPPATCH = 26;

constexpr static const uint8_t METHOD_PUT = 27;

constexpr static const uint8_t METHOD_REBIND = 28;

constexpr static const uint8_t METHOD_REPORT = 29;

constexpr static const uint8_t METHOD_SEARCH = 30;

constexpr static const uint8_t METHOD_TRACE = 31;

constexpr static const uint8_t METHOD_UNBIND = 32;

constexpr static const uint8_t METHOD_UNCHECKOUT = 33;

constexpr static const uint8_t METHOD_UNLINK = 34;

constexpr static const uint8_t METHOD_UNLOCK = 35;

constexpr static const uint8_t METHOD_UPDATE = 36;

constexpr static const uint8_t METHOD_UPDATEREDIRECTREF = 37;

constexpr static const uint8_t METHOD_VERSION_CONTROL = 38;

constexpr static const uint8_t METHOD_DESCRIBE = 39;

constexpr static const uint8_t METHOD_GET_PARAMETER = 40;

constexpr static const uint8_t METHOD_PAUSE = 41;

constexpr static const uint8_t METHOD_PLAY = 42;

constexpr static const uint8_t METHOD_PLAY_NOTIFY = 43;

constexpr static const uint8_t METHOD_REDIRECT = 44;

constexpr static const uint8_t METHOD_SETUP = 45;

constexpr static const uint8_t METHOD_SET_PARAMETER = 46;

constexpr static const uint8_t METHOD_TEARDOWN = 47;

constexpr static const uint8_t METHOD_PURGE = 48;

constexpr static const uint8_t STATE_START = 0;

constexpr static const uint8_t STATE_FINISH = 1;

constexpr static const uint8_t STATE_ERROR = 2;

constexpr static const uint8_t STATE_MESSAGE = 3;

constexpr static const uint8_t STATE_END = 4;

constexpr static const uint8_t STATE_REQUEST = 5;

constexpr static const uint8_t STATE_REQUEST_METHOD = 6;

constexpr static const uint8_t STATE_REQUEST_URL = 7;

constexpr static const uint8_t STATE_REQUEST_PROTOCOL = 8;

constexpr static const uint8_t STATE_REQUEST_VERSION = 9;

constexpr static const uint8_t STATE_RESPONSE = 10;

constexpr static const uint8_t STATE_RESPONSE_VERSION = 11;

constexpr static const uint8_t STATE_RESPONSE_STATUS = 12;

constexpr static const uint8_t STATE_RESPONSE_REASON = 13;

constexpr static const uint8_t STATE_HEADER_NAME = 14;

constexpr static const uint8_t STATE_HEADER_TRANSFER_ENCODING = 15;

constexpr static const uint8_t STATE_HEADER_CONTENT_LENGTH = 16;

constexpr static const uint8_t STATE_HEADER_CONNECTION = 17;

constexpr static const uint8_t STATE_HEADER_VALUE = 18;

constexpr static const uint8_t STATE_HEADERS = 19;

constexpr static const uint8_t STATE_BODY = 20;

constexpr static const uint8_t STATE_TUNNEL = 21;

constexpr static const uint8_t STATE_BODY_VIA_CONTENT_LENGTH = 22;

constexpr static const uint8_t STATE_BODY_WITH_NO_LENGTH = 23;

constexpr static const uint8_t STATE_CHUNK_LENGTH = 24;

constexpr static const uint8_t STATE_CHUNK_EXTENSION_NAME = 25;

constexpr static const uint8_t STATE_CHUNK_EXTENSION_VALUE = 26;

constexpr static const uint8_t STATE_CHUNK_EXTENSION_QUOTED_VALUE = 27;

constexpr static const uint8_t STATE_CHUNK_DATA = 28;

constexpr static const uint8_t STATE_CHUNK_END = 29;

constexpr static const uint8_t STATE_CRLF_AFTER_LAST_CHUNK = 30;

constexpr static const uint8_t STATE_TRAILER_NAME = 31;

constexpr static const uint8_t STATE_TRAILER_VALUE = 32;

using Callback = intptr_t(*)(const Parser*, const unsigned char*, uintptr_t);

struct Callbacks {
  Callback before_state_change;
  Callback after_state_change;
  Callback on_error;
  Callback on_finish;
  Callback on_message_start;
  Callback on_message_complete;
  Callback on_request;
  Callback on_response;
  Callback on_reset;
  Callback on_method;
  Callback on_url;
  Callback on_protocol;
  Callback on_version;
  Callback on_status;
  Callback on_reason;
  Callback on_header_name;
  Callback on_header_value;
  Callback on_headers;
  Callback on_connect;
  Callback on_upgrade;
  Callback on_chunk_length;
  Callback on_chunk_extension_name;
  Callback on_chunk_extension_value;
  Callback on_body;
  Callback on_data;
  Callback on_trailer_name;
  Callback on_trailer_value;
  Callback on_trailers;
};

struct Parser {
  void *owner;
  uint8_t state;
  uint64_t position;
  bool paused;
  uint8_t error_code;
  const unsigned char *error_description;
  uintptr_t error_description_len;
  const unsigned char *unconsumed;
  uintptr_t unconsumed_len;
  uint8_t id;
  uint8_t mode;
  bool continue_without_data;
  uint8_t message_type;
  bool is_connect;
  uint8_t method;
  uint32_t status;
  uint8_t version_major;
  uint8_t version_minor;
  uint8_t connection;
  bool has_content_length;
  bool has_chunked_transfer_encoding;
  bool has_upgrade;
  bool has_trailers;
  uint64_t content_length;
  uint64_t chunk_size;
  uint64_t remaining_content_length;
  uint64_t remaining_chunk_size;
  bool skip_body;
  Callbacks callbacks;
};

extern "C" {

/// A callback that simply returns `0`.
///
/// Use this callback as pointer when you want to remove a callback from the
/// parser.
intptr_t milo_noop(const Parser *_parser, const unsigned char *_data, uintptr_t _len);

/// Cleans up memory used by a string previously returned by one of the milo's C
/// public interface.
void milo_free_string(const unsigned char *s);

/// Creates a new parser.
Parser *milo_create();

/// # Safety
///
/// Destroys a parser.
void milo_destroy(Parser *ptr);

/// # Safety
///
/// Resets a parser to its initial state.
void milo_reset(const Parser *parser, bool keep_position);

/// # Safety
///
/// Parses a slice of characters. It returns the number of consumed characters.
uintptr_t milo_parse(const Parser *parser, const unsigned char *data, uintptr_t limit);

/// # Safety
///
/// Pauses the parser. It will have to be resumed via `milo_resume`.
void milo_pause(const Parser *parser);

/// # Safety
///
/// Resumes the parser.
void milo_resume(const Parser *parser);

/// # Safety
///
/// Marks the parser as finished. Any new data received via `milo_parse` will
/// put the parser in the error state.
void milo_finish(const Parser *parser);

/// # Safety
///
/// Returns the current parser's state as string.
///
/// The returned value must be freed using `free_string`.
const unsigned char *milo_state_string(const Parser *parser);

/// # Safety
///
/// Returns the current parser's error state as string.
///
/// The returned value must be freed using `free_string`.
const unsigned char *milo_error_code_string(const Parser *parser);

/// # Safety
///
/// Returns the current parser's error descrition.
///
/// The returned value must be freed using `free_string`.
const unsigned char *milo_error_description_string(const Parser *parser);

} // extern "C"

} // namespace milo

#endif // MILO_H
