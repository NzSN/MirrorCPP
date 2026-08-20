// mirrorcpp/detail/process.hpp — subprocess spawn + pipes (design §5.3).
//
// RAII child-process wrapper used by the stdio transport:
//   - POSIX: posix_spawn + pipe() + dup2 (fully validated path);
//   - Windows: CreateProcess + anonymous inheritable pipe handles (#ifdef, best
//     effort, not validated here).
//
// Child stdin/stdout are piped to/from the parent; stderr is INHERITED from the
// parent (so Apalache diagnostics reach the user's console).
//
// Lifecycle guarantees (design §4.4):
//   - the destructor kills (best effort) and reaps the child so no Apalache JVM
//     is ever orphaned;
//   - a blocking wait() reaps the child and returns its exit code.
#ifndef MIRRORCPP_DETAIL_PROCESS_HPP
#define MIRRORCPP_DETAIL_PROCESS_HPP

#include <mirrorcpp/error.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mirrorcpp::detail {

class Subprocess {
public:
  Subprocess() = default;
  ~Subprocess();

  Subprocess(const Subprocess&) = delete;
  Subprocess& operator=(const Subprocess&) = delete;

  Subprocess(Subprocess&& other) noexcept;
  Subprocess& operator=(Subprocess&& other) noexcept;

  // Spawn `path` with no arguments. stdin/stdout piped (child side dup2'd to
  // 0/1), stderr inherited. Returns ErrorKind::spawn on failure.
  Result<void> spawn(const std::string& path);

  // Write data to the child's stdin. Writing to a dead child is a spawn error.
  Result<void> write_stdin(std::string_view data);

  // Close the parent's stdin pipe (sends EOF to the child).
  Result<void> close_stdin();

  // Read a chunk from the child's stdout ('' = EOF). Non-blocking reap is
  // attempted on EOF so exit_code() is populated.
  Result<std::string> read_stdout();

  // Non-blocking reap. Returns {true, code} when the child has been reaped.
  Result<std::pair<bool, long>> try_wait();

  // Blocking wait for the child; returns its exit code. A non-zero exit is NOT
  // an error (the caller inspects it).
  Result<long> wait();

  // Best-effort SIGKILL (POSIX) / TerminateProcess (Windows).
  Result<void> kill();

  bool running() const noexcept { return pid_ >= 0 && !exit_code_.has_value(); }
  std::optional<long> exit_code() const noexcept { return exit_code_; }
  long stdout_fd() const noexcept { return stdout_fd_; }

private:
  void reset() noexcept;
  long pid_ = -1;        // pid_t / DWORD held as long
  long stdin_fd_ = -1;   // parent write end of the child's stdin pipe
  long stdout_fd_ = -1;  // parent read end of the child's stdout pipe
  std::optional<long> exit_code_;
};

} // namespace mirrorcpp::detail

#endif
