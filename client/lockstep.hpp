// LAN games: deterministic lockstep over TCP. Every machine runs the whole simulation; only the
// players' orders travel. Time is cut into turns of kTurnTicks; an order given during turn k runs
// at the start of turn k + kDelayTurns on every machine. The host collects each player's orders for
// a turn and, once everyone has finished it, sends them all out; nobody runs a turn before that.
// Each player also reports a checksum of the game state so a desync is noticed.
#pragma once

#include "menu.hpp"
#include "net.hpp"

#include "alamo/sim.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace uaw {

class Lockstep {
public:
    static constexpr int kTurnTicks = 3;   // 0.1 s
    static constexpr int kDelayTurns = 2;  // orders run 0.2-0.3 s after they are given

    static std::unique_ptr<Lockstep> host(int port, std::string& error);
    static std::unique_ptr<Lockstep> join(const std::string& address, const std::string& name, std::string& error);

    bool isHost() const { return host_; }
    int id() const { return id_; }        // 0 = the host; -1 until the host has welcomed us
    bool connected() const;
    const std::string& status() const { return status_; }

    // ---- lobby
    // Host: accepts players and puts them into open slots of `setup` (sent to everyone when it
    // changes); clients: takes the host's setup. Returns true when the setup changed.
    bool pollLobby(SkirmishSetup& setup);
    void start(const SkirmishSetup& setup); // host
    bool started() const { return started_; }
    std::vector<std::pair<int, std::string>> players() const; // id, name (host: everyone; client: as known)

    // ---- game
    // Orders this machine's player gave while the simulation was at `tick`.
    void submit(const std::vector<alamo::Command>& orders, uint64_t tick);
    // True when the turn holding `tick` may run (its orders are known).
    bool canRun(uint64_t tick);
    // At the first tick of a turn: closes the turn the player can no longer add orders to (with the
    // state checksum) and returns the orders to run now, in the same order on every machine.
    std::vector<alamo::Command> beginTurn(uint64_t tick, uint64_t stateHash);
    void poll();
    const std::string& problem() const { return problem_; } // desync or lost connection

private:
    struct Peer {
        int id = 0;
        std::string name;
        net::Socket socket;
        bool alive = true;
    };
    bool host_ = false, started_ = false;
    int id_ = -1, nextId_ = 1;
    std::string name_, status_, problem_;
    net::Socket listener_, server_;     // host: listening; client: connection to the host
    std::vector<Peer> peers_;           // host: the clients
    std::map<int, std::string> names_;  // everyone, by id
    std::string setupLine_;             // last setup sent / received
    // Turns: orders gathered (host), turns closed by each player (host), turns known (all).
    std::map<uint64_t, std::vector<std::pair<int, std::string>>> gathered_; // turn -> (player, order text)
    std::map<uint64_t, std::set<int>> closed_;
    std::map<uint64_t, std::map<int, uint64_t>> hashes_;                    // turn-start tick -> player -> checksum
    std::map<uint64_t, std::vector<alamo::Command>> known_;                 // turn -> orders to run
    std::map<uint64_t, std::vector<alamo::Command>> mine_;                  // this player's orders by turn
    std::map<uint64_t, std::vector<alamo::Command>> pending_;               // client: orders of turns not yet released
    void handle(int from, const std::string& line);
    void hostClose(int player, uint64_t turn, uint64_t tick, uint64_t hash);
    void hostRelease();
    std::set<int> alivePlayers() const;
};

} // namespace uaw
