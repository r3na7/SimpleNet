#include "TcpServer.hpp"
#include <cassert>
#include <limits>
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
    connections_.clear();
    closed_ = nullptr;
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
void snet::TcpServer::close_connections()
{
    for (auto &[id, entry] : connections_)
        entry.connection->close();
}
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
void snet::TcpServer::accept(Socket socket)
{
    assert(!servicing_);
    struct Guard {
        bool &flag;
        explicit Guard(bool &f) : flag(f) { flag = true; }
        ~Guard() { flag = false; }
    } guard(servicing_);
    if (next_id_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Connection identifiers exhausted");
    auto connection = std::make_unique<TcpConnection>(loop_, std::move(socket), options_.connection);
    const auto id = next_id_++;
    auto [position, inserted] = connections_.try_emplace(id, *this, id, std::move(connection));
    assert(inserted);
    auto &entry = position->second;
    auto &client = *entry.connection;
    client.owner_context_ = &entry;
    client.owner_closed_ = &TcpServer::mark_closed;
    connection_callback_(client);
    if (client.state_ != TcpConnection::State::closed)
        client.start();
}
void snet::TcpServer::cleanup_entry(void *context, detail::CleanupReason reason) noexcept
{
    static_cast<TcpServer *>(context)->cleanup(reason);
}
void snet::TcpServer::mark_closed(void *context, TcpConnection &connection) noexcept
{
    auto &entry = *static_cast<OwnedConnection *>(context);
    assert(entry.connection.get() == &connection);
    if (entry.marked)
        return;
    entry.marked = true;
    entry.next_closed = entry.server->closed_;
    entry.server->closed_ = &entry;
}
void snet::TcpServer::cleanup(detail::CleanupReason) noexcept
{
    auto **link = &closed_;
    while (auto *entry = *link) {
        if (!entry->connection->ready_for_cleanup()) {
            link = &entry->next_closed;
            continue;
        }
        *link = entry->next_closed;
        const auto id = entry->id;
        connections_.erase(id);
    }
}

void snet::TcpServer::accept_error(std::error_code) {}
