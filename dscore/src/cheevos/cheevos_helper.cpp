// SPDX-License-Identifier: GPL-3.0-or-later
// DSperate - Nintendo DS emulator. Copyright (C) 2026 DSperate contributors.
//
// The helper-process Backend (make_helper_backend): RG DS OS has no libcurl,
// but it has python3 with ssl, and RG DS's runtime/achievements.py serves
// rcheevos' requests over its stdin/stdout. One request is in flight at a
// time (Client's worker is serial); answers for an older request that timed
// out are recognised by their ID and dropped.
#include "cheevos/cheevos_http.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ds::cheevos {
namespace {

constexpr size_t MAX_BODY = 32u << 20; // the helper caps a body at 16 MiB
constexpr size_t MAX_HEADER = 96;

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool is_file(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string find_program(const std::string& name) {
  if (name.find('/') != std::string::npos) return ::access(name.c_str(), X_OK) == 0 ? name : std::string();
  const char* path = std::getenv("PATH");
  std::string dirs = path && *path ? path : "/usr/local/bin:/usr/bin:/bin";
  size_t at = 0;
  while (at <= dirs.size()) {
    size_t end = dirs.find(':', at);
    if (end == std::string::npos) end = dirs.size();
    std::string dir = dirs.substr(at, end - at);
    if (dir.empty()) dir = ".";
    const std::string candidate = dir + "/" + name;
    if (is_file(candidate) && ::access(candidate.c_str(), X_OK) == 0) return candidate;
    at = end + 1;
  }
  return {};
}

void set_cloexec(int fd) {
  const int flags = ::fcntl(fd, F_GETFD);
  if (flags >= 0) ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

class HelperBackend final : public Backend {
public:
  HelperBackend(HelperOptions o, std::string python) : o_(std::move(o)), python_(std::move(python)) {
    name_ = "helper (" + o_.script + ")";
  }
  ~HelperBackend() override { stop(); }

  const char* name() const override { return name_.c_str(); }

  void interrupt() override {
    const long long cap = now_ms() + 2000;
    long long old = cap_.load();
    while ((old == 0 || old > cap) && !cap_.compare_exchange_weak(old, cap)) {}
  }

  Response perform(const Request& req, const char* user_agent) override {
    Response res;
    if (user_agent && *user_agent && o_.user_agent.empty()) o_.user_agent = user_agent;
    for (const std::string* f : {&req.url, &req.body, &req.content_type, &req.save_path}) {
      if (f->find_first_of("\t\r\n") != std::string::npos) {
        res.error = "request has a tab or a newline in it";
        return res;
      }
    }
    if (given_up_) {
      res.error = "the achievements helper keeps stopping";
      return res;
    }
    // A dead helper gets one restart and the request one more try; dying
    // again on that try stops the helper for the rest of the session.
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (interrupted()) {
        res.error = "shutting down";
        return res;
      }
      if (fd_ < 0) {
        std::string err;
        if (!start(err)) {
          res.error = err;
          if (++deaths_ >= 2) given_up_ = true;
          return res;
        }
      }
      const unsigned long long id = ++next_id_;
      std::string line = std::to_string(id);
      for (const std::string* f : {&req.url, &req.body, &req.content_type, &req.save_path}) {
        line += '\t';
        line += *f;
      }
      line += '\n';
      bool died = false;
      Response r;
      if (exchange(line, id, r, died)) {
        deaths_ = 0;
        return r;
      }
      res = std::move(r);
      if (!died) return res;
      std::fprintf(stderr, "cheevos: the achievements helper stopped (%s)%s\n", res.error.c_str(),
                   deaths_ + 1 >= 2 ? "; not starting it again" : "; starting it again");
      if (++deaths_ >= 2) {
        given_up_ = true;
        return res;
      }
    }
    return res;
  }

private:
  bool interrupted() const {
    const long long cap = cap_.load();
    return cap != 0 && now_ms() >= cap;
  }

  bool start(std::string& err) {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
      err = std::string("socketpair: ") + std::strerror(errno);
      return false;
    }
    set_cloexec(sv[0]);
    set_cloexec(sv[1]);
#ifdef SO_NOSIGPIPE
    const int one = 1;
    ::setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif

    // Everything the child needs is built before fork(): after it, in a
    // threaded process, only async-signal-safe calls are allowed.
    std::vector<std::string> args = {python_, "-B", o_.script, "serve", "--badges", o_.badges};
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
      // The RG DS bridge is preloaded into this engine; python must not load it.
      if (!std::strncmp(*e, "LD_PRELOAD=", 11) || !std::strncmp(*e, "RGDS_RA_USER_AGENT=", 19)) continue;
      env.emplace_back(*e);
    }
    if (!o_.user_agent.empty()) env.push_back("RGDS_RA_USER_AGENT=" + o_.user_agent);
    std::vector<char*> envp;
    for (std::string& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);

    struct rlimit rl{};
    int max_fd = 1024;
    if (::getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) max_fd = static_cast<int>(rl.rlim_cur);
    if (max_fd > 65536) max_fd = 65536;

    const pid_t pid = ::fork();
    if (pid < 0) {
      err = std::string("fork: ") + std::strerror(errno);
      ::close(sv[0]);
      ::close(sv[1]);
      return false;
    }
    if (pid == 0) {
      ::dup2(sv[1], 0);
      ::dup2(sv[1], 1);
      for (int fd = 3; fd < max_fd; ++fd) ::close(fd);
      sigset_t none;
      sigemptyset(&none);
      ::sigprocmask(SIG_SETMASK, &none, nullptr);
      struct sigaction dfl{};
      dfl.sa_handler = SIG_DFL;
      for (int s : {SIGPIPE, SIGINT, SIGTERM, SIGHUP, SIGCHLD}) ::sigaction(s, &dfl, nullptr);
      ::execve(argv[0], argv.data(), envp.data());
      ::_exit(127);
    }
    ::close(sv[1]);
    fd_ = sv[0];
    pid_ = pid;
    inbuf_.clear();
    std::fprintf(stderr, "cheevos: started the achievements helper (pid %d)\n", static_cast<int>(pid));
    return true;
  }

  // Waits for the child to leave on its own for up to `grace_ms`, then TERM,
  // then KILL. Always reaps it.
  void reap(int grace_ms) {
    if (pid_ <= 0) return;
    int status = 0;
    const long long until = now_ms() + grace_ms;
    for (;;) {
      const pid_t r = ::waitpid(pid_, &status, WNOHANG);
      if (r == pid_ || (r < 0 && errno != EINTR)) {
        pid_ = -1;
        return;
      }
      if (now_ms() >= until) break;
      ::poll(nullptr, 0, 10);
    }
    ::kill(pid_, SIGTERM);
    const long long term_until = now_ms() + 500;
    for (;;) {
      const pid_t r = ::waitpid(pid_, &status, WNOHANG);
      if (r == pid_ || (r < 0 && errno != EINTR)) {
        pid_ = -1;
        return;
      }
      if (now_ms() >= term_until) break;
      ::poll(nullptr, 0, 10);
    }
    ::kill(pid_, SIGKILL);
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    pid_ = -1;
  }

  void close_fd() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    inbuf_.clear();
  }

  // After a broken or hung exchange: no grace.
  void kill_now() {
    close_fd();
    if (pid_ > 0) ::kill(pid_, SIGKILL);
    reap(0);
  }

  // Shutdown: EOF on its stdin lets it finish what it is doing and leave.
  void stop() {
    if (fd_ >= 0) ::shutdown(fd_, SHUT_WR);
    reap(1000);
    close_fd();
  }

  // Wait for `events` on the socket within the deadline. 1 ready, 0 timed out
  // or interrupted, -1 error.
  int wait_for(short events, long long deadline) {
    for (;;) {
      long long cap = cap_.load();
      long long until = deadline;
      if (cap != 0 && cap < until) until = cap;
      const long long left = until - now_ms();
      if (left <= 0) return 0;
      pollfd p{fd_, events, 0};
      const int r = ::poll(&p, 1, static_cast<int>(left < 100 ? left : 100));
      if (r < 0) {
        if (errno == EINTR) continue;
        return -1;
      }
      if (r > 0) return 1;
    }
  }

  bool exchange(const std::string& line, unsigned long long id, Response& res, bool& died) {
    const long long deadline = now_ms() + o_.timeout_ms;
    auto broken = [&](const std::string& why) {
      res.status = 0;
      res.body.clear();
      res.error = why;
      died = true;
      kill_now();
      return false;
    };
    auto timed_out = [&] {
      res.status = 0;
      res.body.clear();
      res.error = interrupted() ? "shutting down" : "the achievements helper did not answer in time";
      // Hung or not, its answer is no longer wanted: start afresh next time.
      kill_now();
      return false;
    };

    size_t sent = 0;
    while (sent < line.size()) {
      const int w = wait_for(POLLOUT, deadline);
      if (w == 0) return timed_out();
      if (w < 0) return broken(std::string("poll: ") + std::strerror(errno));
#ifdef MSG_NOSIGNAL
      const ssize_t n = ::send(fd_, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
#else
      const ssize_t n = ::send(fd_, line.data() + sent, line.size() - sent, 0);
#endif
      if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) continue;
        return broken(std::string("write: ") + std::strerror(errno));
      }
      sent += static_cast<size_t>(n);
    }

    // Reads until `inbuf_` holds at least `want` bytes.
    auto fill = [&](size_t want) -> int {
      char buf[16384];
      while (inbuf_.size() < want) {
        const int w = wait_for(POLLIN, deadline);
        if (w <= 0) return w;
        const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
        if (n == 0) return -2; // EOF: it has gone
        if (n < 0) {
          if (errno == EINTR || errno == EAGAIN) continue;
          return -1;
        }
        inbuf_.append(buf, static_cast<size_t>(n));
      }
      return 1;
    };

    for (;;) {
      size_t nl;
      while ((nl = inbuf_.find('\n')) == std::string::npos) {
        if (inbuf_.size() > MAX_HEADER) return broken("unreadable answer from the achievements helper");
        const int r = fill(inbuf_.size() + 1);
        if (r == 0) return timed_out();
        if (r == -2) return broken("it exited");
        if (r < 0) return broken(std::string("read: ") + std::strerror(errno));
      }
      if (nl > MAX_HEADER) return broken("unreadable answer from the achievements helper");
      const std::string header = inbuf_.substr(0, nl);
      inbuf_.erase(0, nl + 1);
      unsigned long long got_id = 0;
      int status = 0;
      unsigned long long length = 0;
      char tail = 0;
      if (std::sscanf(header.c_str(), "%llu %d %llu%c", &got_id, &status, &length, &tail) != 3 || length > MAX_BODY)
        return broken("unreadable answer from the achievements helper");
      const int r = fill(static_cast<size_t>(length));
      if (r == 0) return timed_out();
      if (r == -2) return broken("it exited");
      if (r < 0) return broken(std::string("read: ") + std::strerror(errno));
      if (got_id != id) {
        // Belongs to a request given up on earlier.
        inbuf_.erase(0, static_cast<size_t>(length));
        continue;
      }
      res.status = status;
      res.body.assign(inbuf_, 0, static_cast<size_t>(length));
      inbuf_.erase(0, static_cast<size_t>(length));
      if (status == 0) res.error = "RetroAchievements could not be reached";
      return true;
    }
  }

  HelperOptions o_;
  std::string python_, name_;
  int fd_ = -1;
  pid_t pid_ = -1;
  unsigned long long next_id_ = 0;
  int deaths_ = 0;
  bool given_up_ = false;
  std::string inbuf_;
  std::atomic<long long> cap_{0};
};

} // namespace

std::unique_ptr<Backend> make_helper_backend(const HelperOptions& options, std::string& err) {
  err.clear();
  if (options.script.empty() || !is_file(options.script)) {
    err = "achievements helper not found: " + options.script;
    return nullptr;
  }
  if (options.badges.empty()) {
    err = "the achievements helper needs a badges folder";
    return nullptr;
  }
  const std::string python = find_program(options.python.empty() ? "python3" : options.python);
  if (python.empty()) {
    err = (options.python.empty() ? std::string("python3") : options.python) + " not found";
    return nullptr;
  }
  return std::make_unique<HelperBackend>(options, python);
}

} // namespace ds::cheevos
