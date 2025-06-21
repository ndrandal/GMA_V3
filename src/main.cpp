// File: src/main.cpp
#include "gma/AtomicStore.hpp"
#include "gma/ThreadPool.hpp"
#include "gma/ExecutionContext.hpp"
#include "gma/MarketDispatcher.hpp"
#include "gma/Config.hpp"
#include "gma/server/WebSocketServer.hpp"
#include <boost/asio.hpp>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    // Default port for WebSocketServer
    unsigned short port = 9002;
    if (argc > 1) {
        try {
            port = static_cast<unsigned short>(std::stoul(argv[1]));
        } catch (const std::exception& ex) {
            std::cerr << "Invalid port '" << argv[1] << "': " << ex.what() << std::endl;
            return EXIT_FAILURE;
        }
    }

    // Core components
    gma::AtomicStore store;
    gma::ThreadPool pool(gma::Config::ThreadPoolSize);
    gma::ExecutionContext ctx(&store, &pool);
    gma::MarketDispatcher dispatcher(&pool, &store);

    // Launch server
    boost::asio::io_context ioc;
    gma::WebSocketServer server(ioc, &ctx, &dispatcher, port);
    server.run();

    std::cout << "gma WebSocket server listening on port " << port << std::endl;
    ioc.run();

    return EXIT_SUCCESS;
}
