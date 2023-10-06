// Copyright Joyent, Inc. and other Node contributors.
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the
// following conditions:
//
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
// NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
// OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
// USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "node.h"
#include "node_buffer.h"
#include "util.h"

#include "async_wrap-inl.h"
#include "env-inl.h"
#include "memory_tracker-inl.h"
#include "stream_base-inl.h"
#include "v8.h"
#include "milo.h"

#include <cstdlib>  // free()
#include <cstring>  // strdup(), strchr()

namespace node {
namespace {  // NOLINT(build/namespaces)

using v8::Array;
using v8::Boolean;
using v8::Context;
using v8::EscapableHandleScope;
using v8::Exception;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Int32;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Number;
using v8::Object;
using v8::String;
using v8::Uint32;
using v8::Undefined;
using v8::Value;

const uint32_t kOnMessageBegin = 0;
const uint32_t kOnHeaders = 1;
const uint32_t kOnHeadersComplete = 2;
const uint32_t kOnBody = 3;
const uint32_t kOnTrailers = 4;
const uint32_t kOnTrailersComplete = 5;
const uint32_t kOnMessageComplete = 6;
const uint32_t kOnExecute = 7;

// Any more fields than this will be flushed into JS
const size_t kMaxHeaderFieldsCount = 32;
const size_t kMaxTrailerFieldsCount = 32;

class BindingData : public BaseObject {
 public:
  BindingData(Realm* realm, Local<Object> obj) : BaseObject(realm, obj) {}

  SET_BINDING_ID(http_parser_binding_data)

  std::vector<char> parser_buffer;
  bool parser_buffer_in_use = false;

  void MemoryInfo(MemoryTracker* tracker) const override {
    tracker->TrackField("parser_buffer", parser_buffer);
  }
  SET_SELF_SIZE(BindingData)
  SET_MEMORY_INFO_NAME(BindingData)
};

// helper class for the Parser
struct Data {
  Data() {
    data_ = nullptr;
    size_ = 0;
  }

  ~Data() {
    Reset();
  }

  void Reset() {
    if (size_ == 0) {
      return;
    }

    free(data_);
    size_ = 0;
  }

  void Set(const unsigned char* str, size_t size) {
    if (size_ > 0) {
      free(data_);
    }

    data_ = UncheckedMalloc<unsigned char>(sizeof(unsigned char) * size);
    size_ = size;
    memcpy(data_, str, size_);
  }

  Local<String> ToString(Isolate* isolate) const {
    if (size_ != 0)
      return OneByteString(isolate, data_, size_);
    else
      return String::Empty(isolate);
  }

  Local<String> ToTrimmedString(Isolate* isolate) const {
    if (size_ == 0)
      return String::Empty(isolate);

    size_t size = size_;
    while (size > 0 && (data_[size - 1] == ' ' || data_[size - 1] == '\t')) {
      size--;
    }

    return OneByteString(isolate, data_, size);
  }


  unsigned char* data_;
  size_t size_;
};

class Parser;

struct ParserComparator {
  bool operator()(const Parser* lhs, const Parser* rhs) const;
};

class ConnectionsList : public BaseObject {
 public:
    static void New(const FunctionCallbackInfo<Value>& args);


    static void All(const FunctionCallbackInfo<Value>& args);


    static void Idle(const FunctionCallbackInfo<Value>& args);


    static void Active(const FunctionCallbackInfo<Value>& args);


    static void Expired(const FunctionCallbackInfo<Value>& args);


    void Push(Parser* parser) {
      all_connections_.insert(parser);
    }

    void Pop(Parser* parser) {
      all_connections_.erase(parser);
    }

    void PushActive(Parser* parser) {
      active_connections_.insert(parser);
    }

    void PopActive(Parser* parser) {
      active_connections_.erase(parser);
    }

    SET_NO_MEMORY_INFO()
    SET_MEMORY_INFO_NAME(ConnectionsList)
    SET_SELF_SIZE(ConnectionsList)

 private:
    ConnectionsList(Environment* env, Local<Object> object)
      : BaseObject(env, object) {
        MakeWeak();
      }

    std::set<Parser*, ParserComparator> all_connections_;
    std::set<Parser*, ParserComparator> active_connections_;
};

class Parser : public AsyncWrap, public StreamListener {
  friend class ConnectionsList;
  friend struct ParserComparator;

 public:
  Parser(BindingData* binding_data, Local<Object> wrap)
      : AsyncWrap(binding_data->env(), wrap),
        current_buffer_len_(0),
        current_buffer_data_(nullptr),
        binding_data_(binding_data) {
  }

  SET_NO_MEMORY_INFO()
  SET_MEMORY_INFO_NAME(Parser)
  SET_SELF_SIZE(Parser)

  intptr_t on_message_start(const milo::Parser* parser,
                            const unsigned char* data, uintptr_t length) {
    // Important: Pop from the lists BEFORE resetting the last_message_start_
    // otherwise std::set.erase will fail.
    if (connectionsList_ != nullptr) {
      connectionsList_->Pop(this);
      connectionsList_->PopActive(this);
    }

    num_header_fields_ = num_header_values_ = 0;
    num_trailer_fields_ = num_trailer_values_ = 0;
    headers_completed_ = false;
    trailers_completed_ = false;
    headers_flushed_ = false;
    trailers_flushed_ = false;
    last_message_start_ = uv_hrtime();

    url_.Reset();
    status_message_.Reset();
    error_code_.Reset();
    error_reason_.Reset();

    if (connectionsList_ != nullptr) {
      connectionsList_->Push(this);
      connectionsList_->PushActive(this);
    }

    Local<Value> cb = object()->Get(env()->context(), kOnMessageBegin)
                              .ToLocalChecked();

    if (cb->IsFunction()) {
      InternalCallbackScope callback_scope(
        this, InternalCallbackScope::kSkipTaskQueues);

      MaybeLocal<Value> r = cb.As<Function>()->Call(
        env()->context(), object(), 0, nullptr);

      if (r.IsEmpty()) callback_scope.MarkAsFailed();
    }

    return 0;
  }

  intptr_t on_url(const milo::Parser* parser,
                  const unsigned char* data, uintptr_t length) {
    int rv = TrackHeader(length);
    if (rv != 0) {
      return 1;
    }

    url_.Set(data, length);

    return 0;
  }


  intptr_t on_reason(const milo::Parser* parser,
                     const unsigned char* data, uintptr_t length) {
    int rv = TrackHeader(length);
    if (rv != 0) {
      return 1;
    }

    status_message_.Set(data, length);

    return 0;
  }


  intptr_t on_header_name(const milo::Parser* parser,
                           const unsigned char* data, uintptr_t length) {
    int rv = TrackHeader(length);
    if (rv != 0) {
      return rv;
    }

    CHECK_EQ(num_header_fields_, num_header_values_);

    // start of new field name
    num_header_fields_++;
    if (num_header_fields_ == kMaxHeaderFieldsCount) {
      // ran out of space - flush to javascript land
      FlushHeaders();
      num_header_fields_ = 1;
      num_header_values_ = 0;
    }

    header_fields_[num_header_fields_ - 1].Set(data, length);

    CHECK_LT(num_header_fields_, kMaxHeaderFieldsCount);
    CHECK_EQ(num_header_fields_, num_header_values_ + 1);

    return 0;
  }


  intptr_t on_header_value(const milo::Parser* parser,
                           const unsigned char* data, uintptr_t length) {
    int rv = TrackHeader(length);
    if (rv != 0) {
      return rv;
    }

    CHECK_NE(num_header_values_, num_header_fields_);

    num_header_values_++;
    header_values_[num_header_values_ - 1].Set(data, length);

    CHECK_LT(num_header_values_, arraysize(header_values_));
    CHECK_EQ(num_header_values_, num_header_fields_);

    return 0;
  }


  intptr_t on_headers(const milo::Parser* parser,
                               const unsigned char* data, uintptr_t length) {
    headers_completed_ = true;
    heades_nread_ = 0;

    // Arguments for the on-headers-complete javascript callback. This
    // list needs to be kept in sync with the actual argument list for
    // `parserOnHeadersComplete` in lib/_http_common.js.
    enum on_headers_complete_arg_index {
      A_VERSION_MAJOR = 0,
      A_VERSION_MINOR,
      A_HEADERS,
      A_METHOD,
      A_URL,
      A_STATUS_CODE,
      A_STATUS_MESSAGE,
      A_UPGRADE,
      A_SHOULD_KEEP_ALIVE,
      A_MAX
    };

    Local<Value> argv[A_MAX];
    Local<Object> obj = object();
    Local<Value> cb = obj->Get(env()->context(),
                               kOnHeadersComplete).ToLocalChecked();

    if (!cb->IsFunction())
      return 0;

    Isolate* isolate = env()->isolate();

    Local<Value> undefined = Undefined(isolate);
    for (size_t i = 0; i < arraysize(argv); i++)
      argv[i] = undefined;

    if (headers_flushed_) {
      // Slow case, flush remaining headers.
      FlushHeaders();
    } else {
      // Fast case, pass headers and URL to JS land.
      argv[A_HEADERS] = CreateHeaders();
      if (parser_->mode == milo::REQUEST)
        argv[A_URL] = url_.ToString(isolate);
    }

    num_header_fields_ = 0;
    num_header_values_ = 0;

    // METHOD
    if (parser_->mode == milo::REQUEST) {
      argv[A_METHOD] =
          Uint32::NewFromUnsigned(isolate, parser_->method);
    } else {
    // STATUS
      argv[A_STATUS_CODE] =
          Integer::New(isolate, parser_->status);
      argv[A_STATUS_MESSAGE] = status_message_.ToString(isolate);
    }

    // VERSION
    argv[A_VERSION_MAJOR] = Integer::New(isolate, parser_->version_major);
    argv[A_VERSION_MINOR] = Integer::New(isolate, parser_->version_minor);

    // KEEP ALIVE
    argv[A_SHOULD_KEEP_ALIVE] = Boolean::New(isolate, parser_->connection !=
                                                 milo::CONNECTION_CLOSE);

    argv[A_UPGRADE] = Boolean::New(isolate, parser_->is_connect ||
                                   parser_->has_upgrade);

    MaybeLocal<Value> head_response;
    {
      InternalCallbackScope callback_scope(
          this, InternalCallbackScope::kSkipTaskQueues);
      head_response = cb.As<Function>()->Call(
          env()->context(), object(), arraysize(argv), argv);
      if (head_response.IsEmpty()) callback_scope.MarkAsFailed();
    }

    int64_t val = 0;

    if (head_response.IsEmpty() || !head_response.ToLocalChecked()
                                        ->IntegerValue(env()->context())
                                        .To(&val)) {
      got_exception_ = true;
      return -1;
    }

    if (val > 0) {
      parser_->skip_body = 1;
    }

    return static_cast<intptr_t>(0);
  }


  intptr_t on_body(const milo::Parser* parser,
                   const unsigned char* data, uintptr_t length) {
    if (length == 0)
      return 0;

    Environment* env = this->env();
    HandleScope handle_scope(env->isolate());

    Local<Value> cb = object()->Get(env->context(), kOnBody).ToLocalChecked();

    if (!cb->IsFunction())
      return 0;

    Local<Value> buffer = Buffer::Copy(env,
                                       reinterpret_cast<const char*>(data),
                                       length).ToLocalChecked();

    MaybeLocal<Value> r = MakeCallback(cb.As<Function>(), 1, &buffer);

    if (r.IsEmpty()) {
      got_exception_ = true;
      return 1;
    }

    return 0;
  }


  intptr_t on_message_complete(const milo::Parser* parser,
                               const unsigned char* data, uintptr_t length) {
    HandleScope scope(env()->isolate());

    // Important: Pop from the lists BEFORE resetting the last_message_start_
    // otherwise std::set.erase will fail.
    if (connectionsList_ != nullptr) {
      connectionsList_->Pop(this);
      connectionsList_->PopActive(this);
    }

    last_message_start_ = 0;

    if (connectionsList_ != nullptr) {
      connectionsList_->Push(this);
    }

    if (trailers_flushed_)
      FlushTrailers();  // Flush trailing HTTP trailers.

    Local<Object> obj = object();
    Local<Value> cb = obj->Get(env()->context(),
                               kOnMessageComplete).ToLocalChecked();

    if (!cb->IsFunction())
      return 0;

    MaybeLocal<Value> r;
    {
      InternalCallbackScope callback_scope(
          this, InternalCallbackScope::kSkipTaskQueues);
      r = cb.As<Function>()->Call(env()->context(), object(), 0, nullptr);
      if (r.IsEmpty()) callback_scope.MarkAsFailed();
    }

    if (r.IsEmpty()) {
      got_exception_ = true;
      return 1;
    }

    return 0;
  }


  intptr_t on_trailer_name(const milo::Parser* parser,
                            const unsigned char* data, uintptr_t length) {
    int rv = TrackTrailer(length);
    if (rv != 0) {
      return rv;
    }

    // start of new field name
    num_trailer_fields_++;
    if (num_trailer_fields_ == kMaxTrailerFieldsCount) {
      // ran out of space - flush to javascript land
      FlushTrailers();
      num_trailer_fields_ = 1;
      num_trailer_values_ = 0;
    }

    trailer_fields_[num_trailer_fields_ - 1].Set(data, length);

    CHECK_LT(num_trailer_fields_, kMaxTrailerFieldsCount);
    CHECK_EQ(num_trailer_fields_, num_trailer_values_ + 1);

    return 0;
  }


  intptr_t on_trailer_value(const milo::Parser* parser,
                            const unsigned char* data, uintptr_t length) {
    int rv = TrackTrailer(length);
    if (rv != 0) {
      return rv;
    }

    CHECK_NE(num_trailer_values_, num_trailer_fields_);

    num_trailer_values_++;
    trailer_values_[num_trailer_values_ - 1].Set(data, length);

    CHECK_LT(num_trailer_values_, arraysize(trailer_values_));
    CHECK_EQ(num_trailer_values_, num_trailer_fields_);

    return 0;
  }


  intptr_t on_trailers(const milo::Parser* parser,
                                const unsigned char* data, uintptr_t length) {
    trailers_completed_ = true;
    trailers_nread_ = 0;

    Local<Value> argv[1];
    Local<Object> obj = object();
    Local<Value> cb = obj->Get(env()->context(),
                               kOnTrailersComplete).ToLocalChecked();

    if (!cb->IsFunction())
      return 0;

    Local<Value> undefined = Undefined(env()->isolate());

    if (trailers_flushed_) {
      // Slow case, flush remaining headers.
      FlushTrailers();
      argv[0] = undefined;
    } else {
      // Fast case, pass headers and URL to JS land.
      argv[0] = CreateTrailers();
    }

    num_trailer_fields_ = 0;
    num_trailer_values_ = 0;

    MaybeLocal<Value> trailer_response;
    {
      InternalCallbackScope callback_scope(
          this, InternalCallbackScope::kSkipTaskQueues);
      trailer_response = cb.As<Function>()->Call(
          env()->context(), object(), arraysize(argv), argv);
      if (trailer_response.IsEmpty()) callback_scope.MarkAsFailed();
    }

    int64_t val;

    if (trailer_response.IsEmpty() || !trailer_response.ToLocalChecked()
                                        ->IntegerValue(env()->context())
                                        .To(&val)) {
      got_exception_ = true;
      return -1;
    }

    return static_cast<int>(val);
  }


  static void New(const FunctionCallbackInfo<Value>& args) {
    BindingData* binding_data = Realm::GetBindingData<BindingData>(args);
    new Parser(binding_data, args.This());
  }


  static void Close(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    delete parser;
  }


  static void Free(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    // Since the Parser destructor isn't going to run the destroy() callbacks
    // it needs to be triggered manually.
    parser->EmitTraceEventDestroy();
    parser->EmitDestroy();
  }


  static void Remove(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    if (parser->connectionsList_ != nullptr) {
      parser->connectionsList_->Pop(parser);
      parser->connectionsList_->PopActive(parser);
    }
  }

  void Reset(bool keep_position) {
    milo::milo_reset(parser_, keep_position);
    heades_nread_ = 0;
    trailers_nread_ = 0;
    url_.Reset();
    status_message_.Reset();
    error_code_.Reset();
    error_reason_.Reset();
    num_header_fields_ = 0;
    num_header_values_ = 0;
    num_trailer_fields_ = 0;
    num_trailer_values_ = 0;
    headers_flushed_ = false;
    trailers_flushed_ = false;
    got_exception_ = false;
    headers_completed_ = false;
    trailers_completed_ = false;
  }

  // var bytesParsed = parser->execute(buffer);
  static void Execute(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;

    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());
    CHECK(args[1]->IsUint32());

    if (args.Length() > 2 && args[2]->IsTrue()) {
      CHECK(args[2]->IsTrue());
      parser->parser_->is_connect = args[2]->IsTrue() ? 1 : 0;
    }

    ArrayBufferViewContents<char> buffer(args[0]);
    size_t limit = static_cast<size_t>(args[1].As<Number>()->Value());

    Local<Value> ret = parser->Execute(buffer.data(),
                                       limit > 0 ? limit : buffer.length());

    if (!ret.IsEmpty())
      args.GetReturnValue().Set(ret);
  }


  static void Finish(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    Local<Value> ret = parser->Execute(nullptr, 0);

    if (!ret.IsEmpty())
      args.GetReturnValue().Set(ret);
  }


  static void Initialize(const FunctionCallbackInfo<Value>& args) {
    Environment* env = Environment::GetCurrent(args);

    uint64_t max_http_header_size = 0;
    ConnectionsList* connectionsList = nullptr;

    CHECK(args[0]->IsInt32());
    CHECK(args[1]->IsObject());

    if (args.Length() > 2) {
      CHECK(args[2]->IsNumber());
      max_http_header_size =
          static_cast<uint64_t>(args[2].As<Number>()->Value());
    }
    if (max_http_header_size == 0) {
      max_http_header_size = env->options()->max_http_header_size;
    }

    if (args.Length() > 3 && !args[3]->IsNullOrUndefined()) {
      CHECK(args[3]->IsObject());
      ASSIGN_OR_RETURN_UNWRAP(&connectionsList, args[3]);
    }

    intptr_t type = static_cast<intptr_t>(args[0].As<Int32>()->Value());

    CHECK(type == milo::REQUEST || type == milo::RESPONSE);
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());
    // Should always be called from the same context.
    CHECK_EQ(env, parser->env());

    AsyncWrap::ProviderType provider =
        (type == milo::REQUEST ?
            AsyncWrap::PROVIDER_HTTPINCOMINGMESSAGE
            : AsyncWrap::PROVIDER_HTTPCLIENTREQUEST);

    parser->set_provider_type(provider);
    parser->AsyncReset(args[1].As<Object>());
    parser->Init(type, max_http_header_size);

    if (connectionsList != nullptr) {
      parser->connectionsList_ = connectionsList;

      // This protects from a DoS attack where an attacker establishes
      // the connection without sending any data on applications where
      // server.timeout is left to the default value of zero.
      parser->last_message_start_ = uv_hrtime();

      // Important: Push into the lists AFTER setting the last_message_start_
      // otherwise std::set.erase will fail later.
      parser->connectionsList_->Push(parser);
      parser->connectionsList_->PushActive(parser);
    } else {
      parser->connectionsList_ = nullptr;
    }
  }


  static void Reset(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;

    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());
    CHECK(args[0]->IsBoolean());

    parser->Reset(args[0].As<Boolean>()->Value());
  }


  template <bool should_pause>
  static void Pause(const FunctionCallbackInfo<Value>& args) {
    Environment* env = Environment::GetCurrent(args);
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());
    // Should always be called from the same context.
    CHECK_EQ(env, parser->env());

    if constexpr (should_pause) {
      milo::milo_pause(parser->parser_);
    } else {
      milo::milo_resume(parser->parser_);
    }
  }


  static void Consume(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());
    CHECK(args[0]->IsObject());
    StreamBase* stream = StreamBase::FromObject(args[0].As<Object>());
    CHECK_NOT_NULL(stream);
    stream->PushStreamListener(parser);
  }


  static void Unconsume(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    // Already unconsumed
    if (parser->stream_ == nullptr)
      return;

    parser->stream_->RemoveStreamListener(parser);
  }


  static void GetCurrentBuffer(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    Local<Object> ret = Buffer::Copy(
        parser->env(),
        parser->current_buffer_data_,
        parser->current_buffer_len_).ToLocalChecked();

    args.GetReturnValue().Set(ret);
  }

  static void Duration(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    if (parser->last_message_start_ == 0) {
      args.GetReturnValue().Set(0);
      return;
    }

    double duration = (uv_hrtime() - parser->last_message_start_) / 1e6;
    args.GetReturnValue().Set(duration);
  }

  static void HeadersCompleted(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    args.GetReturnValue().Set(parser->headers_completed_);
  }

  static void TrailersCompleted(const FunctionCallbackInfo<Value>& args) {
    Parser* parser;
    ASSIGN_OR_RETURN_UNWRAP(&parser, args.Holder());

    args.GetReturnValue().Set(parser->trailers_completed_);
  }

 protected:
  static const size_t kAllocBufferSize = 64 * 1024;

  uv_buf_t OnStreamAlloc(size_t suggested_size) override {
    // For most types of streams, OnStreamRead will be immediately after
    // OnStreamAlloc, and will consume all data, so using a static buffer for
    // reading is more efficient. For other streams, just use Malloc() directly.
    if (binding_data_->parser_buffer_in_use)
      return uv_buf_init(Malloc(suggested_size), suggested_size);
    binding_data_->parser_buffer_in_use = true;

    if (binding_data_->parser_buffer.empty())
      binding_data_->parser_buffer.resize(kAllocBufferSize);

    return uv_buf_init(binding_data_->parser_buffer.data(), kAllocBufferSize);
  }


  void OnStreamRead(ssize_t nread, const uv_buf_t& buf) override {
    HandleScope scope(env()->isolate());
    // Once we’re done here, either indicate that the HTTP parser buffer
    // is free for re-use, or free() the data if it didn’t come from there
    // in the first place.
    auto on_scope_leave = OnScopeLeave([&]() {
      if (buf.base == binding_data_->parser_buffer.data())
        binding_data_->parser_buffer_in_use = false;
      else
        free(buf.base);
    });

    if (nread < 0) {
      PassReadErrorToPreviousListener(nread);
      return;
    }

    // Ignore, empty reads have special meaning in http parser
    if (nread == 0)
      return;

    Local<Value> ret = Execute(buf.base, nread);

    // Exception
    if (ret.IsEmpty())
      return;

    Local<Value> cb =
        object()->Get(env()->context(), kOnExecute).ToLocalChecked();

    if (!cb->IsFunction())
      return;

    // Hooks for GetCurrentBuffer
    current_buffer_len_ = nread;
    current_buffer_data_ = buf.base;

    MakeCallback(cb.As<Function>(), 1, &ret);

    current_buffer_len_ = 0;
    current_buffer_data_ = nullptr;
  }


  template <typename P,
            intptr_t(P::*Member)(const milo::Parser* parser,
                                 const unsigned char* data, uintptr_t len)>
  static intptr_t CB(const milo::Parser* parser,
                                 const unsigned char* data, uintptr_t len) {
    Parser* container = reinterpret_cast<Parser*>(parser->owner);
    return (container->*Member)(parser, data, len);
  }

  Local<Value> Execute(const char* data, size_t len) {
    Environment* environment = env();
    Isolate* isolate = env()->isolate();
    EscapableHandleScope scope(isolate);

    current_buffer_len_ = len;
    current_buffer_data_ = data;
    got_exception_ = false;

    uintptr_t nread = 0;
    // Finishing can fail so track the previous error
    u_int8_t previous_err = parser_->error_code;

    if (data != nullptr) {
      nread = milo::milo_parse(
                               parser_,
                               reinterpret_cast<const unsigned char *>(data),
                               len);
    } else {
      milo::milo_finish(parser_);
    }

    u_int8_t err = parser_->error_code;

    current_buffer_len_ = 0;
    current_buffer_data_ = nullptr;

    // If there was an exception in one of the callbacks
    if (got_exception_)
      return scope.Escape(Local<Value>());

    Local<Integer> nread_obj = Integer::New(isolate, nread);

    // If there was a parse error in one of the callbacks
    // TODO(bnoordhuis) What if there is an error on EOF?
    if (err != milo::ERROR_NONE && err != previous_err) {
      Local<Value> e = Exception::Error(environment->
                                        parse_error_string());
      Local<Object> obj = e->ToObject(environment->
                                      isolate()->GetCurrentContext())
        .ToLocalChecked();
      obj->Set(environment->context(),
               environment->bytes_parsed_string(),
               nread_obj).Check();

      Local<String> code;
      Local<String> reason;

      if (error_code_.size_ > 0) {
        code = error_code_.ToString(isolate);
        reason = error_reason_.ToString(isolate);
      } else {
        if (
          err == milo::ERROR_UNEXPECTED_TRANSFER_ENCODING ||
          err == milo::ERROR_INVALID_CONTENT_LENGTH
        ) {
          code = OneByteString(environment->isolate(),
                               "HPE_UNEXPECTED_CONTENT_LENGTH");
        } else {
          const char* error_code_string =
              reinterpret_cast<const char*>(
                  milo::milo_error_code_string(parser_));
          size_t error_code_len = strlen(error_code_string) + 6;
          char* error_code = Malloc(error_code_len);
          snprintf(error_code, error_code_len, "MILO_%s", error_code_string);
          code = OneByteString(environment->isolate(), error_code);
          free(error_code);
          milo::milo_free_string(
              reinterpret_cast<const unsigned char*>(error_code_string));
        }

        const unsigned char* errno_reason =
            milo::milo_error_description_string(parser_);
        reason = OneByteString(environment->isolate(), errno_reason);
        milo::milo_free_string(errno_reason);
      }

      obj->Set(environment->context(),
               environment->code_string(), code).Check();
      obj->Set(environment->context(),
               environment->reason_string(), reason).Check();

      return scope.Escape(e);
    }

    // No return value is needed for `Finish()`
    if (data == nullptr) {
      return scope.Escape(Local<Value>());
    }

    return scope.Escape(nread_obj);
  }


  Local<Array> CreateHeaders() {
    Isolate* isolate = env()->isolate();

    // There could be extra entries but the max size should be fixed
    Local<Value> headers_v[kMaxHeaderFieldsCount * 2];

    for (size_t i = 0; i < num_header_values_; ++i) {
      headers_v[i * 2] = header_fields_[i].ToString(isolate);
      headers_v[i * 2 + 1] = header_values_[i].ToTrimmedString(isolate);
    }

    return Array::New(env()->isolate(), headers_v, num_header_values_ * 2);
  }


  Local<Array> CreateTrailers() {
    Isolate* isolate = env()->isolate();

    // There could be extra entries but the max size should be fixed
    Local<Value> trailers_v[kMaxTrailerFieldsCount * 2];

    for (size_t i = 0; i < num_trailer_values_; ++i) {
      trailers_v[i * 2] = trailer_fields_[i].ToString(isolate);
      trailers_v[i * 2 + 1] = trailer_values_[i].ToTrimmedString(isolate);
    }

    return Array::New(env()->isolate(), trailers_v, num_trailer_values_ * 2);
  }


  // spill headers and request path to JS land
  void FlushHeaders() {
    HandleScope scope(env()->isolate());

    Local<Object> obj = object();
    Local<Value> cb = obj->Get(env()->context(), kOnHeaders).ToLocalChecked();

    if (!cb->IsFunction())
      return;

    Local<Value> argv[2] = {
      CreateHeaders(),
      url_.ToString(env()->isolate())
    };

    MaybeLocal<Value> r = MakeCallback(cb.As<Function>(),
                                       arraysize(argv),
                                       argv);

    if (r.IsEmpty())
      got_exception_ = true;

    url_.Reset();
    headers_flushed_ = true;
  }

  // spill trailers to JS land
  void FlushTrailers() {
    HandleScope scope(env()->isolate());

    Local<Object> obj = object();
    Local<Value> cb = obj->Get(env()->context(), kOnTrailers).ToLocalChecked();

    if (!cb->IsFunction())
      return;

    Local<Value> argv[1] = {
      CreateTrailers(),
    };

    MaybeLocal<Value> r = MakeCallback(cb.As<Function>(),
                                       arraysize(argv),
                                       argv);

    if (r.IsEmpty())
      got_exception_ = true;

    trailers_flushed_ = true;
  }

  void Init(intptr_t type, uint64_t max_http_header_size) {
    parser_ = milo::milo_create();
    heades_nread_ = 0;
    trailers_nread_ = 0;
    url_.Reset();
    status_message_.Reset();
    error_code_.Reset();
    error_reason_.Reset();
    num_header_fields_ = 0;
    num_header_values_ = 0;
    num_trailer_fields_ = 0;
    num_trailer_values_ = 0;
    headers_flushed_ = false;
    trailers_flushed_ = false;
    got_exception_ = false;
    headers_completed_ = false;
    trailers_completed_ = false;
    max_http_header_size_ = max_http_header_size;
    max_http_trailer_size_ = max_http_header_size;

    InitParser(type);
  }


  void InitParser(intptr_t type) {
    parser_->owner = this;
    parser_->mode = type;
    parser_->callbacks.on_message_start = CB<Parser, &Parser::on_message_start>;
    parser_->callbacks.on_url = CB<Parser, &Parser::on_url>;
    parser_->callbacks.on_reason = CB<Parser, &Parser::on_reason>;
    parser_->callbacks.on_header_name = CB<Parser, &Parser::on_header_name>;
    parser_->callbacks.on_header_value = CB<Parser, &Parser::on_header_value>;
    parser_->callbacks.on_headers = CB<Parser, &Parser::on_headers>;
    parser_->callbacks.on_data = CB<Parser, &Parser::on_body>;
    parser_->callbacks.on_trailer_name = CB<Parser, &Parser::on_trailer_name>;
    parser_->callbacks.on_trailer_value = CB<Parser, &Parser::on_trailer_value>;
    parser_->callbacks.on_trailers = CB<Parser, &Parser::on_trailers>;
    parser_->callbacks.on_message_complete =
        CB<Parser, &Parser::on_message_complete>;

    // Important - Do not remove the code below, only comment it out.
    // This enables to track Milo state change when using its debug version.
    // parser_->callbacks.after_state_change =
    //                              [](milo::Parser* p,
    //                                 const unsigned char* data,
    //                                 uintptr_t length) -> intptr_t {
    //   const unsigned char* state = milo::get_state_string(p);
    //   intptr_t type = p->message_type;

    //   printf(
    //     "%p[%s @ %lu] %s\n",
    //     reinterpret_cast<void*>(p),
    //     (type == milo::REQUEST
    //        ? "REQ"
    //        : (type == milo::RESPONSE ? "RES" : "---")),
    //     p->position,
    //     state
    //   );

    //   milo::milo_free_string(state);

    //   return 0;
    // };
    // parser_->callbacks.on_error =
    //                              [](milo::Parser* p,
    //                                 const unsigned char* data,
    //                                 uintptr_t length) -> intptr_t {
    //   const unsigned char* error_code_string =
    //       milo::get_error_code_string(p);
    //   const unsigned char* error_description_string =
    //       milo::get_error_description_string(p);
    //   intptr_t type = p->message_type;

    //   printf(
    //     "%p[%s @ %lu] ERROR %s (%u): %s\n",
    //     reinterpret_cast<void*>(p),
    //     (type == milo::REQUEST
    //        ? "REQ"
    //        : (type == milo::RESPONSE ? "RES" : "---")),
    //     p->position,
    //     error_code_string,
    //     static_cast<unsigned int>(p->error_code),
    //     error_description_string
    //   );

    //   milo::milo_free_string(error_code_string);
    //   milo::milo_free_string(error_description_string);

    //   return 0;
    // };
  }

  int TrackHeader(size_t len) {
    heades_nread_ += len;
    if (heades_nread_ >= max_http_header_size_) {
      error_code_.Set(
          reinterpret_cast<const unsigned char*>("HPE_HEADER_OVERFLOW"), 19);
      error_reason_.Set(
          reinterpret_cast<const unsigned char*>("Header overflow"), 15);
      return 1;
    }
    return 0;
  }

  int TrackTrailer(size_t len) {
    trailers_nread_ += len;
    if (trailers_nread_ >= max_http_trailer_size_) {
      error_code_.Set(
          reinterpret_cast<const unsigned char*>("HPE_HEADER_OVERFLOW"), 19);
      error_reason_.Set(
          reinterpret_cast<const unsigned char*>("Header overflow"), 15);
      return 1;
    }
    return 0;
  }


  bool IsNotIndicativeOfMemoryLeakAtExit() const override {
    // HTTP parsers are able to emit events without any GC root referring
    // to them, because they receive events directly from the underlying
    // libuv resource.
    return true;
  }


  milo::Parser* parser_;
  Data header_fields_[kMaxHeaderFieldsCount];  // header fields
  Data header_values_[kMaxHeaderFieldsCount];  // header values
  Data trailer_fields_[kMaxTrailerFieldsCount];  // trailer fields
  Data trailer_values_[kMaxTrailerFieldsCount];  // trailer values
  Data url_;
  Data status_message_;
  Data error_code_;
  Data error_reason_;
  size_t num_header_fields_;
  size_t num_header_values_;
  size_t num_trailer_fields_;
  size_t num_trailer_values_;
  bool headers_flushed_;
  bool trailers_flushed_;
  bool got_exception_;
  size_t current_buffer_len_;
  const char* current_buffer_data_;
  bool headers_completed_ = false;
  bool trailers_completed_ = false;
  uint64_t heades_nread_ = 0;
  uint64_t trailers_nread_ = 0;
  uint64_t max_http_header_size_;
  uint64_t max_http_trailer_size_;
  uint64_t last_message_start_;
  ConnectionsList* connectionsList_;

  BaseObjectPtr<BindingData> binding_data_;
};

bool ParserComparator::operator()(const Parser* lhs, const Parser* rhs) const {
  if (lhs->last_message_start_ == 0 && rhs->last_message_start_ == 0) {
    // When both parsers are idle, guarantee strict order by
    // comparing pointers as ints.
    return lhs < rhs;
  } else if (lhs->last_message_start_ == 0) {
    return true;
  } else if (rhs->last_message_start_ == 0) {
    return false;
  }

  return lhs->last_message_start_ < rhs->last_message_start_;
}

void ConnectionsList::New(const FunctionCallbackInfo<Value>& args) {
  Local<Context> context = args.GetIsolate()->GetCurrentContext();
  Environment* env = Environment::GetCurrent(context);

  new ConnectionsList(env, args.This());
}

void ConnectionsList::All(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  ConnectionsList* list;

  ASSIGN_OR_RETURN_UNWRAP(&list, args.Holder());

  std::vector<Local<Value>> result;
  result.reserve(list->all_connections_.size());
  for (auto parser : list->all_connections_) {
    result.emplace_back(parser->object());
  }

  return args.GetReturnValue().Set(
      Array::New(isolate, result.data(), result.size()));
}

void ConnectionsList::Idle(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  ConnectionsList* list;

  ASSIGN_OR_RETURN_UNWRAP(&list, args.Holder());

  std::vector<Local<Value>> result;
  result.reserve(list->all_connections_.size());
  for (auto parser : list->all_connections_) {
    if (parser->last_message_start_ == 0) {
      result.emplace_back(parser->object());
    }
  }

  return args.GetReturnValue().Set(
      Array::New(isolate, result.data(), result.size()));
}

void ConnectionsList::Active(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  ConnectionsList* list;

  ASSIGN_OR_RETURN_UNWRAP(&list, args.Holder());

  std::vector<Local<Value>> result;
  result.reserve(list->active_connections_.size());
  for (auto parser : list->active_connections_) {
    result.emplace_back(parser->object());
  }

  return args.GetReturnValue().Set(
      Array::New(isolate, result.data(), result.size()));
}

void ConnectionsList::Expired(const FunctionCallbackInfo<Value>& args) {
  Isolate* isolate = args.GetIsolate();

  ConnectionsList* list;

  ASSIGN_OR_RETURN_UNWRAP(&list, args.Holder());
  CHECK(args[0]->IsNumber());
  CHECK(args[1]->IsNumber());
  uint64_t headers_timeout =
    static_cast<uint64_t>(args[0].As<Uint32>()->Value()) * 1000000;
  uint64_t request_timeout =
    static_cast<uint64_t>(args[1].As<Uint32>()->Value()) * 1000000;

  if (headers_timeout == 0 && request_timeout == 0) {
    return args.GetReturnValue().Set(Array::New(isolate, 0));
  } else if (request_timeout > 0 && headers_timeout > request_timeout) {
    std::swap(headers_timeout, request_timeout);
  }

  // On IoT or embedded devices the uv_hrtime() may return the timestamp
  // that is smaller than configured timeout for headers or request
  // to prevent subtracting two unsigned integers
  // that can yield incorrect results we should check
  // if the 'now' is bigger than the timeout for headers or request
  const uint64_t now = uv_hrtime();
  const uint64_t headers_deadline =
      (headers_timeout > 0 && now > headers_timeout) ? now - headers_timeout
                                                     : 0;
  const uint64_t request_deadline =
      (request_timeout > 0 && now > request_timeout) ? now - request_timeout
                                                     : 0;

  if (headers_deadline == 0 && request_deadline == 0) {
    return args.GetReturnValue().Set(Array::New(isolate, 0));
  }

  auto iter = list->active_connections_.begin();
  auto end = list->active_connections_.end();

  std::vector<Local<Value>> result;
  result.reserve(list->active_connections_.size());
  while (iter != end) {
    Parser* parser = *iter;
    iter++;

    // Check for expiration.
    if (
      (!parser->headers_completed_ && headers_deadline > 0 &&
        parser->last_message_start_ < headers_deadline) ||
      (
        request_deadline > 0 &&
        parser->last_message_start_ < request_deadline)
    ) {
      result.emplace_back(parser->object());

      list->active_connections_.erase(parser);
    }
  }

  return args.GetReturnValue().Set(
      Array::New(isolate, result.data(), result.size()));
}

void InitializeHttpParser(Local<Object> target,
                          Local<Value> unused,
                          Local<Context> context,
                          void* priv) {
  Realm* realm = Realm::GetCurrent(context);
  Environment* env = realm->env();
  Isolate* isolate = env->isolate();
  BindingData* const binding_data = realm->AddBindingData<BindingData>(target);
  if (binding_data == nullptr) return;

  Local<FunctionTemplate> t = NewFunctionTemplate(isolate, Parser::New);
  t->InstanceTemplate()->SetInternalFieldCount(Parser::kInternalFieldCount);

  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "REQUEST"),
         Integer::New(env->isolate(), milo::REQUEST));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "RESPONSE"),
         Integer::New(env->isolate(), milo::RESPONSE));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnMessageBegin"),
         Integer::NewFromUnsigned(env->isolate(), kOnMessageBegin));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnHeaders"),
         Integer::NewFromUnsigned(env->isolate(), kOnHeaders));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnHeadersComplete"),
         Integer::NewFromUnsigned(env->isolate(), kOnHeadersComplete));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnBody"),
         Integer::NewFromUnsigned(env->isolate(), kOnBody));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnTrailers"),
         Integer::NewFromUnsigned(env->isolate(), kOnTrailers));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnTrailersComplete"),
         Integer::NewFromUnsigned(env->isolate(), kOnTrailersComplete));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnMessageComplete"),
         Integer::NewFromUnsigned(env->isolate(), kOnMessageComplete));
  t->Set(FIXED_ONE_BYTE_STRING(env->isolate(), "kOnExecute"),
         Integer::NewFromUnsigned(env->isolate(), kOnExecute));

  Local<Array> methods = Array::New(env->isolate());
#define V(num, name, string)                                                  \
    methods->Set(env->context(),                                              \
        num, FIXED_ONE_BYTE_STRING(env->isolate(), #string)).Check();
  MILO_METHODS_MAP(V)
#undef V
  target->Set(env->context(),
              FIXED_ONE_BYTE_STRING(env->isolate(), "methods"),
              methods).Check();

  t->Inherit(AsyncWrap::GetConstructorTemplate(env));
  SetProtoMethod(isolate, t, "close", Parser::Close);
  SetProtoMethod(isolate, t, "free", Parser::Free);
  SetProtoMethod(isolate, t, "remove", Parser::Remove);
  SetProtoMethod(isolate, t, "execute", Parser::Execute);
  SetProtoMethod(isolate, t, "finish", Parser::Finish);
  SetProtoMethod(isolate, t, "initialize", Parser::Initialize);
  SetProtoMethod(isolate, t, "pause", Parser::Pause<true>);
  SetProtoMethod(isolate, t, "resume", Parser::Pause<false>);
  SetProtoMethod(isolate, t, "reset", Parser::Reset);
  SetProtoMethod(isolate, t, "consume", Parser::Consume);
  SetProtoMethod(isolate, t, "unconsume", Parser::Unconsume);
  SetProtoMethod(isolate, t, "getCurrentBuffer", Parser::GetCurrentBuffer);
  SetProtoMethod(isolate, t, "duration", Parser::Duration);
  SetProtoMethod(isolate, t, "headersCompleted", Parser::HeadersCompleted);
  SetProtoMethod(isolate, t, "trailersCompleted", Parser::TrailersCompleted);

  SetConstructorFunction(context, target, "HTTPParser", t);

  Local<FunctionTemplate> c =
      NewFunctionTemplate(isolate, ConnectionsList::New);
  c->InstanceTemplate()
    ->SetInternalFieldCount(ConnectionsList::kInternalFieldCount);
  SetProtoMethod(isolate, c, "all", ConnectionsList::All);
  SetProtoMethod(isolate, c, "idle", ConnectionsList::Idle);
  SetProtoMethod(isolate, c, "active", ConnectionsList::Active);
  SetProtoMethod(isolate, c, "expired", ConnectionsList::Expired);
  SetConstructorFunction(context, target, "ConnectionsList", c);
}

}  // anonymous namespace
}  // namespace node

NODE_BINDING_CONTEXT_AWARE_INTERNAL(http_parser, node::InitializeHttpParser)
