#pragma once
#include "Acceptor.hpp"
#include "TcpServer.hpp"

/**
 * @file Simplenet.hpp
 * @brief Umbrella header for the SimpleNet public API.
 */

/**
 * @namespace snet
 * @brief Socket addresses and single-threaded Reactor components for Linux.
 * @see docs/reactor.md
 */

#include "Address.hpp"
#include "Buffer.hpp"
#include "Channel.hpp"
#include "EventLoop.hpp"
#include "Poller.hpp"

#include "Socket.hpp"

#include "TcpConnection.hpp"
