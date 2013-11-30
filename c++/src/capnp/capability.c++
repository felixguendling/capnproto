// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#define CAPNP_PRIVATE

#include "capability.h"
#include "capability-context.h"
#include "message.h"
#include "arena.h"
#include <kj/refcount.h>
#include <kj/debug.h>
#include <kj/vector.h>
#include <map>

namespace capnp {

Capability::Client::Client(decltype(nullptr))
    : hook(newBrokenCap("Called null capability.")) {}

Capability::Client::Client(kj::Exception&& exception)
    : hook(newBrokenCap(kj::mv(exception))) {}

kj::Promise<void> Capability::Server::internalUnimplemented(
    const char* actualInterfaceName, uint64_t requestedTypeId) {
  KJ_FAIL_REQUIRE("Requested interface not implemented.", actualInterfaceName, requestedTypeId) {
    // Recoverable exception will be caught by promise framework.

    // We can't "return kj::READY_NOW;" inside this block because it causes a memory leak due to
    // a bug that exists in both Clang and GCC:
    //   http://gcc.gnu.org/bugzilla/show_bug.cgi?id=33799
    //   http://llvm.org/bugs/show_bug.cgi?id=12286
    break;
  }
  return kj::READY_NOW;
}

kj::Promise<void> Capability::Server::internalUnimplemented(
    const char* interfaceName, uint64_t typeId, uint16_t methodId) {
  KJ_FAIL_REQUIRE("Method not implemented.", interfaceName, typeId, methodId) {
    // Recoverable exception will be caught by promise framework.
    break;
  }
  return kj::READY_NOW;
}

kj::Promise<void> Capability::Server::internalUnimplemented(
    const char* interfaceName, const char* methodName, uint64_t typeId, uint16_t methodId) {
  KJ_FAIL_REQUIRE("Method not implemented.", interfaceName, typeId, methodName, methodId) {
    // Recoverable exception will be caught by promise framework.
    break;
  }
  return kj::READY_NOW;
}

ResponseHook::~ResponseHook() noexcept(false) {}

kj::Promise<void> ClientHook::whenResolved() {
  KJ_IF_MAYBE(promise, whenMoreResolved()) {
    return promise->then([](kj::Own<ClientHook>&& resolution) {
      return resolution->whenResolved();
    });
  } else {
    return kj::READY_NOW;
  }
}

// =======================================================================================

class LocalResponse final: public ResponseHook, public kj::Refcounted {
public:
  LocalResponse(uint sizeHint)
      : message(sizeHint == 0 ? SUGGESTED_FIRST_SEGMENT_WORDS : sizeHint) {}

  LocalMessage message;
};

class LocalCallContext final: public CallContextHook, public kj::Refcounted {
public:
  LocalCallContext(kj::Own<LocalMessage>&& request, kj::Own<ClientHook> clientRef,
                   kj::Own<kj::PromiseFulfiller<void>> cancelAllowedFulfiller)
      : request(kj::mv(request)), clientRef(kj::mv(clientRef)),
        cancelAllowedFulfiller(kj::mv(cancelAllowedFulfiller)) {}

  ObjectPointer::Reader getParams() override {
    KJ_IF_MAYBE(r, request) {
      return r->get()->getRoot();
    } else {
      KJ_FAIL_REQUIRE("Can't call getParams() after releaseParams().");
    }
  }
  void releaseParams() override {
    request = nullptr;
  }
  ObjectPointer::Builder getResults(uint firstSegmentWordSize) override {
    if (response == nullptr) {
      auto localResponse = kj::refcounted<LocalResponse>(firstSegmentWordSize);
      responseBuilder = localResponse->message.getRoot();
      response = Response<ObjectPointer>(responseBuilder.asReader(), kj::mv(localResponse));
    }
    return responseBuilder;
  }
  kj::Promise<void> tailCall(kj::Own<RequestHook>&& request) override {
    auto result = directTailCall(kj::mv(request));
    KJ_IF_MAYBE(f, tailCallPipelineFulfiller) {
      f->get()->fulfill(ObjectPointer::Pipeline(kj::mv(result.pipeline)));
    }
    return kj::mv(result.promise);
  }
  ClientHook::VoidPromiseAndPipeline directTailCall(kj::Own<RequestHook>&& request) override {
    KJ_REQUIRE(response == nullptr, "Can't call tailCall() after initializing the results struct.");
    releaseParams();

    auto promise = request->send();

    auto voidPromise = promise.then([this](Response<ObjectPointer>&& tailResponse) {
      response = kj::mv(tailResponse);
    });

    return { kj::mv(voidPromise), PipelineHook::from(kj::mv(promise)) };
  }
  kj::Promise<ObjectPointer::Pipeline> onTailCall() override {
    auto paf = kj::newPromiseAndFulfiller<ObjectPointer::Pipeline>();
    tailCallPipelineFulfiller = kj::mv(paf.fulfiller);
    return kj::mv(paf.promise);
  }
  void allowAsyncCancellation() override {
    KJ_REQUIRE(request == nullptr, "Must call releaseParams() before allowAsyncCancellation().");
    cancelAllowedFulfiller->fulfill();
  }
  bool isCanceled() override {
    return cancelRequested;
  }
  kj::Own<CallContextHook> addRef() override {
    return kj::addRef(*this);
  }

  kj::Maybe<kj::Own<LocalMessage>> request;
  kj::Maybe<Response<ObjectPointer>> response;
  ObjectPointer::Builder responseBuilder = nullptr;  // only valid if `response` is non-null
  kj::Own<ClientHook> clientRef;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<ObjectPointer::Pipeline>>> tailCallPipelineFulfiller;
  kj::Own<kj::PromiseFulfiller<void>> cancelAllowedFulfiller;
  bool cancelRequested = false;

  class Canceler {
  public:
    Canceler(kj::Own<LocalCallContext>&& context): context(kj::mv(context)) {}
    Canceler(Canceler&&) = default;

    ~Canceler() {
      if (context) context->cancelRequested = true;
    }

  private:
    kj::Own<LocalCallContext> context;
  };
};

class LocalRequest final: public RequestHook {
public:
  inline LocalRequest(uint64_t interfaceId, uint16_t methodId,
                      uint firstSegmentWordSize, kj::Own<ClientHook> client)
      : message(kj::heap<LocalMessage>(
            firstSegmentWordSize == 0 ? SUGGESTED_FIRST_SEGMENT_WORDS : firstSegmentWordSize)),
        interfaceId(interfaceId), methodId(methodId), client(kj::mv(client)) {}

  RemotePromise<ObjectPointer> send() override {
    KJ_REQUIRE(message.get() != nullptr, "Already called send() on this request.");

    // For the lambda capture.
    uint64_t interfaceId = this->interfaceId;
    uint16_t methodId = this->methodId;

    auto cancelPaf = kj::newPromiseAndFulfiller<void>();

    auto context = kj::refcounted<LocalCallContext>(
        kj::mv(message), client->addRef(), kj::mv(cancelPaf.fulfiller));
    auto promiseAndPipeline = client->call(interfaceId, methodId, kj::addRef(*context));

    // We have to make sure the call is not canceled unless permitted.  We need to fork the promise
    // so that if the client drops their copy, the promise isn't necessarily canceled.
    auto forked = promiseAndPipeline.promise.fork();

    // We daemonize one branch, but only after joining it with the promise that fires if
    // cancellation is allowed.
    auto daemonPromise = forked.addBranch();
    daemonPromise.attach(kj::addRef(*context));
    daemonPromise.exclusiveJoin(kj::mv(cancelPaf.promise));
    // Daemonize, ignoring exceptions.
    kj::daemonize(kj::mv(daemonPromise), [](kj::Exception&&) {});

    // Now the other branch returns the response from the context.
    auto contextPtr = context.get();
    auto promise = forked.addBranch().then([contextPtr]() {
      contextPtr->getResults(1);  // force response allocation
      return kj::mv(KJ_ASSERT_NONNULL(contextPtr->response));
    });

    // We also want to notify the context that cancellation was requested if this branch is
    // destroyed.
    promise.attach(LocalCallContext::Canceler(kj::mv(context)));

    // We return the other branch.
    return RemotePromise<ObjectPointer>(
        kj::mv(promise), ObjectPointer::Pipeline(kj::mv(promiseAndPipeline.pipeline)));
  }

  const void* getBrand() override {
    return nullptr;
  }

  kj::Own<LocalMessage> message;

private:
  uint64_t interfaceId;
  uint16_t methodId;
  kj::Own<ClientHook> client;
};

// =======================================================================================
// Call queues
//
// These classes handle pipelining in the case where calls need to be queued in-memory until some
// local operation completes.

class QueuedPipeline final: public PipelineHook, public kj::Refcounted {
  // A PipelineHook which simply queues calls while waiting for a PipelineHook to which to forward
  // them.

public:
  QueuedPipeline(kj::Promise<kj::Own<PipelineHook>>&& promiseParam)
      : promise(promiseParam.fork()),
        selfResolutionOp(promise.addBranch().then(
            [this](kj::Own<PipelineHook>&& inner) {
              redirect = kj::mv(inner);
            })) {
    selfResolutionOp.eagerlyEvaluate();
  }

  kj::Own<PipelineHook> addRef() override {
    return kj::addRef(*this);
  }

  kj::Own<ClientHook> getPipelinedCap(kj::ArrayPtr<const PipelineOp> ops) override {
    auto copy = kj::heapArrayBuilder<PipelineOp>(ops.size());
    for (auto& op: ops) {
      copy.add(op);
    }
    return getPipelinedCap(copy.finish());
  }

  kj::Own<ClientHook> getPipelinedCap(kj::Array<PipelineOp>&& ops) override;

private:
  kj::ForkedPromise<kj::Own<PipelineHook>> promise;

  kj::Maybe<kj::Own<PipelineHook>> redirect;
  // Once the promise resolves, this will become non-null and point to the underlying object.

  kj::Promise<void> selfResolutionOp;
  // Represents the operation which will set `redirect` when possible.
};

class QueuedClient final: public ClientHook, public kj::Refcounted {
  // A ClientHook which simply queues calls while waiting for a ClientHook to which to forward
  // them.

public:
  QueuedClient(kj::Promise<kj::Own<ClientHook>>&& promiseParam)
      : promise(promiseParam.fork()),
        selfResolutionOp(promise.addBranch().then(
            [this](kj::Own<ClientHook>&& inner) {
              redirect = kj::mv(inner);
            })),
        promiseForCallForwarding(promise.addBranch().fork()),
        promiseForClientResolution(promise.addBranch().fork()) {
    selfResolutionOp.eagerlyEvaluate();
  }

  Request<ObjectPointer, ObjectPointer> newCall(
      uint64_t interfaceId, uint16_t methodId, uint firstSegmentWordSize) override {
    auto hook = kj::heap<LocalRequest>(
        interfaceId, methodId, firstSegmentWordSize, kj::addRef(*this));
    auto root = hook->message->getRoot();  // Do not inline `root` -- kj::mv may happen first.
    return Request<ObjectPointer, ObjectPointer>(root, kj::mv(hook));
  }

  VoidPromiseAndPipeline call(uint64_t interfaceId, uint16_t methodId,
                              kj::Own<CallContextHook>&& context) override {
    // This is a bit complicated.  We need to initiate this call later on.  When we initiate the
    // call, we'll get a void promise for its completion and a pipeline object.  Right now, we have
    // to produce a similar void promise and pipeline that will eventually be chained to those.
    // The problem is, these are two independent objects, but they both depend on the result of
    // one future call.
    //
    // So, we need to set up a continuation that will initiate the call later, then we need to
    // fork the promise for that continuation in order to send the completion promise and the
    // pipeline to their respective places.
    //
    // TODO(perf):  Too much reference counting?  Can we do better?  Maybe a way to fork
    //   Promise<Tuple<T, U>> into Tuple<Promise<T>, Promise<U>>?

    struct CallResultHolder: public kj::Refcounted {
      // Essentially acts as a refcounted \VoidPromiseAndPipeline, so that we can create a promise
      // for it and fork that promise.

      VoidPromiseAndPipeline content;
      // One branch of the fork will use content.promise, the other branch will use
      // content.pipeline.  Neither branch will touch the other's piece.

      inline CallResultHolder(VoidPromiseAndPipeline&& content): content(kj::mv(content)) {}

      kj::Own<CallResultHolder> addRef() { return kj::addRef(*this); }
    };

    // Create a promise for the call initiation.
    kj::ForkedPromise<kj::Own<CallResultHolder>> callResultPromise =
        promiseForCallForwarding.addBranch().then(kj::mvCapture(context,
        [=](kj::Own<CallContextHook>&& context, kj::Own<ClientHook>&& client){
          return kj::refcounted<CallResultHolder>(
              client->call(interfaceId, methodId, kj::mv(context)));
        })).fork();

    // Create a promise that extracts the pipeline from the call initiation, and construct our
    // QueuedPipeline to chain to it.
    auto pipelinePromise = callResultPromise.addBranch().then(
        [](kj::Own<CallResultHolder>&& callResult){
          return kj::mv(callResult->content.pipeline);
        });
    auto pipeline = kj::refcounted<QueuedPipeline>(kj::mv(pipelinePromise));

    // Create a promise that simply chains to the void promise produced by the call initiation.
    auto completionPromise = callResultPromise.addBranch().then(
        [](kj::Own<CallResultHolder>&& callResult){
          return kj::mv(callResult->content.promise);
        });

    // OK, now we can actually return our thing.
    return VoidPromiseAndPipeline { kj::mv(completionPromise), kj::mv(pipeline) };
  }

  kj::Maybe<ClientHook&> getResolved() override {
    KJ_IF_MAYBE(inner, redirect) {
      return **inner;
    } else {
      return nullptr;
    }
  }

  kj::Maybe<kj::Promise<kj::Own<ClientHook>>> whenMoreResolved() override {
    return promiseForClientResolution.addBranch();
  }

  kj::Own<ClientHook> addRef() override {
    return kj::addRef(*this);
  }

  const void* getBrand() override {
    return nullptr;
  }

private:
  typedef kj::ForkedPromise<kj::Own<ClientHook>> ClientHookPromiseFork;

  kj::Maybe<kj::Own<ClientHook>> redirect;
  // Once the promise resolves, this will become non-null and point to the underlying object.

  ClientHookPromiseFork promise;
  // Promise that resolves when we have a new ClientHook to forward to.
  //
  // This fork shall only have three branches:  `selfResolutionOp`, `promiseForCallForwarding`, and
  // `promiseForClientResolution`, in that order.

  kj::Promise<void> selfResolutionOp;
  // Represents the operation which will set `redirect` when possible.

  ClientHookPromiseFork promiseForCallForwarding;
  // When this promise resolves, each queued call will be forwarded to the real client.  This needs
  // to occur *before* any 'whenMoreResolved()' promises resolve, because we want to make sure
  // previously-queued calls are delivered before any new calls made in response to the resolution.

  ClientHookPromiseFork promiseForClientResolution;
  // whenMoreResolved() returns forks of this promise.  These must resolve *after* queued calls
  // have been initiated (so that any calls made in the whenMoreResolved() handler are correctly
  // delivered after calls made earlier), but *before* any queued calls return (because it might
  // confuse the application if a queued call returns before the capability on which it was made
  // resolves).  Luckily, we know that queued calls will involve, at the very least, an
  // eventLoop.evalLater.
};

kj::Own<ClientHook> QueuedPipeline::getPipelinedCap(kj::Array<PipelineOp>&& ops) {
  KJ_IF_MAYBE(r, redirect) {
    return r->get()->getPipelinedCap(kj::mv(ops));
  } else {
    auto clientPromise = promise.addBranch().then(kj::mvCapture(ops,
        [](kj::Array<PipelineOp>&& ops, kj::Own<PipelineHook> pipeline) {
          return pipeline->getPipelinedCap(kj::mv(ops));
        }));

    return kj::refcounted<QueuedClient>(kj::mv(clientPromise));
  }
}

// =======================================================================================

class LocalPipeline final: public PipelineHook, public kj::Refcounted {
public:
  inline LocalPipeline(kj::Own<CallContextHook>&& contextParam)
      : context(kj::mv(contextParam)),
        results(context->getResults(1)) {}

  kj::Own<PipelineHook> addRef() {
    return kj::addRef(*this);
  }

  kj::Own<ClientHook> getPipelinedCap(kj::ArrayPtr<const PipelineOp> ops) {
    return results.getPipelinedCap(ops);
  }

private:
  kj::Own<CallContextHook> context;
  ObjectPointer::Reader results;
};

class LocalClient final: public ClientHook, public kj::Refcounted {
public:
  LocalClient(kj::Own<Capability::Server>&& server)
      : server(kj::mv(server)) {}

  Request<ObjectPointer, ObjectPointer> newCall(
      uint64_t interfaceId, uint16_t methodId, uint firstSegmentWordSize) override {
    auto hook = kj::heap<LocalRequest>(
        interfaceId, methodId, firstSegmentWordSize, kj::addRef(*this));
    auto root = hook->message->getRoot();  // Do not inline `root` -- kj::mv may happen first.
    return Request<ObjectPointer, ObjectPointer>(root, kj::mv(hook));
  }

  VoidPromiseAndPipeline call(uint64_t interfaceId, uint16_t methodId,
                              kj::Own<CallContextHook>&& context) override {
    auto contextPtr = context.get();

    // We don't want to actually dispatch the call synchronously, because:
    // 1) The server may prefer a different EventLoop.
    // 2) If the server is in the same EventLoop, calling it synchronously could be dangerous due
    //    to risk of deadlocks if it happens to take a mutex that the client already holds.  One
    //    of the main goals of message-passing architectures is to avoid this!
    //
    // So, we do an evalLater() here.
    //
    // Note also that QueuedClient depends on this evalLater() to ensure that pipelined calls don't
    // complete before 'whenMoreResolved()' promises resolve.
    auto promise = kj::evalLater([this,interfaceId,methodId,contextPtr]() {
      return server->dispatchCall(interfaceId, methodId,
                                  CallContext<ObjectPointer, ObjectPointer>(*contextPtr));
    });

    // Make sure that this client cannot be destroyed until the promise completes.
    promise.attach(kj::addRef(*this));

    // We have to fork this promise for the pipeline to receive a copy of the answer.
    auto forked = promise.fork();

    auto pipelinePromise = forked.addBranch().then(kj::mvCapture(context->addRef(),
        [=](kj::Own<CallContextHook>&& context) -> kj::Own<PipelineHook> {
          context->releaseParams();
          return kj::refcounted<LocalPipeline>(kj::mv(context));
        }));

    auto tailPipelinePromise = context->onTailCall().then([](ObjectPointer::Pipeline&& pipeline) {
      return kj::mv(pipeline.hook);
    });

    pipelinePromise.exclusiveJoin(kj::mv(tailPipelinePromise));

    auto completionPromise = forked.addBranch();
    completionPromise.attach(kj::mv(context));

    return VoidPromiseAndPipeline { kj::mv(completionPromise),
        kj::refcounted<QueuedPipeline>(kj::mv(pipelinePromise)) };
  }

  kj::Maybe<ClientHook&> getResolved() override {
    return nullptr;
  }

  kj::Maybe<kj::Promise<kj::Own<ClientHook>>> whenMoreResolved() override {
    return nullptr;
  }

  kj::Own<ClientHook> addRef() override {
    return kj::addRef(*this);
  }

  const void* getBrand() override {
    // We have no need to detect local objects.
    return nullptr;
  }

private:
  kj::Own<Capability::Server> server;
};

kj::Own<ClientHook> Capability::Client::makeLocalClient(kj::Own<Capability::Server>&& server) {
  return kj::refcounted<LocalClient>(kj::mv(server));
}

kj::Own<ClientHook> newLocalPromiseClient(kj::Promise<kj::Own<ClientHook>>&& promise) {
  return kj::refcounted<QueuedClient>(kj::mv(promise));
}

}  // namespace capnp
