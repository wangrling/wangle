/*
 *  Copyright (c) 2017, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include "Mocks.h"

using namespace folly;
using namespace testing;
using namespace wangle;

using TestServer = ServerBootstrap<DefaultPipeline>;
using TestClient = ClientBootstrap<DefaultPipeline>;

class TestClientPipelineFactory : public PipelineFactory<DefaultPipeline> {
 public:
  DefaultPipeline::Ptr newPipeline(
      std::shared_ptr<AsyncTransportWrapper> socket) override {
    // Socket should be connected already
    EXPECT_TRUE(socket->good());

    auto pipeline = DefaultPipeline::create();
    pipeline->addBack(wangle::AsyncSocketHandler(socket));
    pipeline->finalize();
    return pipeline;
  }
};

class AcceptRoutingHandlerTest : public Test {
 public:
  void SetUp() override {
    routingData_.routingData = 'A';

    downstreamHandler_ = new MockBytesToBytesHandler();
    downstreamPipelineFactory_ =
        std::make_shared<MockDownstreamPipelineFactory>(downstreamHandler_);

    server_ = std::make_unique<TestServer>();

    // A routing pipeline with a mock routing handler that we can set
    // expectations on.
    routingPipeline_ = DefaultPipeline::create();

    routingDataHandlerFactory_ =
        std::make_shared<MockRoutingDataHandlerFactory>();
    acceptRoutingHandler_ = new MockAcceptRoutingHandler(
        server_.get(),
        routingDataHandlerFactory_,
        downstreamPipelineFactory_,
        routingPipeline_);
    routingDataHandler_ =
        new MockRoutingDataHandler(kConnId0, acceptRoutingHandler_);
    routingDataHandlerFactory_->setRoutingDataHandler(routingDataHandler_);

    acceptPipeline_ = AcceptPipeline::create();
    acceptPipeline_->addBack(
        std::shared_ptr<MockAcceptRoutingHandler>(acceptRoutingHandler_));
    acceptPipeline_->finalize();

    // A single threaded IOGroup shared between client and server for a
    // deterministic event list.
    auto ioGroup = std::make_shared<IOThreadPoolExecutor>(kNumIOThreads);

    server_
        ->pipeline(std::make_shared<MockAcceptPipelineFactory>(acceptPipeline_))
        ->group(ioGroup, ioGroup)
        ->bind(0);
    server_->getSockets()[0]->getAddress(&address_);
    VLOG(4) << "Start server at " << address_;
  }

  EventBase* getEventBase() {
    return server_->getIOGroup()->getEventBase();
  }

  Future<DefaultPipeline*> clientConnect() {
    client_ = std::make_shared<TestClient>();
    client_->pipelineFactory(std::make_shared<TestClientPipelineFactory>());
    client_->group(server_->getIOGroup());
    return client_->connect(address_);
  }

  Future<DefaultPipeline*> clientConnectAndWrite() {
    auto clientPipelinePromise =
        std::make_shared<folly::Promise<DefaultPipeline*>>();

    getEventBase()->runInEventBaseThread([=]() {
      clientConnect().then([=](DefaultPipeline* clientPipeline) {
        VLOG(4) << "Client connected. Send data.";
        auto data = IOBuf::create(1);
        data->append(1);
        *(data->writableData()) = 'a';
        clientPipeline->write(std::move(data)).then([=]() {
          clientPipelinePromise->setValue(clientPipeline);
        });
      });
    });

    return clientPipelinePromise->getFuture();
  }

  Future<DefaultPipeline*> clientConnectAndCleanClose() {
    auto clientPipelinePromise =
        std::make_shared<folly::Promise<DefaultPipeline*>>();

    getEventBase()->runInEventBaseThread([=]() {
      clientConnectAndWrite().then([=](DefaultPipeline* clientPipeline) {
        VLOG(4) << "Client close";
        clientPipeline->close().then(
            [=]() { clientPipelinePromise->setValue(clientPipeline); });
      });
    });

    return clientPipelinePromise->getFuture();
  }

  Future<DefaultPipeline*> clientConnectWithException() {
    auto clientPipelinePromise =
        std::make_shared<folly::Promise<DefaultPipeline*>>();

    getEventBase()->runInEventBaseThread([=]() {
      clientConnect().then([=](DefaultPipeline* clientPipeline) {
        VLOG(4) << "Client connected. Induce an unclean close.";
        clientPipeline
            ->writeException(std::runtime_error(
                "Client socket exception, right after connect."))
            .then([=]() { clientPipelinePromise->setValue(clientPipeline); });
      });
    });

    return clientPipelinePromise->getFuture();
  }

  void TearDown() override {
    Mock::VerifyAndClear(downstreamHandler_);
    acceptPipeline_.reset();
    server_.reset();
  }

 protected:
  AcceptPipeline::Ptr acceptPipeline_;
  DefaultPipeline::Ptr routingPipeline_;
  std::shared_ptr<MockRoutingDataHandlerFactory> routingDataHandlerFactory_;
  MockRoutingDataHandler* routingDataHandler_;

  MockAcceptRoutingHandler* acceptRoutingHandler_;
  MockBytesToBytesHandler* downstreamHandler_;
  std::shared_ptr<MockDownstreamPipelineFactory> downstreamPipelineFactory_;
  std::unique_ptr<TestServer> server_;
  SocketAddress address_;
  RoutingDataHandler<char>::RoutingData routingData_;

  std::shared_ptr<TestClient> client_;

  int kConnId0{0};
  int kNumIOThreads{1};
};

TEST_F(AcceptRoutingHandlerTest, ParseRoutingDataSuccess) {
  // Server receives data, and parses routing data
  EXPECT_CALL(*routingDataHandler_, transportActive(_));
  EXPECT_CALL(*routingDataHandler_, parseRoutingData(_, _))
      .WillOnce(
          Invoke([&](folly::IOBufQueue& /*bufQueue*/,
                     MockRoutingDataHandler::RoutingData& /*routingData*/) {
            VLOG(4) << "Parsed routing data";
            return true;
          }));

  // Downstream pipeline is created, and its handler receives events
  boost::barrier barrier(2);
  EXPECT_CALL(*downstreamHandler_, transportActive(_));
  EXPECT_CALL(*downstreamHandler_, read(_, _))
      .WillOnce(Invoke([&](MockBytesToBytesHandler::Context* /*ctx*/,
                           IOBufQueue& /*bufQueue*/) {
        VLOG(4) << "Downstream received a read";
      }));
  EXPECT_CALL(*downstreamHandler_, readEOF(_))
      .WillOnce(Invoke([&](MockBytesToBytesHandler::Context* ctx) {
        VLOG(4) << "Downstream EOF";
        ctx->fireClose();
        barrier.wait();
      }));
  EXPECT_CALL(*downstreamHandler_, transportInactive(_));

  // Send client request that triggers server processing
  auto futureClientPipeline = clientConnectAndCleanClose();
  futureClientPipeline.wait();

  barrier.wait();
  VLOG(4) << "Stopping server";
  server_->stop();
  server_->join();

  // Routing pipeline has been erased
  EXPECT_EQ(0, acceptRoutingHandler_->getRoutingPipelineCount());
}

TEST_F(AcceptRoutingHandlerTest, SocketErrorInRoutingPipeline) {
  // Server receives data, and parses routing data
  boost::barrier barrierConnect(2);
  EXPECT_CALL(*routingDataHandler_, transportActive(_));
  EXPECT_CALL(*routingDataHandler_, parseRoutingData(_, _))
      .WillOnce(
          Invoke([&](folly::IOBufQueue& /*bufQueue*/,
                     MockRoutingDataHandler::RoutingData& /*routingData*/) {
            VLOG(4) << "Need more data to be parse.";
            barrierConnect.wait();
            return false;
          }));

  // Send client request that triggers server processing
  auto futureClientPipeline = clientConnectAndWrite();

  // Socket exception after routing pipeline had been created
  barrierConnect.wait();
  boost::barrier barrierException(2);
  futureClientPipeline.then([](DefaultPipeline* clientPipeline) {
    clientPipeline->getTransport()->getEventBase()->runInEventBaseThread(
        [clientPipeline]() {
          clientPipeline->writeException(
              std::runtime_error("Socket error while expecting routing data."));
        });
  });
  EXPECT_CALL(*routingDataHandler_, readException(_, _))
      .WillOnce(Invoke([&](MockBytesToBytesHandler::Context* /*ctx*/,
                           folly::exception_wrapper ex) {
        VLOG(4) << "Routing data handler Exception";
        acceptRoutingHandler_->onError(kConnId0, ex);
        barrierException.wait();
      }));
  barrierException.wait();

  // Downstream pipeline is not created
  EXPECT_CALL(*downstreamHandler_, transportActive(_)).Times(0);
  delete downstreamHandler_;

  VLOG(4) << "Stopping server";
  server_->stop();
  server_->join();

  // Routing pipeline has been erased
  EXPECT_EQ(0, acceptRoutingHandler_->getRoutingPipelineCount());
}

TEST_F(AcceptRoutingHandlerTest, OnNewConnectionWithBadSocket) {
  // Routing data handler doesn't receive any data
  EXPECT_CALL(*routingDataHandler_, transportActive(_)).Times(0);
  EXPECT_CALL(*routingDataHandler_, parseRoutingData(_, _)).Times(0);

  // Downstream pipeline is not created
  EXPECT_CALL(*downstreamHandler_, transportActive(_)).Times(0);
  delete downstreamHandler_;

  // Send client request that triggers server processing
  auto futureClientPipeline = clientConnectWithException();
  futureClientPipeline.wait();

  VLOG(4) << "Stopping server";
  server_->stop();
  server_->join();

  // Routing pipeline isn't added
  EXPECT_EQ(0, acceptRoutingHandler_->getRoutingPipelineCount());
}

TEST_F(AcceptRoutingHandlerTest, RoutingPipelineErasedOnlyOnce) {
  // Simulate client socket throwing an exception, while routing data handler
  // parsed data successfully.
  acceptPipeline_->readException(
      std::runtime_error("An exception from the socket."));
  acceptRoutingHandler_->onRoutingData(kConnId0, routingData_);
}
