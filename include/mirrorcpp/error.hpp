// mirrorcpp/error.hpp — Error model: ErrorKind, Error, Result<T> (design §4.3).
//
// Ownership note (task t2): DiffHint is defined in mirrorcpp/protocol.hpp
// (t3). To let error.hpp compile standalone — without depending on
// protocol.hpp — Error carries the step_mismatch hint list behind
// std::shared_ptr<const std::vector<DiffHint>>. std::shared_ptr supports
// incomplete element types (the control block is type-erased), so this header
// never needs DiffHint's definition; callers (protocol.cpp / client code) that
// populate or inspect hints do so in a TU that has included protocol.hpp.
// State (std::map<std::string, Value>) lives in value.hpp (also part of t2), so
// error.hpp includes value.hpp for it.
#ifndef MIRRORCPP_ERROR_HPP
#define MIRRORCPP_ERROR_HPP

#include <mirrorcpp/value.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mirrorcpp {

// Defined in mirrorcpp/protocol.hpp (t3). Forward declaration only; see the
// header comment for why Error's hints member uses a shared_ptr indirection.
struct DiffHint;

enum class ErrorKind {
  io,            // socket/pipe/read-write failure, unexpected EOF
  spawn,         // child process failed to start / exited abnormally (stdio transport)
  json,          // malformed JSON on the wire
  protocol,      // unexpected message for the current phase; mirror-sent protocol_error
  registration,  // mirror-sent register_error
  spec_invalid,  // spec_validated with {"invalid": …}
  step_mismatch, // conformance failure; carries expected/actual/hints
  tls,           // handshake/verification/pin failure
  registry,      // discovery/connect-from-registry exhausted candidates
  spec_source,   // spec_from_files: missing/ambiguous module
  model_interface, // local model-interface codec/selection/binding failure
};

// Human-readable name for an ErrorKind (for diagnostics, CLI exit messages).
inline const char* error_kind_name(ErrorKind k) noexcept {
  switch (k) {
    case ErrorKind::io:            return "io";
    case ErrorKind::spawn:         return "spawn";
    case ErrorKind::json:          return "json";
    case ErrorKind::protocol:      return "protocol";
    case ErrorKind::registration:  return "registration";
    case ErrorKind::spec_invalid:  return "spec_invalid";
    case ErrorKind::step_mismatch: return "step_mismatch";
    case ErrorKind::tls:           return "tls";
    case ErrorKind::registry:      return "registry";
    case ErrorKind::spec_source:   return "spec_source";
    case ErrorKind::model_interface: return "model_interface";
  }
  return "unknown";
}

struct Error {
  ErrorKind kind = ErrorKind::io;
  std::string message;  // human-readable; includes mirror text when present
  std::optional<std::string> code; // stable model-interface/server code

  // Populated only when kind == step_mismatch:
  std::optional<State> expected;
  std::optional<State> actual;
  // Hints list for step_mismatch. Indirection: DiffHint is defined in
  // protocol.hpp (t3); shared_ptr tolerates the incomplete type. Nullptr
  // unless the mismatch carried hints.
  std::shared_ptr<const std::vector<DiffHint>> hints;

  Error() = default;
  Error(ErrorKind k, std::string m) : kind(k), message(std::move(m)) {}
  Error(ErrorKind k, std::string m, std::string stable_code)
      : kind(k), message(std::move(m)), code(std::move(stable_code)) {}
  Error(ErrorKind k, std::string m, std::optional<State> e, std::optional<State> a,
        std::shared_ptr<const std::vector<DiffHint>> h = {})
      : kind(k), message(std::move(m)), expected(std::move(e)), actual(std::move(a)),
        hints(std::move(h)) {}

  bool is_step_mismatch() const noexcept { return kind == ErrorKind::step_mismatch; }
};

// Convenience constructor for the step_mismatch kind. hints may be null when
// the mirror sent no hints; populate via shared_ptr once DiffHint is visible.
inline Error make_step_mismatch(std::string message, std::optional<State> expected,
                                std::optional<State> actual,
                                std::shared_ptr<const std::vector<DiffHint>> hints = {}) {
  return Error(ErrorKind::step_mismatch, std::move(message), std::move(expected),
               std::move(actual), std::move(hints));
}

inline Error make_error(ErrorKind kind, std::string message) {
  return Error(kind, std::move(message));
}

// Result<T> = expected<T, Error>; no exceptions cross the API boundary (§4.3).
template <class T>
using Result = std::expected<T, Error>;

}  // namespace mirrorcpp

#endif  // MIRRORCPP_ERROR_HPP
