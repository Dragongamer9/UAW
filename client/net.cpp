#include "net.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SockLen = int;
#define UAW_CLOSE closesocket
#define UAW_WOULDBLOCK (WSAGetLastError() == WSAEWOULDBLOCK)
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SockLen = socklen_t;
#define UAW_CLOSE ::close
#define UAW_WOULDBLOCK (errno == EAGAIN || errno == EWOULDBLOCK)
#endif

namespace uaw::net {
namespace {

using Raw = decltype(socket(0, 0, 0));

bool startup() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA d;
        if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
        done = true;
    }
#endif
    return true;
}

void nonBlocking(long long fd) {
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(SOCKET(fd), FIONBIO, &on);
#else
    fcntl(int(fd), F_SETFL, fcntl(int(fd), F_GETFL, 0) | O_NONBLOCK);
#endif
    int one = 1;
    setsockopt(Raw(fd), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
}

std::string lastError() {
#ifdef _WIN32
    return "socket error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

} // namespace

Socket::~Socket() { close(); }

Socket::Socket(Socket&& o) noexcept : fd_(o.fd_), in_(std::move(o.in_)), out_(std::move(o.out_)) { o.fd_ = -1; }

Socket& Socket::operator=(Socket&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        in_ = std::move(o.in_);
        out_ = std::move(o.out_);
        o.fd_ = -1;
    }
    return *this;
}

void Socket::close() {
    if (fd_ >= 0) {
        flush();
        UAW_CLOSE(Raw(fd_));
    }
    fd_ = -1;
}

Socket Socket::listen(int port, std::string* error) {
    if (!startup()) {
        if (error) *error = "network start-up failed";
        return Socket();
    }
    auto fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (static_cast<long long>(fd) < 0) {
        if (error) *error = lastError();
        return Socket();
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(uint16_t(port));
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 8) != 0) {
        if (error) *error = lastError();
        UAW_CLOSE(fd);
        return Socket();
    }
    nonBlocking(static_cast<long long>(fd));
    return Socket(static_cast<long long>(fd));
}

Socket Socket::connect(const std::string& host, int port, std::string* error) {
    if (!startup()) {
        if (error) *error = "network start-up failed";
        return Socket();
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        if (error) *error = "unknown host " + host;
        return Socket();
    }
    auto fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (static_cast<long long>(fd) < 0 || ::connect(fd, res->ai_addr, SockLen(res->ai_addrlen)) != 0) {
        if (error) *error = "can't reach " + host + ":" + std::to_string(port) + " (" + lastError() + ")";
        if (static_cast<long long>(fd) >= 0) UAW_CLOSE(fd);
        freeaddrinfo(res);
        return Socket();
    }
    freeaddrinfo(res);
    nonBlocking(static_cast<long long>(fd));
    return Socket(static_cast<long long>(fd));
}

Socket Socket::accept() {
    if (fd_ < 0) return Socket();
    auto c = ::accept(Raw(fd_), nullptr, nullptr);
    if (static_cast<long long>(c) < 0) return Socket();
    nonBlocking(static_cast<long long>(c));
    return Socket(static_cast<long long>(c));
}

bool Socket::send(const std::string& line) {
    if (fd_ < 0) return false;
    out_ += line;
    out_ += '\n';
    flush();
    return fd_ >= 0;
}

void Socket::flush() {
    while (fd_ >= 0 && !out_.empty()) {
        const auto n = ::send(Raw(fd_), out_.data(), int(out_.size()), 0);
        if (n > 0) {
            out_.erase(0, size_t(n));
            continue;
        }
        if (n < 0 && UAW_WOULDBLOCK) break;
        UAW_CLOSE(Raw(fd_));
        fd_ = -1;
    }
}

bool Socket::receive(std::vector<std::string>& lines) {
    bool open = fd_ >= 0;
    char buf[8192];
    while (fd_ >= 0) {
        const auto n = ::recv(Raw(fd_), buf, int(sizeof buf), 0);
        if (n > 0) {
            in_.append(buf, size_t(n));
            continue;
        }
        if (n < 0 && UAW_WOULDBLOCK) break;
        UAW_CLOSE(Raw(fd_)); // closed or failed
        fd_ = -1;
        open = false;
    }
    size_t start = 0, nl;
    while ((nl = in_.find('\n', start)) != std::string::npos) {
        std::string l = in_.substr(start, nl - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        start = nl + 1;
    }
    in_.erase(0, start);
    flush();
    return open;
}

} // namespace uaw::net
