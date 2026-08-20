// MirrorCPP mirrorcpp-validate CLI integration tests (design §5.7 / §8).
//
// Drives the built CLI against the python fake-mirror over TCP and checks the
// exit-code matrix:
//   valid            -> exit 0, stdout "VALID"
//   invalid          -> exit 1, stdout "INVALID" + apalache output
//   register_error   -> exit 2 (infrastructure error)
//   connection refused-> exit 2
//   bad flags        -> exit 2
//
// Requires the CLI binary path via MIRRORCPP_CLI_BIN compile definition and
// the fake server via MIRRORCPP_TEST_INTEGRATION_DIR.
#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <vector>

namespace {

// Find an ephemeral loopback port (bind to 0, note the port, close).
int free_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return 0;
  }
  socklen_t len = sizeof addr;
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    ::close(fd);
    return 0;
  }
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

// Spawn the python fake-mirror TCP server; returns the child pid.
pid_t start_server(const std::filesystem::path& server, int port, const char* scenario) {
  const pid_t pid = ::fork();
  if (pid != 0) return pid;
  // Child: /dev/null for stdio, then exec.
  const int devnull = ::open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    ::dup2(devnull, STDIN_FILENO);
    ::dup2(devnull, STDOUT_FILENO);
    ::dup2(devnull, STDERR_FILENO);
  }
  const std::string port_s = std::to_string(port);
  char* const argv[] = {const_cast<char*>("python3"),
                        const_cast<char*>(server.c_str()),
                        const_cast<char*>(port_s.c_str()),
                        const_cast<char*>(scenario),
                        nullptr};
  ::execvp("python3", argv);
  _exit(127);
}

// Run the CLI, capturing combined stdout+stderr into `out`. Returns exit code.
int run_cli(const std::string& cli_bin, const std::vector<std::string>& args,
            std::string& out) {
  int pipefd[2];
  if (::pipe(pipefd) != 0) return 127;
  const pid_t pid = ::fork();
  if (pid == 0) {
    ::close(pipefd[0]);
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::dup2(pipefd[1], STDERR_FILENO);
    ::close(pipefd[1]);
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(cli_bin.c_str()));
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execv(cli_bin.c_str(), argv.data());
    _exit(127);
  }
  ::close(pipefd[1]);
  char buf[4096];
  ssize_t n;
  while ((n = ::read(pipefd[0], buf, sizeof buf)) > 0) out.append(buf, static_cast<size_t>(n));
  ::close(pipefd[0]);
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 128;
}

struct ServerGuard {
  pid_t pid = -1;
  int port = 0;
  ~ServerGuard() { if (pid > 0) { ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0); } }
};

const std::filesystem::path kServer =
    std::filesystem::path(MIRRORCPP_TEST_INTEGRATION_DIR) / "fake_mirror_tcp.py";
const std::string kCli = MIRRORCPP_CLI_BIN;
const std::string kSpec =
    (std::filesystem::path(MIRRORCPP_TEST_INTEGRATION_DIR) / ".." / ".." / "specs" /
     "HourClock.tla").string();

}  // namespace

// ---------------------------------------------------------------------------
// valid -> exit 0, "VALID" on stdout
// ---------------------------------------------------------------------------
TEST_CASE("cli validate: valid -> exit 0 VALID", "[integration][cli]") {
  ServerGuard srv;
  srv.port = free_port();
  REQUIRE(srv.port > 0);
  srv.pid = start_server(kServer, srv.port, "valid");
  REQUIRE(srv.pid > 0);
  ::usleep(200000);  // let the server bind

  std::string out;
  const int rc = run_cli(kCli, {"--host", "127.0.0.1", "--port", std::to_string(srv.port),
                            "--spec", kSpec}, out);
  CHECK(rc == 0);
  CHECK(out.find("VALID") != std::string::npos);
}

// ---------------------------------------------------------------------------
// invalid -> exit 1, "INVALID" + apalache output on stdout
// ---------------------------------------------------------------------------
TEST_CASE("cli validate: invalid -> exit 1 INVALID + output", "[integration][cli]") {
  ServerGuard srv;
  srv.port = free_port();
  REQUIRE(srv.port > 0);
  srv.pid = start_server(kServer, srv.port, "invalid");
  REQUIRE(srv.pid > 0);
  ::usleep(200000);

  std::string out;
  const int rc = run_cli(kCli, {"--host", "127.0.0.1", "--port", std::to_string(srv.port),
                            "--spec", kSpec}, out);
  CHECK(rc == 1);
  CHECK(out.find("INVALID") != std::string::npos);
  CHECK(out.find("state invariant violated") != std::string::npos);
}

// ---------------------------------------------------------------------------
// register_error -> exit 2 (infrastructure error)
// ---------------------------------------------------------------------------
TEST_CASE("cli validate: register_error -> exit 2", "[integration][cli]") {
  ServerGuard srv;
  srv.port = free_port();
  REQUIRE(srv.port > 0);
  srv.pid = start_server(kServer, srv.port, "register_error");
  REQUIRE(srv.pid > 0);
  ::usleep(200000);

  std::string out;
  const int rc = run_cli(kCli, {"--host", "127.0.0.1", "--port", std::to_string(srv.port),
                            "--spec", kSpec}, out);
  CHECK(rc == 2);
  CHECK(out.find("bound") != std::string::npos);
}

// ---------------------------------------------------------------------------
// connection refused -> exit 2
// ---------------------------------------------------------------------------
TEST_CASE("cli validate: connection refused -> exit 2", "[integration][cli]") {
  const int port = free_port();  // nothing listens here
  std::string out;
  const int rc = run_cli(kCli, {"--host", "127.0.0.1", "--port", std::to_string(port),
                            "--spec", kSpec, "--bound", "5"}, out);
  CHECK(rc == 2);
}

// ---------------------------------------------------------------------------
// bad flags -> exit 2
// ---------------------------------------------------------------------------
TEST_CASE("cli validate: bad flags -> exit 2", "[integration][cli]") {
  std::string out;
  // --registry with --host/--port is a conflict.
  const int rc = run_cli(kCli, {"--host", "127.0.0.1", "--port", "9", "--registry",
                            "http://x", "--spec", kSpec}, out);
  CHECK(rc == 2);
  CHECK(out.find("mutually exclusive") != std::string::npos);
}

// ---------------------------------------------------------------------------
// --registry --tls without cert/key/ca -> friendly ErrorKind::tls -> exit 2
// (parity with the --tls branch pre-check). TLS build only: the notls build
// rejects --registry with its own "requires a TLS build" message.
// ---------------------------------------------------------------------------
#if MIRRORCPP_WITH_TLS
TEST_CASE("cli validate: registry without certs -> exit 2", "[integration][cli]") {
  std::string out;
  const int rc = run_cli(kCli, {"--registry", "http://127.0.0.1:9", "--tls",
                            "--spec", kSpec}, out);
  CHECK(rc == 2);
  CHECK(out.find("--cert") != std::string::npos);
}
#endif
