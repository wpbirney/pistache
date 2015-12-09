/* listener.cc
   Mathieu Stefani, 12 August 2015
   
*/

#include <iostream>
#include <sys/socket.h>
#include <unistd.h> 
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/epoll.h>
#include <pthread.h>
#include <signal.h>
#include <cassert>
#include <cstring>
#include "listener.h"
#include "peer.h"
#include "common.h"
#include "os.h"

using namespace std;

namespace Net {

namespace Tcp {

namespace {
    volatile sig_atomic_t g_listen_fd = -1;

    void handle_sigint(int) {
        if (g_listen_fd != -1) {
            close(g_listen_fd);
            g_listen_fd = -1;
        }
    }
}

using Polling::NotifyOn;


void setSocketOptions(Fd fd, Flags<Options> options) {
    if (options.hasFlag(Options::ReuseAddr)) {
        int one = 1;
        TRY(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one)));
    }

    if (options.hasFlag(Options::Linger)) {
        struct linger opt;
        opt.l_onoff = 1;
        opt.l_linger = 1;
        TRY(::setsockopt(fd, SOL_SOCKET, SO_LINGER, &opt, sizeof (opt)));
    }

    if (options.hasFlag(Options::FastOpen)) {
        int hint = 5;
        TRY(::setsockopt(fd, SOL_TCP, TCP_FASTOPEN, &hint, sizeof (hint)));
    }
    if (options.hasFlag(Options::NoDelay)) {
        int one = 1;
        TRY(::setsockopt(fd, SOL_TCP, TCP_NODELAY, &one, sizeof (one)));
    }

}

Listener::Listener()
    : listen_fd(-1)
    , backlog_(Const::MaxBacklog)
{ }

Listener::Listener(const Address& address)
    : addr_(address)
    , listen_fd(-1)
    , backlog_(Const::MaxBacklog)
{
}

void
Listener::init(size_t workers, Flags<Options> options, int backlog)
{
    if (workers > hardware_concurrency()) {
        // Log::warning() << "More workers than available cores"
    }

    options_ = options;
    backlog_ = backlog;

    if (options_.hasFlag(Options::InstallSignalHandler)) {
        if (signal(SIGINT, handle_sigint) == SIG_ERR) {
            throw std::runtime_error("Could not install signal handler");
        }
    }

    for (size_t i = 0; i < workers; ++i) {
        auto wrk = std::unique_ptr<IoWorker>(new IoWorker);
        ioGroup.push_back(std::move(wrk));
    }
}

void
Listener::setHandler(const std::shared_ptr<Handler>& handler)
{
    handler_ = handler;
}

void
Listener::pinWorker(size_t worker, const CpuSet& set)
{
    if (ioGroup.empty()) {
        throw std::domain_error("Invalid operation, did you call init() before ?");
    }
    if (worker > ioGroup.size()) {
        throw std::invalid_argument("Trying to pin invalid worker");
    }

    auto &wrk = ioGroup[worker];
    wrk->pin(set);
}

bool
Listener::bind() {
    return bind(addr_);
}

bool
Listener::bind(const Address& address) {
    if (ioGroup.empty()) {
        throw std::runtime_error("Call init() before calling bind()");
    }

    addr_ = address;

    struct addrinfo hints;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM; 
    hints.ai_flags = AI_PASSIVE;
    hints.ai_protocol = 0;

    auto host = addr_.host();
    if (host == "*") {
        host = "0.0.0.0";
    }

    /* We rely on the fact that a string literal is an lvalue const char[N] */
    static constexpr size_t MaxPortLen = sizeof("65535");

    char port[MaxPortLen];
    std::fill(port, port + MaxPortLen, 0);
    std::snprintf(port, MaxPortLen, "%d", static_cast<uint16_t>(addr_.port()));

    struct addrinfo *addrs;
    TRY(::getaddrinfo(host.c_str(), port, &hints, &addrs));

    int fd = -1;

    for (struct addrinfo *addr = addrs; addr; addr = addr->ai_next) {
        fd = ::socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
        if (fd < 0) continue;

        setSocketOptions(fd, options_);

        if (::bind(fd, addr->ai_addr, addr->ai_addrlen) < 0) {
            close(fd);
            continue;
        }

        TRY(::listen(fd, backlog_));
    }


    listen_fd = fd;
    g_listen_fd = fd;

    for (auto& io: ioGroup) {
        io->start(handler_, options_);
    }

    sh = false;
    loadThread.reset(new std::thread([=]() {
        this->runLoadThread();
    }));
    
    return true;
}

void
Listener::run() {
    for (;;) {
        struct sockaddr_in peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        int client_fd = ::accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_addr_len);
        if (client_fd < 0) {
            if (g_listen_fd == -1) {
                cout << "SIGINT Signal received, shutdowning !" << endl;
                shutdown();
                break;

            } else {
                throw std::runtime_error(strerror(errno));
            }
        }

        make_non_blocking(client_fd);

        auto peer = make_shared<Peer>(Address::fromUnix((struct sockaddr *)&peer_addr));
        peer->associateFd(client_fd);

        dispatchPeer(peer);
    }
}

void
Listener::runLoadThread() {
    std::vector<rusage> lastUsages;

    while (!sh) {
        std::vector<Async::Promise<rusage>> loads;
        loads.reserve(ioGroup.size());

        for (const auto& io: ioGroup) {
            loads.push_back(io->getLoad());
        }

        Async::whenAll(std::begin(loads), std::end(loads))
              .then([&](const std::vector<rusage>& usages) {
                    auto totalElapsed = [](rusage usage) {
                        return (usage.ru_stime.tv_sec * 1e6 + usage.ru_stime.tv_usec)
                             + (usage.ru_utime.tv_sec * 1e6 + usage.ru_utime.tv_usec);
                    };

                    if (lastUsages.empty()) lastUsages = usages;
                    else {
                        for (size_t i = 0; i < usages.size(); ++i) {
                            auto last = lastUsages[i];
                            const auto& usage = usages[i];

                            auto now = totalElapsed(usage);
                            auto time = now - totalElapsed(last);

                            auto load = (time * 100.0) / 1e6;

                            //printf("Total load for I/O thread %lu = %.3lf%%\n", i, load);

                        }
                        lastUsages = usages;
                    }
             }, Async::NoExcept);

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void
Listener::shutdown() {
    for (auto &worker: ioGroup) {
        worker->shutdown();
    }
    sh = true;
}

Address
Listener::address() const {
    return addr_;
}

Options
Listener::options() const {
    return options_;
}

void
Listener::dispatchPeer(const std::shared_ptr<Peer>& peer) {
    const size_t workers = ioGroup.size();
    size_t worker = peer->fd() % workers;

    ioGroup[worker]->handleNewPeer(peer);

}

} // namespace Tcp

} // namespace Net
