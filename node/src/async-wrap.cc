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

#include "async-wrap.h"
#include "async-wrap-inl.h"
#include "env.h"
#include "env-inl.h"
#include "util.h"
#include "util-inl.h"

#include "uv.h"
#include "v8.h"
#include "v8-profiler.h"

using v8::Array;
using v8::ArrayBuffer;
using v8::Context;
using v8::Float64Array;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::HandleScope;
using v8::HeapProfiler;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::MaybeLocal;
using v8::Number;
using v8::Object;
using v8::Promise;
using v8::PromiseHookType;
using v8::RetainedObjectInfo;
using v8::String;
using v8::Symbol;
using v8::TryCatch;
using v8::Uint32Array;
using v8::Value;

using AsyncHooks = node::Environment::AsyncHooks;

namespace node {

static const char* const provider_names[] = {
#define V(PROVIDER)                                                           \
  #PROVIDER,
  NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
};


// Report correct information in a heapdump.

class RetainedAsyncInfo: public RetainedObjectInfo {
 public:
  explicit RetainedAsyncInfo(uint16_t class_id, AsyncWrap* wrap);

  void Dispose() override;
  bool IsEquivalent(RetainedObjectInfo* other) override;
  intptr_t GetHash() override;
  const char* GetLabel() override;
  intptr_t GetSizeInBytes() override;

 private:
  const char* label_;
  const AsyncWrap* wrap_;
  const int length_;
};


RetainedAsyncInfo::RetainedAsyncInfo(uint16_t class_id, AsyncWrap* wrap)
    : label_(provider_names[class_id - NODE_ASYNC_ID_OFFSET]),
      wrap_(wrap),
      length_(wrap->self_size()) {
}


void RetainedAsyncInfo::Dispose() {
  delete this;
}


bool RetainedAsyncInfo::IsEquivalent(RetainedObjectInfo* other) {
  return label_ == other->GetLabel() &&
          wrap_ == static_cast<RetainedAsyncInfo*>(other)->wrap_;
}


intptr_t RetainedAsyncInfo::GetHash() {
  return reinterpret_cast<intptr_t>(wrap_);
}


const char* RetainedAsyncInfo::GetLabel() {
  return label_;
}


intptr_t RetainedAsyncInfo::GetSizeInBytes() {
  return length_;
}


RetainedObjectInfo* WrapperInfo(uint16_t class_id, Local<Value> wrapper) {
  // No class_id should be the provider type of NONE.
  CHECK_GT(class_id, NODE_ASYNC_ID_OFFSET);
  // And make sure the class_id doesn't extend past the last provider.
  CHECK_LE(class_id - NODE_ASYNC_ID_OFFSET, AsyncWrap::PROVIDERS_LENGTH);
  CHECK(wrapper->IsObject());
  CHECK(!wrapper.IsEmpty());

  Local<Object> object = wrapper.As<Object>();
  CHECK_GT(object->InternalFieldCount(), 0);

  AsyncWrap* wrap = Unwrap<AsyncWrap>(object);
  CHECK_NE(nullptr, wrap);

  return new RetainedAsyncInfo(class_id, wrap);
}


// end RetainedAsyncInfo


static void DestroyIdsCb(uv_timer_t* handle) {
  Environment* env = Environment::from_destroy_ids_timer_handle(handle);

  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());
  Local<Function> fn = env->async_hooks_destroy_function();

  TryCatch try_catch(env->isolate());

  do {
    std::vector<double> destroy_ids_list;
    destroy_ids_list.swap(*env->destroy_ids_list());
    for (auto current_id : destroy_ids_list) {
      // Want each callback to be cleaned up after itself, instead of cleaning
      // them all up after the while() loop completes.
      HandleScope scope(env->isolate());
      Local<Value> argv = Number::New(env->isolate(), current_id);
      MaybeLocal<Value> ret = fn->Call(
          env->context(), Undefined(env->isolate()), 1, &argv);

      if (ret.IsEmpty()) {
        ClearFatalExceptionHandlers(env);
        FatalException(env->isolate(), try_catch);
      }
    }
  } while (!env->destroy_ids_list()->empty());
}


static void PushBackDestroyId(Environment* env, double id) {
  if (env->async_hooks()->fields()[AsyncHooks::kDestroy] == 0)
    return;

  if (env->destroy_ids_list()->empty())
    uv_timer_start(env->destroy_ids_timer_handle(), DestroyIdsCb, 0, 0);

  env->destroy_ids_list()->push_back(id);
}


bool DomainEnter(Environment* env, Local<Object> object) {
  Local<Value> domain_v = object->Get(env->domain_string());
  if (domain_v->IsObject()) {
    Local<Object> domain = domain_v.As<Object>();
    if (domain->Get(env->disposed_string())->IsTrue())
      return true;
    Local<Value> enter_v = domain->Get(env->enter_string());
    if (enter_v->IsFunction()) {
      if (enter_v.As<Function>()->Call(domain, 0, nullptr).IsEmpty()) {
        FatalError("node::AsyncWrap::MakeCallback",
                   "domain enter callback threw, please report this");
      }
    }
  }
  return false;
}


bool DomainExit(Environment* env, v8::Local<v8::Object> object) {
  Local<Value> domain_v = object->Get(env->domain_string());
  if (domain_v->IsObject()) {
    Local<Object> domain = domain_v.As<Object>();
    if (domain->Get(env->disposed_string())->IsTrue())
      return true;
    Local<Value> exit_v = domain->Get(env->exit_string());
    if (exit_v->IsFunction()) {
      if (exit_v.As<Function>()->Call(domain, 0, nullptr).IsEmpty()) {
        FatalError("node::AsyncWrap::MakeCallback",
                  "domain exit callback threw, please report this");
      }
    }
  }
  return false;
}


static bool PreCallbackExecution(AsyncWrap* wrap, bool run_domain_cbs) {
  if (wrap->env()->using_domains() && run_domain_cbs) {
    bool is_disposed = DomainEnter(wrap->env(), wrap->object());
    if (is_disposed)
      return false;
  }

  return AsyncWrap::EmitBefore(wrap->env(), wrap->get_id());
}


bool AsyncWrap::EmitBefore(Environment* env, double async_id) {
  AsyncHooks* async_hooks = env->async_hooks();

  if (async_hooks->fields()[AsyncHooks::kBefore] > 0) {
    Local<Value> uid = Number::New(env->isolate(), async_id);
    Local<Function> fn = env->async_hooks_before_function();
    TryCatch try_catch(env->isolate());
    MaybeLocal<Value> ar = fn->Call(
        env->context(), Undefined(env->isolate()), 1, &uid);
    if (ar.IsEmpty()) {
      ClearFatalExceptionHandlers(env);
      FatalException(env->isolate(), try_catch);
      return false;
    }
  }

  return true;
}


static bool PostCallbackExecution(AsyncWrap* wrap, bool run_domain_cbs) {
  if (!AsyncWrap::EmitAfter(wrap->env(), wrap->get_id()))
    return false;

  if (wrap->env()->using_domains() && run_domain_cbs) {
    bool is_disposed = DomainExit(wrap->env(), wrap->object());
    if (is_disposed)
      return false;
  }

  return true;
}

bool AsyncWrap::EmitAfter(Environment* env, double async_id) {
  AsyncHooks* async_hooks = env->async_hooks();

  // If the callback failed then the after() hooks will be called at the end
  // of _fatalException().
  if (async_hooks->fields()[AsyncHooks::kAfter] > 0) {
    Local<Value> uid = Number::New(env->isolate(), async_id);
    Local<Function> fn = env->async_hooks_after_function();
    TryCatch try_catch(env->isolate());
    MaybeLocal<Value> ar = fn->Call(
        env->context(), Undefined(env->isolate()), 1, &uid);
    if (ar.IsEmpty()) {
      ClearFatalExceptionHandlers(env);
      FatalException(env->isolate(), try_catch);
      return false;
    }
  }

  return true;
}

class PromiseWrap : public AsyncWrap {
 public:
  PromiseWrap(Environment* env, Local<Object> object, bool silent)
    : AsyncWrap(env, object, PROVIDER_PROMISE, silent) {}
  size_t self_size() const override { return sizeof(*this); }
};


static void PromiseHook(PromiseHookType type, Local<Promise> promise,
                        Local<Value> parent, void* arg) {
  Local<Context> context = promise->CreationContext();
  Environment* env = Environment::GetCurrent(context);
  PromiseWrap* wrap = Unwrap<PromiseWrap>(promise);
  if (type == PromiseHookType::kInit || wrap == nullptr) {
    bool silent = type != PromiseHookType::kInit;
    // set parent promise's async Id as this promise's triggerId
    if (parent->IsPromise()) {
      // parent promise exists, current promise
      // is a chained promise, so we set parent promise's id as
      // current promise's triggerId
      Local<Promise> parent_promise = parent.As<Promise>();
      auto parent_wrap = Unwrap<PromiseWrap>(parent_promise);

      if (parent_wrap == nullptr) {
        // create a new PromiseWrap for parent promise with silent parameter
        parent_wrap = new PromiseWrap(env, parent_promise, true);
        parent_wrap->MakeWeak(parent_wrap);
      }
      // get id from parentWrap
      double trigger_id = parent_wrap->get_id();
      env->set_init_trigger_id(trigger_id);
    }
    wrap = new PromiseWrap(env, promise, silent);
    wrap->MakeWeak(wrap);
  } else if (type == PromiseHookType::kResolve) {
    // TODO(matthewloring): need to expose this through the async hooks api.
  }
  CHECK_NE(wrap, nullptr);
  if (type == PromiseHookType::kBefore) {
    PreCallbackExecution(wrap, false);
  } else if (type == PromiseHookType::kAfter) {
    PostCallbackExecution(wrap, false);
  }
}


static void SetupHooks(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  if (!args[0]->IsObject())
    return env->ThrowTypeError("first argument must be an object");

  // All of init, before, after, destroy are supplied by async_hooks
  // internally, so this should every only be called once. At which time all
  // the functions should be set. Detect this by checking if init !IsEmpty().
  CHECK(env->async_hooks_init_function().IsEmpty());

  Local<Object> fn_obj = args[0].As<Object>();

#define SET_HOOK_FN(name)                                                     \
  Local<Value> name##_v = fn_obj->Get(                                        \
      env->context(),                                                         \
      FIXED_ONE_BYTE_STRING(env->isolate(), #name)).ToLocalChecked();         \
  CHECK(name##_v->IsFunction());                                              \
  env->set_async_hooks_##name##_function(name##_v.As<Function>());

  SET_HOOK_FN(init);
  SET_HOOK_FN(before);
  SET_HOOK_FN(after);
  SET_HOOK_FN(destroy);
  env->AddPromiseHook(PromiseHook, nullptr);
#undef SET_HOOK_FN
}


void AsyncWrap::GetAsyncId(const FunctionCallbackInfo<Value>& args) {
  AsyncWrap* wrap;
  args.GetReturnValue().Set(-1);
  ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());
  args.GetReturnValue().Set(wrap->get_id());
}


void AsyncWrap::PushAsyncIds(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  // No need for CHECK(IsNumber()) on args because if FromJust() doesn't fail
  // then the checks in push_ids() and pop_ids() will.
  double async_id = args[0]->NumberValue(env->context()).FromJust();
  double trigger_id = args[1]->NumberValue(env->context()).FromJust();
  env->async_hooks()->push_ids(async_id, trigger_id);
}


void AsyncWrap::PopAsyncIds(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  double async_id = args[0]->NumberValue(env->context()).FromJust();
  args.GetReturnValue().Set(env->async_hooks()->pop_ids(async_id));
}


void AsyncWrap::ClearIdStack(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  env->async_hooks()->clear_id_stack();
}


void AsyncWrap::AsyncReset(const FunctionCallbackInfo<Value>& args) {
  AsyncWrap* wrap;
  ASSIGN_OR_RETURN_UNWRAP(&wrap, args.Holder());
  wrap->AsyncReset();
}


void AsyncWrap::QueueDestroyId(const FunctionCallbackInfo<Value>& args) {
  CHECK(args[0]->IsNumber());
  PushBackDestroyId(Environment::GetCurrent(args), args[0]->NumberValue());
}


void AsyncWrap::Initialize(Local<Object> target,
                           Local<Value> unused,
                           Local<Context> context) {
  Environment* env = Environment::GetCurrent(context);
  Isolate* isolate = env->isolate();
  HandleScope scope(isolate);

  env->SetMethod(target, "setupHooks", SetupHooks);
  env->SetMethod(target, "pushAsyncIds", PushAsyncIds);
  env->SetMethod(target, "popAsyncIds", PopAsyncIds);
  env->SetMethod(target, "clearIdStack", ClearIdStack);
  env->SetMethod(target, "addIdToDestroyList", QueueDestroyId);

  v8::PropertyAttribute ReadOnlyDontDelete =
      static_cast<v8::PropertyAttribute>(v8::ReadOnly | v8::DontDelete);

#define FORCE_SET_TARGET_FIELD(obj, str, field)                               \
  (obj)->ForceSet(context,                                                    \
                  FIXED_ONE_BYTE_STRING(isolate, str),                        \
                  field,                                                      \
                  ReadOnlyDontDelete).FromJust()

  // Attach the uint32_t[] where each slot contains the count of the number of
  // callbacks waiting to be called on a particular event. It can then be
  // incremented/decremented from JS quickly to communicate to C++ if there are
  // any callbacks waiting to be called.
  uint32_t* fields_ptr = env->async_hooks()->fields();
  int fields_count = env->async_hooks()->fields_count();
  Local<ArrayBuffer> fields_ab =
      ArrayBuffer::New(isolate, fields_ptr, fields_count * sizeof(*fields_ptr));
  FORCE_SET_TARGET_FIELD(target,
                         "async_hook_fields",
                         Uint32Array::New(fields_ab, 0, fields_count));

  // The following v8::Float64Array has 5 fields. These fields are shared in
  // this way to allow JS and C++ to read/write each value as quickly as
  // possible. The fields are represented as follows:
  //
  // kAsyncUid: Maintains the state of the next unique id to be assigned.
  //
  // kInitTriggerId: Write the id of the resource responsible for a handle's
  //   creation just before calling the new handle's constructor. After the new
  //   handle is constructed kInitTriggerId is set back to 0.
  double* uid_fields_ptr = env->async_hooks()->uid_fields();
  int uid_fields_count = env->async_hooks()->uid_fields_count();
  Local<ArrayBuffer> uid_fields_ab = ArrayBuffer::New(
      isolate,
      uid_fields_ptr,
      uid_fields_count * sizeof(*uid_fields_ptr));
  FORCE_SET_TARGET_FIELD(target,
                         "async_uid_fields",
                         Float64Array::New(uid_fields_ab, 0, uid_fields_count));

  Local<Object> constants = Object::New(isolate);
#define SET_HOOKS_CONSTANT(name)                                              \
  FORCE_SET_TARGET_FIELD(                                                     \
      constants, #name, Integer::New(isolate, AsyncHooks::name));

  SET_HOOKS_CONSTANT(kInit);
  SET_HOOKS_CONSTANT(kBefore);
  SET_HOOKS_CONSTANT(kAfter);
  SET_HOOKS_CONSTANT(kDestroy);
  SET_HOOKS_CONSTANT(kCurrentAsyncId);
  SET_HOOKS_CONSTANT(kCurrentTriggerId);
  SET_HOOKS_CONSTANT(kAsyncUidCntr);
  SET_HOOKS_CONSTANT(kInitTriggerId);
#undef SET_HOOKS_CONSTANT
  FORCE_SET_TARGET_FIELD(target, "constants", constants);

  Local<Object> async_providers = Object::New(isolate);
#define V(p)                                                                  \
  FORCE_SET_TARGET_FIELD(                                                     \
      async_providers, #p, Integer::New(isolate, AsyncWrap::PROVIDER_ ## p));
  NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
  FORCE_SET_TARGET_FIELD(target, "Providers", async_providers);

  // These Symbols are used throughout node so the stored values on each object
  // can be accessed easily across files.
  FORCE_SET_TARGET_FIELD(
      target,
      "async_id_symbol",
      Symbol::New(isolate, FIXED_ONE_BYTE_STRING(isolate, "asyncId")));
  FORCE_SET_TARGET_FIELD(
      target,
      "trigger_id_symbol",
      Symbol::New(isolate, FIXED_ONE_BYTE_STRING(isolate, "triggerId")));

#undef FORCE_SET_TARGET_FIELD

  env->set_async_hooks_init_function(Local<Function>());
  env->set_async_hooks_before_function(Local<Function>());
  env->set_async_hooks_after_function(Local<Function>());
  env->set_async_hooks_destroy_function(Local<Function>());
}


void LoadAsyncWrapperInfo(Environment* env) {
  HeapProfiler* heap_profiler = env->isolate()->GetHeapProfiler();
#define V(PROVIDER)                                                           \
  heap_profiler->SetWrapperClassInfoProvider(                                 \
      (NODE_ASYNC_ID_OFFSET + AsyncWrap::PROVIDER_ ## PROVIDER), WrapperInfo);
  NODE_ASYNC_PROVIDER_TYPES(V)
#undef V
}


AsyncWrap::AsyncWrap(Environment* env,
                     Local<Object> object,
                     ProviderType provider,
                     bool silent)
    : BaseObject(env, object),
      provider_type_(provider) {
  CHECK_NE(provider, PROVIDER_NONE);
  CHECK_GE(object->InternalFieldCount(), 1);

  // Shift provider value over to prevent id collision.
  persistent().SetWrapperClassId(NODE_ASYNC_ID_OFFSET + provider);

  // Use AsyncReset() call to execute the init() callbacks.
  AsyncReset(silent);
}


AsyncWrap::~AsyncWrap() {
  PushBackDestroyId(env(), get_id());
}


// Generalized call for both the constructor and for handles that are pooled
// and reused over their lifetime. This way a new uid can be assigned when
// the resource is pulled out of the pool and put back into use.
void AsyncWrap::AsyncReset(bool silent) {
  async_id_ = env()->new_async_id();
  trigger_id_ = env()->get_init_trigger_id();

  if (silent) return;

  EmitAsyncInit(env(), object(),
                env()->async_hooks()->provider_string(provider_type()),
                async_id_, trigger_id_);
}


void AsyncWrap::EmitAsyncInit(Environment* env,
                              Local<Object> object,
                              Local<String> type,
                              double async_id,
                              double trigger_id) {
  AsyncHooks* async_hooks = env->async_hooks();

  // Nothing to execute, so can continue normally.
  if (async_hooks->fields()[AsyncHooks::kInit] == 0) {
    return;
  }

  HandleScope scope(env->isolate());
  Local<Function> init_fn = env->async_hooks_init_function();

  Local<Value> argv[] = {
    Number::New(env->isolate(), async_id),
    type,
    Number::New(env->isolate(), trigger_id),
    object,
  };

  TryCatch try_catch(env->isolate());
  MaybeLocal<Value> ret = init_fn->Call(
      env->context(), object, arraysize(argv), argv);

  if (ret.IsEmpty()) {
    ClearFatalExceptionHandlers(env);
    FatalException(env->isolate(), try_catch);
  }
}


Local<Value> AsyncWrap::MakeCallback(const Local<Function> cb,
                                     int argc,
                                     Local<Value>* argv) {
  CHECK(env()->context() == env()->isolate()->GetCurrentContext());

  Environment::AsyncCallbackScope callback_scope(env());

  Environment::AsyncHooks::ExecScope exec_scope(env(),
                                                get_id(),
                                                get_trigger_id());

  if (!PreCallbackExecution(this, true)) {
    return Local<Value>();
  }

  // Finally... Get to running the user's callback.
  MaybeLocal<Value> ret = cb->Call(env()->context(), object(), argc, argv);

  Local<Value> ret_v;
  if (!ret.ToLocal(&ret_v)) {
    return Local<Value>();
  }

  if (!PostCallbackExecution(this, true)) {
    return Local<Value>();
  }

  exec_scope.Dispose();

  if (callback_scope.in_makecallback()) {
    return ret_v;
  }

  Environment::TickInfo* tick_info = env()->tick_info();

  if (tick_info->length() == 0) {
    env()->isolate()->RunMicrotasks();
  }

  // Make sure the stack unwound properly. If there are nested MakeCallback's
  // then it should return early and not reach this code.
  CHECK_EQ(env()->current_async_id(), 0);
  CHECK_EQ(env()->trigger_id(), 0);

  Local<Object> process = env()->process_object();

  if (tick_info->length() == 0) {
    tick_info->set_index(0);
    return ret_v;
  }

  MaybeLocal<Value> rcheck =
      env()->tick_callback_function()->Call(env()->context(),
                                            process,
                                            0,
                                            nullptr);

  // Make sure the stack unwound properly.
  CHECK_EQ(env()->current_async_id(), 0);
  CHECK_EQ(env()->trigger_id(), 0);

  return rcheck.IsEmpty() ? Local<Value>() : ret_v;
}


/* Public C++ embedder API */


async_uid AsyncHooksGetCurrentId(Isolate* isolate) {
  return Environment::GetCurrent(isolate)->current_async_id();
}


async_uid AsyncHooksGetTriggerId(Isolate* isolate) {
  return Environment::GetCurrent(isolate)->get_init_trigger_id();
}


async_uid EmitAsyncInit(Isolate* isolate,
                     Local<Object> resource,
                     const char* name,
                     async_uid trigger_id) {
  Environment* env = Environment::GetCurrent(isolate);
  async_uid async_id = env->new_async_id();

  Local<String> type =
      String::NewFromUtf8(isolate, name, v8::NewStringType::kInternalized)
          .ToLocalChecked();
  AsyncWrap::EmitAsyncInit(env, resource, type, async_id, trigger_id);
  return async_id;
}

void EmitAsyncDestroy(Isolate* isolate, async_uid id) {
  PushBackDestroyId(Environment::GetCurrent(isolate), id);
}

}  // namespace node

NODE_MODULE_CONTEXT_AWARE_BUILTIN(async_wrap, node::AsyncWrap::Initialize)
