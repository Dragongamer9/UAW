#include "lockstep.hpp"
#include "savegame.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace uaw {

namespace {

// "host:port" or "host".
void splitAddress(const std::string& a, std::string& host, int& port) {
    host = a;
    port = net::kDefaultPort;
    if (const size_t c = a.rfind(':'); c != std::string::npos) {
        host = a.substr(0, c);
        port = std::atoi(a.c_str() + c + 1);
    }
    if (host.empty()) host = "127.0.0.1";
}

std::string word(const std::string& line, size_t& pos) {
    while (pos < line.size() && line[pos] == ' ') ++pos;
    const size_t start = pos;
    while (pos < line.size() && line[pos] != ' ') ++pos;
    return line.substr(start, pos - start);
}

} // namespace

std::unique_ptr<Lockstep> Lockstep::host(int port, std::string& error) {
    auto l = std::unique_ptr<Lockstep>(new Lockstep());
    l->host_ = true;
    l->id_ = 0;
    l->listener_ = net::Socket::listen(port, &error);
    if (!l->listener_.valid()) return nullptr;
    l->names_[0] = "Host";
    l->status_ = "Hosting on port " + std::to_string(port);
    return l;
}

std::unique_ptr<Lockstep> Lockstep::join(const std::string& address, const std::string& name, std::string& error) {
    std::string host;
    int port;
    splitAddress(address, host, port);
    auto l = std::unique_ptr<Lockstep>(new Lockstep());
    l->name_ = name.empty() ? "Player" : name;
    l->server_ = net::Socket::connect(host, port, &error);
    if (!l->server_.valid()) return nullptr;
    l->server_.send("HELLO 1 " + l->name_);
    l->status_ = "Connected to " + host + ", waiting for the host";
    return l;
}

bool Lockstep::connected() const { return host_ || server_.valid(); }

std::vector<std::pair<int, std::string>> Lockstep::players() const {
    std::vector<std::pair<int, std::string>> out(names_.begin(), names_.end());
    return out;
}

std::set<int> Lockstep::alivePlayers() const {
    std::set<int> out{0};
    for (const Peer& p : peers_)
        if (p.alive) out.insert(p.id);
    return out;
}

bool Lockstep::pollLobby(SkirmishSetup& setup) {
    bool changed = false;
    if (host_) {
        // New players: an open slot each (or they wait if there is none).
        for (net::Socket s = listener_.accept(); s.valid(); s = listener_.accept()) {
            Peer p;
            p.id = nextId_++;
            p.socket = std::move(s);
            peers_.push_back(std::move(p));
        }
        for (Peer& p : peers_) {
            if (!p.alive) continue;
            std::vector<std::string> lines;
            const bool open = p.socket.receive(lines);
            for (const std::string& l : lines) {
                if (l.rfind("HELLO ", 0) == 0) {
                    size_t pos = 6;
                    word(l, pos);
                    p.name = l.size() > pos ? l.substr(pos + 1) : "Player";
                    names_[p.id] = p.name;
                    p.socket.send("WELCOME " + std::to_string(p.id));
                    for (PlayerSlot& slot : setup.slots)
                        if (slot.kind == PlayerSlot::Kind::Open) {
                            slot.kind = PlayerSlot::Kind::Human;
                            slot.netId = p.id;
                            changed = true;
                            break;
                        }
                    status_ = p.name + " joined";
                }
            }
            if (!open) {
                p.alive = false;
                names_.erase(p.id);
                status_ = (p.name.empty() ? "A player" : p.name) + " left";
                for (PlayerSlot& slot : setup.slots)
                    if (slot.kind == PlayerSlot::Kind::Human && slot.netId == p.id) {
                        slot.kind = PlayerSlot::Kind::Open;
                        slot.netId = -1;
                        changed = true;
                    }
            }
        }
        // Everyone sees the current setup and who is there.
        const std::string line = encodeSetup(setup);
        if (line != setupLine_ || changed) {
            setupLine_ = line;
            std::string who = "PLAYERS";
            for (const auto& [id, n] : names_) who += " " + std::to_string(id) + ":" + n;
            for (Peer& p : peers_)
                if (p.alive) {
                    p.socket.send(who);
                    p.socket.send("SETUP " + line);
                }
            changed = true;
        }
        return changed;
    }
    std::vector<std::string> lines;
    if (!server_.receive(lines)) {
        problem_ = status_ = "The connection to the host was lost";
    }
    for (const std::string& l : lines) {
        if (l.rfind("WELCOME ", 0) == 0) {
            id_ = std::atoi(l.c_str() + 8);
        } else if (l.rfind("SETUP ", 0) == 0) {
            SkirmishSetup s;
            if (decodeSetup(l.substr(6), s)) {
                setup = s;
                setupLine_ = l.substr(6);
                changed = true;
            }
        } else if (l.rfind("PLAYERS", 0) == 0) {
            names_.clear();
            std::istringstream ls(l.substr(7));
            std::string item;
            while (ls >> item)
                if (const size_t c = item.find(':'); c != std::string::npos) names_[std::atoi(item.c_str())] = item.substr(c + 1);
        } else if (l == "START") {
            started_ = true;
        } else {
            handle(0, l); // game messages can follow START in the same read
        }
    }
    return changed;
}

void Lockstep::start(const SkirmishSetup& setup) {
    if (!host_) return;
    setupLine_ = encodeSetup(setup);
    for (Peer& p : peers_)
        if (p.alive) {
            p.socket.send("SETUP " + setupLine_);
            p.socket.send("START");
        }
    started_ = true;
}

void Lockstep::submit(const std::vector<alamo::Command>& orders, uint64_t tick) {
    if (orders.empty()) return;
    const uint64_t turn = tick / kTurnTicks + kDelayTurns;
    for (const alamo::Command& c : orders) mine_[turn].push_back(c);
}

bool Lockstep::canRun(uint64_t tick) {
    const uint64_t turn = tick / kTurnTicks;
    return turn < uint64_t(kDelayTurns) || known_.count(turn) != 0;
}

std::vector<alamo::Command> Lockstep::beginTurn(uint64_t tick, uint64_t stateHash) {
    const uint64_t turn = tick / kTurnTicks;
    static const bool log = std::getenv("UAW_NET_LOG") != nullptr;
    if (log) std::fprintf(stderr, "[net %d] begin turn %llu (tick %llu)\n", id_, (unsigned long long)turn, (unsigned long long)tick);
    // The player can no longer add orders to turn + kDelayTurns - 1: send them, and close it.
    const uint64_t closing = turn + kDelayTurns - 1;
    std::vector<alamo::Command> orders;
    if (auto it = mine_.find(closing); it != mine_.end()) {
        orders = std::move(it->second);
        mine_.erase(it);
    }
    if (host_) {
        for (const alamo::Command& c : orders) gathered_[closing].push_back({0, encodeCommand(c)});
        hostClose(0, closing, tick, stateHash);
    } else {
        for (const alamo::Command& c : orders) server_.send("O " + std::to_string(closing) + " " + encodeCommand(c));
        server_.send("DONE " + std::to_string(closing) + " " + std::to_string(tick) + " " + std::to_string(stateHash));
    }
    // This turn's orders (known, or canRun would have said no).
    std::vector<alamo::Command> out;
    if (auto it = known_.find(turn); it != known_.end()) out = std::move(it->second); // kept: canRun() asks again for its other ticks
    known_.erase(known_.begin(), known_.lower_bound(turn));                            // earlier turns are done
    return out;
}

void Lockstep::hostClose(int player, uint64_t turn, uint64_t tick, uint64_t hash) {
    closed_[turn].insert(player);
    auto& h = hashes_[tick];
    h[player] = hash;
    // Everyone alive reported this moment: compare.
    const std::set<int> alive = alivePlayers();
    bool all = true;
    for (int p : alive) all = all && h.count(p);
    if (all) {
        for (const auto& [p, v] : h)
            if (alive.count(p) && v != h.begin()->second && problem_.empty()) {
                problem_ = "Out of sync at " + std::to_string(tick / 30) + " s: the games differ";
                for (Peer& peer : peers_)
                    if (peer.alive) peer.socket.send("DESYNC " + std::to_string(tick));
            }
        hashes_.erase(tick);
    }
    hostRelease();
}

void Lockstep::hostRelease() {
    // Send out every turn that all players still connected have closed, in turn order.
    const std::set<int> alive = alivePlayers();
    while (!closed_.empty()) {
        auto it = closed_.begin();
        for (int p : alive)
            if (!it->second.count(p)) return;
        const uint64_t turn = it->first;
        std::vector<std::pair<int, std::string>> list = std::move(gathered_[turn]);
        gathered_.erase(turn);
        // Same order everywhere: by player, then as given.
        std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<alamo::Command> orders;
        for (const auto& [p, text] : list) {
            alamo::Command c;
            if (decodeCommand(text, c)) orders.push_back(c);
        }
        for (Peer& peer : peers_) {
            if (!peer.alive) continue;
            for (const auto& [p, text] : list) peer.socket.send("C " + std::to_string(turn) + " " + text);
            peer.socket.send("GO " + std::to_string(turn));
        }
        known_[turn] = std::move(orders);
        closed_.erase(it);
    }
}

void Lockstep::handle(int from, const std::string& l) {
    static const bool log = std::getenv("UAW_NET_LOG") != nullptr;
    if (log) std::fprintf(stderr, "[net %d] from %d: %s\n", id_, from, l.c_str());
    size_t pos = 0;
    const std::string kind = word(l, pos);
    if (host_) {
        if (kind == "O") {
            const uint64_t turn = std::strtoull(word(l, pos).c_str(), nullptr, 10);
            gathered_[turn].push_back({from, l.substr(std::min(l.size(), pos + 1))});
        } else if (kind == "DONE") {
            const uint64_t turn = std::strtoull(word(l, pos).c_str(), nullptr, 10);
            const uint64_t tick = std::strtoull(word(l, pos).c_str(), nullptr, 10);
            const uint64_t hash = std::strtoull(word(l, pos).c_str(), nullptr, 10);
            hostClose(from, turn, tick, hash);
        }
        return;
    }
    if (kind == "C") {
        const uint64_t turn = std::strtoull(word(l, pos).c_str(), nullptr, 10);
        alamo::Command c;
        if (decodeCommand(l.substr(std::min(l.size(), pos + 1)), c)) pending_[turn].push_back(c);
    } else if (kind == "GO") {
        const uint64_t turn = std::strtoull(word(l, pos).c_str(), nullptr, 10);
        known_[turn] = std::move(pending_[turn]);
        pending_.erase(turn);
    } else if (kind == "DESYNC") {
        problem_ = "Out of sync with the host: the games differ";
    }
}

void Lockstep::poll() {
    if (host_) {
        for (Peer& p : peers_) {
            if (!p.alive) continue;
            std::vector<std::string> lines;
            const bool open = p.socket.receive(lines);
            for (const std::string& l : lines) handle(p.id, l);
            if (!open) {
                p.alive = false;
                status_ = (p.name.empty() ? "A player" : p.name) + " left the game";
                hostRelease(); // nobody waits for them any more
            }
        }
        return;
    }
    std::vector<std::string> lines;
    const bool open = server_.receive(lines);
    for (const std::string& l : lines) handle(0, l);
    if (!open && problem_.empty()) problem_ = "The connection to the host was lost";
}

} // namespace uaw
