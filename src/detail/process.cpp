// mirrorcpp/detail/process.cpp — subprocess spawn + pipes (design §5.3).
// POSIX: posix_spawn + pipe + dup2 (validated). Windows: CreateProcess (#ifdef, best effort).
#include "process.hpp"

#include "line.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace mirrorcpp::detail {

using std::unexpected;

namespace {

// Decode a waitpid status into a single integer exit code. A signal-terminated
// child is reported as 128 + signal so it is never mistaken for a clean 0.
long decode_status(int status) {
#ifdef _WIN32
  return status;
#else
  if (WIFEXITED(status)) return static_cast<long>(WEXITSTATUS(status));
  if (WIFSIGNALED(status)) return 128L + static_cast<long>(WTERMSIG(status));
  return -1;
#endif
}

// write() that suppresses SIGPIPE on this thread so writing to a dead child's
// stdin pipe returns EPIPE instead of killing the process.
ssize_t write_no_sigpipe(long fd, const char* data, std::size_t n) {
#ifdef _WIN32
  return ::_write(static_cast<int>(fd), data, static_cast<unsigned>(n));
#else
  sigset_t set{}, old{};
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, &old);
  const ssize_t r = ::write(fd, data, n);
  pthread_sigmask(SIG_SETMASK, &old, nullptr);
  return r;
#endif
}

} // namespace

Subprocess::~Subprocess() {
  // Best effort: kill + reap so the child (and any Apalache JVM) is never orphaned.
  if (pid_ < 0) return;
  if (!exit_code_) {
    (void)kill();
    (void)wait();
  }
  if (stdin_fd_ >= 0) ::close(stdin_fd_);
  if (stdout_fd_ >= 0) ::close(stdout_fd_);
}

void Subprocess::reset() noexcept {
  if (pid_ >= 0 && !exit_code_) {
    (void)kill();
    (void)wait();
  }
  if (stdin_fd_ >= 0) ::close(stdin_fd_);
  if (stdout_fd_ >= 0) ::close(stdout_fd_);
  pid_ = -1;
  stdin_fd_ = -1;
  stdout_fd_ = -1;
  exit_code_.reset();
}

Subprocess::Subprocess(Subprocess&& other) noexcept
    : pid_(other.pid_),
      stdin_fd_(other.stdin_fd_),
      stdout_fd_(other.stdout_fd_),
      exit_code_(other.exit_code_) {
  other.pid_ = -1;
  other.stdin_fd_ = -1;
  other.stdout_fd_ = -1;
  other.exit_code_.reset();
}

Subprocess& Subprocess::operator=(Subprocess&& other) noexcept {
  if (this != &other) {
    reset();
    pid_ = other.pid_;
    stdin_fd_ = other.stdin_fd_;
    stdout_fd_ = other.stdout_fd_;
    exit_code_ = other.exit_code_;
    other.pid_ = -1;
    other.stdin_fd_ = -1;
    other.stdout_fd_ = -1;
    other.exit_code_.reset();
  }
  return *this;
}

Result<void> Subprocess::spawn(const std::string& path) {
  reset();

#ifdef _WIN32
  // -- Windows (best effort, unvalidated here) ------------------------------
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof sa;
  sa.bInheritHandle = TRUE;
  HANDLE child_in_r = nullptr, child_in_w = nullptr;
  HANDLE child_out_r = nullptr, child_out_w = nullptr;
  if (!CreatePipe(&child_in_r, &child_in_w, &sa, 0) ||
      !CreatePipe(&child_out_r, &child_out_w, &sa, 0)) {
    if (child_in_r) CloseHandle(child_in_r);
    if (child_in_w) CloseHandle(child_in_w);
    if (child_out_r) CloseHandle(child_out_r);
    if (child_out_w) CloseHandle(child_out_w);
    return unexpected(Error(ErrorKind::spawn, "CreatePipe failed"));
  }
  SetHandleInformation(child_in_w, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(child_out_r, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si{};
  si.cb = sizeof si;
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = child_in_r;
  si.hStdOutput = child_out_w;
  si.hStdError = ::GetStdHandle(STD_ERROR_HANDLE); // inherited
  PROCESS_INFORMATION pi{};
  std::string cmdline = "\"" + path + "\"";
  if (!CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    CloseHandle(child_in_r); CloseHandle(child_in_w);
    CloseHandle(child_out_r); CloseHandle(child_out_w);
    return unexpected(Error(ErrorKind::spawn, "CreateProcess failed"));
  }
  CloseHandle(child_in_r);
  CloseHandle(child_out_w);
  pid_ = static_cast<long>(reinterpret_cast<uintptr_t>(pi.hProcess));
  stdin_fd_ = ::_open_osfhandle(reinterpret_cast<intptr_t>(child_in_w), _O_WRONLY);
  stdout_fd_ = ::_open_osfhandle(reinterpret_cast<intptr_t>(child_out_r), _O_RDONLY);
  CloseHandle(pi.hThread);
  return {};
#else
  // -- POSIX (validated) ----------------------------------------------------
#if defined(__linux__)
  int in_pipe[2]{}, out_pipe[2]{};
  if (::pipe2(in_pipe, O_CLOEXEC) != 0 || ::pipe2(out_pipe, O_CLOEXEC) != 0) {
    const int e = errno;
    if (in_pipe[0] >= 0) { ::close(in_pipe[0]); ::close(in_pipe[1]); }
    if (out_pipe[0] >= 0) { ::close(out_pipe[0]); ::close(out_pipe[1]); }
    return unexpected(Error(ErrorKind::spawn, "pipe failed: " + std::string(std::strerror(e))));
  }
#else
  int in_pipe[2]{-1, -1}, out_pipe[2]{-1, -1};
  if (::pipe(in_pipe) != 0 || ::pipe(out_pipe) != 0) {
    const int e = errno;
    if (in_pipe[0] >= 0) { ::close(in_pipe[0]); ::close(in_pipe[1]); }
    if (out_pipe[0] >= 0) { ::close(out_pipe[0]); ::close(out_pipe[1]); }
    return unexpected(Error(ErrorKind::spawn, "pipe failed: " + std::string(std::strerror(e))));
  }
  // Set CLOEXEC on all four ends so they never leak into unrelated children.
  for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1]})
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif

  posix_spawn_file_actions_t fa;
  if (posix_spawn_file_actions_init(&fa) != 0) {
    const int e = errno;
    ::close(in_pipe[0]); ::close(in_pipe[1]); ::close(out_pipe[0]); ::close(out_pipe[1]);
    return unexpected(Error(ErrorKind::spawn, "posix_spawn_file_actions_init: " + std::string(std::strerror(e))));
  }
  // Child: fd 0 <- in_pipe[0], fd 1 <- out_pipe[1]; stderr inherited (no action).
  posix_spawn_file_actions_adddup2(&fa, in_pipe[0], STDIN_FILENO);
  posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
  posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
  // Parent keeps in_pipe[1] (stdin) and out_pipe[0] (stdout); close in child via CLOEXEC.
  posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
  posix_spawn_file_actions_addclose(&fa, out_pipe[0]);

  std::string path_copy = path;
  char* argv[] = { path_copy.data(), nullptr };
  pid_t pid = 0;
  const int rc = ::posix_spawn(&pid, path.c_str(), &fa, nullptr, argv, environ);
  const int saved_errno = errno;
  posix_spawn_file_actions_destroy(&fa);

  if (rc != 0) {
    ::close(in_pipe[0]); ::close(in_pipe[1]); ::close(out_pipe[0]); ::close(out_pipe[1]);
    return unexpected(Error(ErrorKind::spawn,
                            "posix_spawn(" + path + "): " + std::string(std::strerror(rc != -1 ? rc : saved_errno))));
  }

  // Parent: close the child's ends.
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  pid_ = static_cast<long>(pid);
  stdin_fd_ = in_pipe[1];
  stdout_fd_ = out_pipe[0];
  return {};
#endif
}

Result<void> Subprocess::write_stdin(std::string_view data) {
  if (stdin_fd_ < 0) return unexpected(Error(ErrorKind::spawn, "write_stdin: stdin pipe closed"));
  std::size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = write_no_sigpipe(stdin_fd_, data.data() + off, data.size() - off);
    if (n < 0) {
      const int e = errno;
      if (e == EINTR) continue;
      if (e == EPIPE) {
        (void)try_wait(); // best effort: populate exit_code_ for a precise message
        std::string msg = "child stdin pipe closed";
        if (exit_code_) msg += " (child exited with code " + std::to_string(*exit_code_) + ")";
        return unexpected(Error(ErrorKind::spawn, msg));
      }
      return unexpected(Error(ErrorKind::io, std::string("write to child stdin failed: ") + std::strerror(e)));
    }
    off += static_cast<std::size_t>(n);
  }
  return {};
}

Result<void> Subprocess::close_stdin() {
  if (stdin_fd_ >= 0) {
    ::close(stdin_fd_);
    stdin_fd_ = -1;
  }
  return {};
}

Result<std::string> Subprocess::read_stdout() {
  if (stdout_fd_ < 0) return unexpected(Error(ErrorKind::io, "read_stdout: stdout pipe closed"));
  char chunk[kLineChunkSize];
  for (;;) {
    const ssize_t n = ::read(stdout_fd_, chunk, sizeof chunk);
    if (n < 0 && errno == EINTR) continue;
    if (n == 0) {
      // EOF: attempt a non-blocking reap so exit_code() is available.
      (void)try_wait();
      return std::string{};
    }
    if (n < 0) {
      return unexpected(Error(ErrorKind::io, std::string("read from child stdout failed: ") + std::strerror(errno)));
    }
    return std::string(chunk, static_cast<std::size_t>(n));
  }
}

Result<std::pair<bool, long>> Subprocess::try_wait() {
  if (pid_ < 0) return std::pair<bool, long>{false, -1};
  if (exit_code_) return std::pair<bool, long>{true, *exit_code_};
#ifdef _WIN32
  const DWORD code = WaitForSingleObject(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid_)), 0);
  if (code == WAIT_TIMEOUT) return std::pair<bool, long>{false, -1};
  DWORD exit = 0;
  if (!GetExitCodeProcess(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid_)), &exit))
    return unexpected(Error(ErrorKind::io, "GetExitCodeProcess failed"));
  CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid_)));
  exit_code_ = static_cast<long>(exit);
  return std::pair<bool, long>{true, *exit_code_};
#else
  int status = 0;
  const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (r == 0) return std::pair<bool, long>{false, -1};
  if (r < 0) {
    if (errno == EINTR) return std::pair<bool, long>{false, -1};
    return unexpected(Error(ErrorKind::io, std::string("waitpid failed: ") + std::strerror(errno)));
  }
  exit_code_ = decode_status(status);
  return std::pair<bool, long>{true, *exit_code_};
#endif
}

Result<long> Subprocess::wait() {
  if (pid_ < 0) return 0L;
  if (exit_code_) return *exit_code_;
#ifdef _WIN32
  WaitForSingleObject(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid_)), INFINITE);
  DWORD exit = 0;
  GetExitCodeProcess(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid_)), &exit);
  CloseHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid_)));
  exit_code_ = static_cast<long>(exit);
  return *exit_code_;
#else
  int status = 0;
  for (;;) {
    const pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, 0);
    if (r < 0) {
      if (errno == EINTR) continue;
      return unexpected(Error(ErrorKind::io, std::string("waitpid failed: ") + std::strerror(errno)));
    }
    break;
  }
  exit_code_ = decode_status(status);
  return *exit_code_;
#endif
}

Result<void> Subprocess::kill() {
  if (pid_ < 0 || exit_code_) return {};
#ifdef _WIN32
  TerminateProcess(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(pid_)), 1);
#else
  ::kill(static_cast<pid_t>(pid_), SIGKILL);
#endif
  return {};
}

} // namespace mirrorcpp::detail

