#include "TcpServer.hpp"
#include <cassert>
#include <stdexcept>
snet::TcpServer::TcpServer(EventLoop &loop, Socket socket, TcpServerOptions options)
    : loop_(loop), options_(checked_options(options)), acceptor_(loop, std::move(socket), options_.acceptor),
      cleanup_(loop, this, &TcpServer::cleanup_entry)
{
    acceptor_.on_accept([this](Socket client) { accept(std::move(client)); });
    acceptor_.on_error([this](Acceptor &, std::error_code error) { accept_error(error); });
}
snet::TcpServer::~TcpServer() noexcept
{
    assert(!servicing_ && !callback_active_);
    stop();
}
void snet::TcpServer::on_connection(ConnectionCallback callback) { connection_callback_ = std::move(callback); }
void snet::TcpServer::on_accept_error(AcceptErrorCallback callback) { error_callback_ = std::move(callback); }
void snet::TcpServer::start()
{
    if (started_ || stopped_)
        throw std::logic_error("Server cannot be started twice or after stop");
    if (!connection_callback_)
        throw std::logic_error("Server requires connection configuration");
    acceptor_.start();
    started_ = true;
}
void snet::TcpServer::resume_accepting()
{
    if (stopped_)
        throw std::logic_error("Stopped listener cannot resume");
    if (!connection_callback_)
        throw std::logic_error("Server requires connection configuration");
    acceptor_.resume_accepting();
}
void snet::TcpServer::stop_accepting()
{
    if (stopped_)
        return;
    stopped_ = true;
    acceptor_.close();
}
void snet::TcpServer::close_connections() {}
void snet::TcpServer::stop()
{
    stop_accepting();
    close_connections();
}
snet::TcpServerOptions snet::TcpServer::checked_options(TcpServerOptions options)
{
    options.connection = TcpConnection::checked_options(options.connection);
    return options;
}
void snet::TcpServer::accept(Socket) {}
void snet::TcpServer::cleanup_entry(void *context, detail::CleanupReason reason) noexcept
{
    static_cast<TcpServer *>(context)->cleanup(reason);
}
void snet::TcpServer::cleanup(detail::CleanupReason) noexcept {}

void snet::TcpServer::accept_error(std::error_code) {}
