#include "web/webhost_view.h"

// POSIX only, and the whole file says so rather than the build system: this
// backend spawns a child with posix_spawn, talks to it over a BSD socket and
// reaps it with waitpid. Windows has all three under different names and none
// of them here. tests/ is skipped on Windows for the same reason (see the
// top-level CMakeLists), so the stub below is what a Windows build links.
#ifndef _WIN32

#include "paths.h"

#include <web_module_view.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <csignal>
#include <netinet/in.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace fs = std::filesystem;

namespace logosctl::web {

namespace {

// How long the helper is given to come up and connect back.
constexpr int kConnectTimeoutMs = 20000;

// How long a terminated helper is given to exit before SIGKILL.
constexpr int kExitGraceMs = 2000;

// Kill a helper outright and REAP it. The waitpid is the point: without it the
// daemon accumulates a zombie per page it ever gave up on, and this process is
// long-lived by design.
void killAndReap(pid_t pid)
{
    if (pid <= 0) return;
    ::kill(pid, SIGKILL);
    int status = 0;
    ::waitpid(pid, &status, 0);
}

// ── the channel ─────────────────────────────────────────────────────────────
//
// One JSON text per line, which is framing enough: a web-transport message is
// a JSON object serialised compactly, and JSON escapes every newline that
// appears inside a string, so a literal '\n' can only be the separator.
//
// The shape is in_memory_channel.cpp's, deliberately and for its reasons: the
// pump has to be able to outlive the endpoint, because close() is reachable
// FROM the pump (a decode error inside a receiver runs RpcPeer::fail(), which
// closes this very channel), and a thread cannot join itself.
struct SocketState {
    std::mutex mu;
    int fd = -1;
    bool closed = false;

    // Held across the DELIVERY so setReceiver(nullptr) waits out an in-flight
    // receiver. RECURSIVE because the receiver is allowed to detach itself --
    // same path, same reason as the in-memory pair.
    std::recursive_mutex receiverMu;
    logos::web::IMessageChannel::Receiver receiver;

    void shutdownFd()
    {
        std::lock_guard<std::mutex> g(mu);
        if (closed) return;
        closed = true;
        if (fd >= 0) {
            // shutdown() rather than close(): it wakes the pump's blocking
            // read() with an EOF, where closing the descriptor out from under
            // it would be a use-after-close race with whatever the kernel
            // hands the number to next. The descriptor itself is closed by the
            // pump when it falls out.
            ::shutdown(fd, SHUT_RDWR);
        }
    }

    bool write(const std::string& message)
    {
        std::lock_guard<std::mutex> g(mu);
        if (closed || fd < 0) return false;
        std::string frame = message;
        frame.push_back('\n');
        std::size_t sent = 0;
        while (sent < frame.size()) {
            const ssize_t n = ::send(fd, frame.data() + sent, frame.size() - sent, 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (n == 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    // Runs on the pump thread until the far end goes away. Returns when the
    // page's process is gone or this endpoint was closed.
    void pump()
    {
        std::string inbox;
        char buffer[8192];
        for (;;) {
            int localFd;
            {
                std::lock_guard<std::mutex> g(mu);
                if (closed || fd < 0) break;
                localFd = fd;
            }
            const ssize_t n = ::recv(localFd, buffer, sizeof(buffer), 0);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;   // EOF -- the page's process is gone
            inbox.append(buffer, static_cast<std::size_t>(n));

            std::size_t nl;
            while ((nl = inbox.find('\n')) != std::string::npos) {
                std::string line = inbox.substr(0, nl);
                inbox.erase(0, nl + 1);
                if (line.empty()) continue;
                // COPIED, then invoked: a receiver may detach itself
                // mid-delivery, and calling the member would run a
                // std::function setReceiver has just destroyed.
                std::lock_guard<std::recursive_mutex> g(receiverMu);
                const logos::web::IMessageChannel::Receiver cb = receiver;
                if (cb) cb(line);
            }
        }

        std::lock_guard<std::mutex> g(mu);
        closed = true;
        if (fd >= 0) { ::close(fd); fd = -1; }
    }
};

class SocketChannel : public logos::web::IMessageChannel {
public:
    explicit SocketChannel(int fd)
        : m_state(std::make_shared<SocketState>())
    {
        m_state->fd = fd;
    }

    // Started separately from the constructor so the owner can install the
    // end-of-stream callback first: the page can die before the first message,
    // and a pump that had already reached EOF would report it to nobody.
    void start(std::function<void()> onEof)
    {
        auto state = m_state;                  // the pump's own reference
        m_pump = std::thread([state, onEof = std::move(onEof)] {
            state->pump();
            if (onEof) onEof();
        });
    }

    ~SocketChannel() override { shutdown(); }

    void setReceiver(Receiver receiver) override
    {
        std::lock_guard<std::recursive_mutex> g(m_state->receiverMu);
        m_state->receiver = std::move(receiver);
    }

    bool send(const std::string& message) override { return m_state->write(message); }

    void close() override { shutdown(); }

    bool isOpen() const override
    {
        std::lock_guard<std::mutex> g(m_state->mu);
        return !m_state->closed;
    }

private:
    void shutdown()
    {
        m_state->shutdownFd();
        std::lock_guard<std::mutex> g(m_pumpMu);
        if (!m_pump.joinable()) return;
        if (m_pump.get_id() == std::this_thread::get_id())
            m_pump.detach();   // we ARE the pump; see SocketState
        else
            m_pump.join();
    }

    std::shared_ptr<SocketState> m_state;
    std::mutex m_pumpMu;
    std::thread m_pump;
};

// ── the view ────────────────────────────────────────────────────────────────

class WebhostView : public LogosCore::WebModuleView {
public:
    WebhostView(std::shared_ptr<SocketChannel> channel, pid_t pid)
        : m_channel(std::move(channel)), m_pid(pid) {}

    ~WebhostView() override { stop(); }

    logos::web::MessageChannelPtr channel() const override { return m_channel; }

    std::optional<int64_t> pid() const override
    {
        return m_alive.load() ? std::optional<int64_t>(static_cast<int64_t>(m_pid))
                              : std::nullopt;
    }

    void setOnDied(std::function<void()> callback) override
    {
        std::lock_guard<std::mutex> g(m_mu);
        m_onDied = std::move(callback);
    }

    bool isAlive() const override { return m_alive.load(); }

    // Called from the pump thread when the socket reaches EOF, and from stop()
    // when the host tears the page down. Announces at most once, whichever
    // arrives first.
    void announceDeath()
    {
        if (m_announced.exchange(true)) return;
        m_alive.store(false);
        std::function<void()> cb;
        {
            std::lock_guard<std::mutex> g(m_mu);
            cb = m_onDied;
        }
        if (cb) cb();
    }

private:
    void stop()
    {
        // The deliberate teardown, so the death callback must NOT fire: the
        // container is already unloading this module and announcing here would
        // report an orderly unload as a lost page.
        m_announced.store(true);
        m_alive.store(false);

        if (m_pid > 0) {
            ::kill(m_pid, SIGTERM);
            for (int waited = 0; waited < kExitGraceMs; waited += 20) {
                int status = 0;
                const pid_t r = ::waitpid(m_pid, &status, WNOHANG);
                if (r == m_pid || (r < 0 && errno == ECHILD)) { m_pid = -1; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (m_pid > 0) {
                killAndReap(m_pid);
                m_pid = -1;
            }
        }
        if (m_channel) m_channel->close();
    }

    std::shared_ptr<SocketChannel> m_channel;
    pid_t m_pid = -1;
    std::atomic<bool> m_alive{true};
    std::atomic<bool> m_announced{false};
    mutable std::mutex m_mu;
    std::function<void()> m_onDied;
};

// ── spawning one ────────────────────────────────────────────────────────────

// A loopback listener on an ephemeral port, or -1. The port is written to
// `port`.
int listenOnLoopback(uint16_t& port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
        || ::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return -1;
    }
    port = ntohs(addr.sin_port);
    return fd;
}

std::unique_ptr<LogosCore::WebModuleView> spawnView(const std::string& helper,
                                                    const LogosCore::WebModuleViewRequest& request)
{
    uint16_t port = 0;
    const int listener = listenOnLoopback(port);
    if (listener < 0) {
        spdlog::error("Web module {}: could not open a loopback listener for the "
                      "webview host: {}", request.moduleName, std::strerror(errno));
        return nullptr;
    }

    const std::string portArg = std::to_string(port);
    std::vector<std::string> argvStore = {
        helper,
        "--entry", request.entryPath,
        "--port", portArg,
        "--module", request.moduleName,
    };
    std::vector<char*> argv;
    argv.reserve(argvStore.size() + 1);
    for (auto& a : argvStore) argv.push_back(a.data());
    argv.push_back(nullptr);

    // A headless daemon has no screen to put a module's page on, and a `web`
    // module here is a provider rather than a UI. Only a DEFAULT, though: an
    // operator who exports QT_QPA_PLATFORM keeps it, which is how a developer
    // watches the page they are debugging.
    std::vector<std::string> envStore;
    std::vector<char*> envp;
    const bool platformSet = ::getenv("QT_QPA_PLATFORM") != nullptr;
    for (char** e = environ; *e; ++e) envStore.emplace_back(*e);
    if (!platformSet) envStore.emplace_back("QT_QPA_PLATFORM=offscreen");
    envp.reserve(envStore.size() + 1);
    for (auto& e : envStore) envp.push_back(e.data());
    envp.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawn(&pid, helper.c_str(), nullptr, nullptr,
                                 argv.data(), envp.data());
    if (rc != 0) {
        spdlog::error("Web module {}: could not start {}: {}",
                      request.moduleName, helper, std::strerror(rc));
        ::close(listener);
        return nullptr;
    }

    pollfd pfd{listener, POLLIN, 0};
    const int ready = ::poll(&pfd, 1, kConnectTimeoutMs);
    if (ready <= 0) {
        spdlog::error("Web module {}: the webview host did not connect back "
                      "within {} ms", request.moduleName, kConnectTimeoutMs);
        killAndReap(pid);
        ::close(listener);
        return nullptr;
    }

    const int conn = ::accept(listener, nullptr, nullptr);
    ::close(listener);
    if (conn < 0) {
        spdlog::error("Web module {}: accept() failed: {}",
                      request.moduleName, std::strerror(errno));
        killAndReap(pid);
        return nullptr;
    }

    auto channel = std::make_shared<SocketChannel>(conn);
    auto view = std::make_unique<WebhostView>(channel, pid);
    WebhostView* raw = view.get();
    // Installed BEFORE the pump starts: a page that dies immediately still has
    // its EOF reported, because the pump cannot reach it until this returns.
    channel->start([raw] { raw->announceDeath(); });
    spdlog::info("Web module {}: page host running at pid {} on 127.0.0.1:{}",
                 request.moduleName, static_cast<long>(pid), port);
    return view;
}

std::string resolveHelper(const std::string& given, std::string* whyNot)
{
    if (!given.empty()) {
        if (fs::is_regular_file(given)) return given;
        if (whyNot) *whyNot = "no webview host at '" + given + "'";
        return {};
    }

    // The env var first, so a developer can point the daemon at a webhost they
    // just built without reinstalling either.
    if (const char* fromEnv = ::getenv("LOGOSCORE_WEBHOST")) {
        if (fs::is_regular_file(fromEnv)) return fromEnv;
        if (whyNot) *whyNot = std::string("LOGOSCORE_WEBHOST points at '") + fromEnv
                            + "', which is not a file";
        return {};
    }

    const std::string binDir = paths::executableDir();
    if (!binDir.empty()) {
        const fs::path candidate = fs::path(binDir) / webhostBinaryName();
        if (fs::is_regular_file(candidate)) return candidate.string();
    }
    if (whyNot)
        *whyNot = std::string("this build ships no ") + webhostBinaryName()
                + " beside the daemon (build with -DLOGOSCORE_WITH_WEBENGINE=ON, "
                  "or set LOGOSCORE_WEBHOST)";
    return {};
}

} // namespace

const char* webhostBinaryName() { return "logoscore-webhost"; }

bool installWebhostBackend(const std::string& helperPath, std::string* whyNot)
{
    const std::string helper = resolveHelper(helperPath, whyNot);
    if (helper.empty()) return false;

    LogosCore::setWebModuleViewFactory(
        [helper](const LogosCore::WebModuleViewRequest& request)
            -> std::unique_ptr<LogosCore::WebModuleView> {
            return spawnView(helper, request);
        });
    spdlog::info("Web container backend: {}", helper);
    return true;
}

} // namespace logosctl::web

#else // _WIN32

namespace logosctl::web {

const char* webhostBinaryName() { return "logoscore-webhost.exe"; }

bool installWebhostBackend(const std::string&, std::string* whyNot)
{
    if (whyNot) *whyNot = "the webview host is not built for Windows";
    return false;
}

} // namespace logosctl::web

#endif // _WIN32
