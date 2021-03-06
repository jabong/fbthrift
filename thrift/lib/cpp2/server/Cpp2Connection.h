/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements. See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership. The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied. See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef THRIFT_ASYNC_CPP2CONNECTION_H_
#define THRIFT_ASYNC_CPP2CONNECTION_H_ 1

#include <thrift/lib/cpp/async/HHWheelTimer.h>
#include <thrift/lib/cpp/async/TEventConnection.h>
#include <thrift/lib/cpp/concurrency/Util.h>
#include <thrift/lib/cpp/transport/TSocketAddress.h>
#include <thrift/lib/cpp/TApplicationException.h>
#include <thrift/lib/cpp2/async/HeaderServerChannel.h>
#include <thrift/lib/cpp2/async/SaslServer.h>
#include <thrift/lib/cpp2/server/Cpp2ConnContext.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>
#include <thrift/lib/cpp2/server/Cpp2Worker.h>
#include <thrift/lib/cpp2/async/DuplexChannel.h>
#include <memory>
#include <unordered_set>

namespace apache { namespace thrift {
/**
 * Represents a connection that is handled via libevent. This connection
 * essentially encapsulates a socket that has some associated libevent state.
 */
class Cpp2Connection
    : public ResponseChannel::Callback
    , public apache::thrift::async::HHWheelTimer::Callback
    , public std::enable_shared_from_this<Cpp2Connection> {
 public:

  static const std::string loadHeader;
  /**
   * Constructor for Cpp2Connection.
   *
   * @param asyncSocket shared pointer to the async socket
   * @param address the peer address of this connection
   * @param worker the worker instance that is handling this connection
   * @param serverChannel server channel to use in duplex mode,
   *        should be nullptr in normal mode
   */
  Cpp2Connection(
    const std::shared_ptr<apache::thrift::async::TAsyncSocket>& asyncSocket,
      const apache::thrift::transport::TSocketAddress* address,
      Cpp2Worker* worker,
      const std::shared_ptr<HeaderServerChannel>& serverChannel = nullptr);

  /// Destructor -- close down the connection.
  ~Cpp2Connection();

  // ResponseChannel callbacks
  void requestReceived(std::unique_ptr<ResponseChannel::Request>&&);
  void channelClosed(folly::exception_wrapper&&);

  void start() {
    channel_->setCallback(this);
  }

  void stop();

  void timeoutExpired() noexcept;

  void requestTimeoutExpired();

  bool pending();

 protected:
  std::unique_ptr<apache::thrift::AsyncProcessor> processor_;
  std::unique_ptr<DuplexChannel> duplexChannel_;
  std::shared_ptr<apache::thrift::HeaderServerChannel> channel_;
  Cpp2Worker* worker_;
  Cpp2Worker* getWorker() {
    return worker_;
  }
  Cpp2ConnContext context_;

  std::shared_ptr<apache::thrift::async::TAsyncSocket> socket_;

  /**
   * Wrap the request in our own request.  This is done for 2 reasons:
   * a) To have task timeouts for all requests,
   * b) To ensure the channel is not destroyed before callback is called
   */
  class Cpp2Request
      : public ResponseChannel::Request
      , public apache::thrift::async::HHWheelTimer::Callback {
   public:
    friend class Cpp2Connection;

    Cpp2Request(std::unique_ptr<ResponseChannel::Request> req,
                   std::shared_ptr<Cpp2Connection> con);

    // Delegates to wrapped request.
    virtual bool isActive() { return req_->isActive(); }
    virtual void cancel() { req_->cancel(); }

    virtual bool isOneway() { return req_->isOneway(); }

    virtual void sendReply(std::unique_ptr<folly::IOBuf>&& buf,
                   MessageChannel::SendCallback* notUsed = nullptr);
    virtual void sendErrorWrapped(
        folly::exception_wrapper ew,
        std::string exCode,
        MessageChannel::SendCallback* notUsed = nullptr);
    virtual void timeoutExpired() noexcept;

    virtual ~Cpp2Request();

    // Cancel request is ususally called from a different thread than sendReply.
    virtual void cancelRequest();

    Cpp2RequestContext* getContext() {
      return &reqContext_;
    }

    virtual apache::thrift::server::TServerObserver::CallTimestamps&
    getTimestamps() {
      return req_->getTimestamps();
    }

   private:
    MessageChannel::SendCallback* prepareSendCallback(
        MessageChannel::SendCallback* sendCallback,
        apache::thrift::server::TServerObserver* observer);

    std::unique_ptr<HeaderServerChannel::HeaderRequest> req_;
    std::shared_ptr<Cpp2Connection> connection_;
    Cpp2RequestContext reqContext_;
  };

  class Cpp2Sample
      : public MessageChannel::SendCallback {
   public:
    Cpp2Sample(
      apache::thrift::server::TServerObserver::CallTimestamps&& timestamps,
      apache::thrift::server::TServerObserver* observer,
      MessageChannel::SendCallback* chainedCallback = nullptr);

    void sendQueued();
    void messageSent();
    void messageSendError(folly::exception_wrapper&& e);
    ~Cpp2Sample();

   private:
    apache::thrift::server::TServerObserver::CallTimestamps timestamps_;
    apache::thrift::server::TServerObserver* observer_;
    MessageChannel::SendCallback* chainedCallback_;
  };

  std::unordered_set<Cpp2Request*> activeRequests_;

  void removeRequest(Cpp2Request* req);
  void killRequest(ResponseChannel::Request& req,
                   TApplicationException::TApplicationExceptionType reason,
                   const char* comment);
  void disconnect(const char* comment) noexcept;

  // Set any error headers necessary, based on the received headers
  apache::thrift::transport::THeader::StringToStringMap setErrorHeaders(
    const apache::thrift::transport::THeader::StringToStringMap&
    recv_headers);

  friend class Cpp2Request;

  std::weak_ptr<Cpp2Connection> weakptr_;
};

}} // apache::thrift

#endif // #ifndef THRIFT_ASYNC_CPP2CONNECTION_H_
