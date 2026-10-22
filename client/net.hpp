// Minimal non-blocking TCP for LAN games: line-based messages over BSD sockets / Winsock.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace uaw::net {

constexpr int kDefaultPort = 7470;

// One connection (or a listening socket). Non-blocking; lines end with '\n'.
class Socket {
public:
    Socket() = default;
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& o) noexcept;
    Socket& operator=(Socket&& o) noexcept;

    static Socket listen(int port, std::string* error = nullptr);
    static Socket connect(const std::string& host, int port, std::string* error = nullptr); // blocking, with a timeout
    Socket accept();            // a waiting connection, or an invalid socket
    bool valid() const { return fd_ >= 0; }
    bool send(const std::string& line); // queues line + '\n' and flushes what it can
    void flush();
    // Complete lines received so far. False once the peer has gone (lines still returned).
    bool receive(std::vector<std::string>& lines);
    void close();

private:
    explicit Socket(long long fd) : fd_(fd) {}
    long long fd_ = -1;
    std::string in_, out_;
};

} // namespace uaw::net
