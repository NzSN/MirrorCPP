// MirrorCPP async job interface integration tests (guide §6, C17–C22;
// drafts/conformance-gap-plan.md G1.6).
//
// Drives the async API against the scripted TCP fake mirror
// (fake_mirror_async.py): submit/query/await/cancel, long-poll semantics,
// idempotent terminal results, queue-full backpressure, unknown ids, infraError
// outcomes, cross-connection job control, and the stdio preflight refusal.
#include <mirrorcpp/mirrorcpp.hpp>

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>

using namespace mirrorcpp;

namespace {

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
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len);
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

pid_t start_server(const std::filesystem::path& server, int port,
                   const char* scenario, int nconns) {
  const pid_t pid = ::fork();
  if (pid != 0) return pid;
  const int devnull = ::open("/dev/null", O_RDWR);
  if (devnull >= 0) {
    ::dup2(devnull, STDIN_FILENO);
    ::dup2(devnull, STDOUT_FILENO);
    ::dup2(devnull, STDERR_FILENO);
  }
  const std::string port_s = std::to_string(port);
  const std::string n_s = std::to_string(nconns);
  char* const argv[] = {const_cast<char*>("python3"),
                        const_cast<char*>(server.c_str()),
                        const_cast<char*>(port_s.c_str()),
                        const_cast<char*>(scenario),
                        const_cast<char*>(n_s.c_str()),
                        nullptr};
  ::execvp("python3", argv);
  _exit(127);
}

struct ServerGuard {
  pid_t pid = -1;
  int port = 0;
  ~ServerGuard() { if (pid > 0) { ::kill(pid, SIGTERM); ::waitpid(pid, nullptr, 0); } }
};

const std::filesystem::path kAsyncServer =
    std::filesystem::path(MIRRORCPP_TEST_INTEGRATION_DIR) / "fake_mirror_async.py";

ServerGuard start(const char* scenario, int nconns = 1) {
  ServerGuard srv;
  srv.port = free_port();
  REQUIRE(srv.port > 0);
  srv.pid = start_server(kAsyncServer, srv.port, scenario, nconns);
  REQUIRE(srv.pid > 0);
  ::usleep(200000);  // let the server bind
  return srv;
}

std::unique_ptr<Transport> connect(int port) {
  auto t = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(port));
  for (int attempt = 0; !t && attempt < 100; ++attempt) {
    ::usleep(20000);
    t = connect_tcp("127.0.0.1", static_cast<std::uint16_t>(port));
  }
  REQUIRE(t.has_value());
  return std::move(*t);
}

ApalacheConfig cfg() {
  ApalacheConfig c;
  c.spec_path = "s.tla";
  c.invariant = "Inv";
  return c;
}

}  // namespace

TEST_CASE("async: submit refused up front on a stdio transport", "[integration][async]") {
  auto t = spawn_mirror(
      (std::filesystem::path(MIRRORCPP_TEST_INTEGRATION_DIR) / "fake_mirror_happy").string());
  REQUIRE(t != nullptr);
  REQUIRE_FALSE(t->async_capable());
  auto r = submit_validate_async(*t, cfg(), 5);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == ErrorKind::protocol);
  (void)t->close();
}

TEST_CASE("async: validate job submit/query/await + idempotent terminal (C18)", "[integration][async]") {
  auto srv = start("validate");
  auto t = connect(srv.port);
  REQUIRE(t->async_capable());

  auto accepted = submit_validate_async(*t, cfg(), 5);
  REQUIRE(accepted.has_value());
  const std::string job = accepted->job_id;
  CHECK(accepted->kind == JobKind::validate);

  auto st = query_job(*t, job);
  REQUIRE(st.has_value());
  CHECK(st->phase == JobPhase::running);

  // Long-poll with timeout: non-terminal job_status, never an error (C18).
  auto waited = await_job(*t, job, 1);
  REQUIRE(waited.has_value());
  REQUIRE(std::holds_alternative<JobStatus>(*waited));
  CHECK(std::get<JobStatus>(*waited).phase == JobPhase::running);

  // No timeout: blocks to terminal. The validate outcome reuses SpecValidated
  // (C20) — the same handling as the sync flow applies.
  auto done = await_job(*t, job);
  REQUIRE(done.has_value());
  REQUIRE(std::holds_alternative<JobResult>(*done));
  const auto& outcome = std::get<JobResult>(*done).outcome;
  REQUIRE(std::holds_alternative<SpecValidated>(outcome.value));
  CHECK(std::get<SpecValidated>(outcome.value).is_valid());

  // Terminal results are idempotent until eviction (C18).
  auto again = await_job(*t, job);
  REQUIRE(again.has_value());
  REQUIRE(std::holds_alternative<JobResult>(*again));
  CHECK(std::get<SpecValidated>(std::get<JobResult>(*again).outcome.value).is_valid());

  (void)t->close();
}

TEST_CASE("async: trace-gen job yields genTraces outcome", "[integration][async]") {
  auto srv = start("gen");
  auto t = connect(srv.port);

  TraceGenerationConfig tc;
  tc.num_traces = 2;
  auto accepted = submit_trace_gen_async(*t, cfg(), tc);
  REQUIRE(accepted.has_value());
  CHECK(accepted->kind == JobKind::gen_traces);

  auto done = await_job(*t, accepted->job_id);
  REQUIRE(done.has_value());
  REQUIRE(std::holds_alternative<JobResult>(*done));
  const auto& outcome = std::get<JobResult>(*done).outcome;
  REQUIRE(std::holds_alternative<GenTracesDone>(outcome.value));
  CHECK(std::get<GenTracesDone>(outcome.value).itf_trace_paths ==
        std::vector<std::string>{"out/itf/t1.itf.json"});
  (void)t->close();
}

TEST_CASE("async: full queue rejects at submit with register_error (C22)", "[integration][async]") {
  auto srv = start("queue_full");
  auto t = connect(srv.port);
  auto r = submit_validate_async(*t, cfg(), 5);
  REQUIRE_FALSE(r.has_value());
  CHECK(r.error().kind == ErrorKind::registration);
  CHECK(r.error().message.find("job queue full") != std::string::npos);
  (void)t->close();
}

TEST_CASE("async: unknown job id answers phase unknown (C21)", "[integration][async]") {
  auto srv = start("validate");
  auto t = connect(srv.port);

  auto st = query_job(*t, "job-never-submitted");
  REQUIRE(st.has_value());
  CHECK(st->phase == JobPhase::unknown);

  auto cancelled = cancel_job(*t, "job-never-submitted");
  REQUIRE(cancelled.has_value());
  REQUIRE(std::holds_alternative<JobStatus>(*cancelled));
  CHECK(std::get<JobStatus>(*cancelled).phase == JobPhase::unknown);
  (void)t->close();
}

TEST_CASE("async: infraError outcome is not a spec verdict", "[integration][async]") {
  auto srv = start("infra");
  auto t = connect(srv.port);
  auto accepted = submit_validate_async(*t, cfg(), 5);
  REQUIRE(accepted.has_value());

  auto done = await_job(*t, accepted->job_id);
  REQUIRE(done.has_value());
  REQUIRE(std::holds_alternative<JobResult>(*done));
  const auto& outcome = std::get<JobResult>(*done).outcome;
  REQUIRE(std::holds_alternative<std::string>(outcome.value));  // infraError
  CHECK(std::get<std::string>(outcome.value) == "worker died");
  (void)t->close();
}

TEST_CASE("async: job control is cross-connection (C17)", "[integration][async]") {
  auto srv = start("validate", 2);
  auto a = connect(srv.port);
  auto b = connect(srv.port);

  auto accepted = submit_validate_async(*a, cfg(), 5);
  REQUIRE(accepted.has_value());
  const std::string job = accepted->job_id;

  // A second connection may query/cancel the job (jobIds are process-global).
  auto st = query_job(*b, job);
  REQUIRE(st.has_value());
  CHECK(st->phase == JobPhase::running);

  auto cancelled = cancel_job(*b, job);
  REQUIRE(cancelled.has_value());
  REQUIRE(std::holds_alternative<JobStatus>(*cancelled));
  CHECK(std::get<JobStatus>(*cancelled).phase == JobPhase::cancelled);

  auto st2 = query_job(*a, job);
  REQUIRE(st2.has_value());
  CHECK(st2->phase == JobPhase::cancelled);

  (void)a->close();
  (void)b->close();
}

TEST_CASE("async: query preserves terminal payload and compatibility phase", "[integration][async]") {
  auto srv = start("validate");
  auto t = connect(srv.port);
  auto job = submit_validate_async(*t, cfg(), 3);
  REQUIRE(job.has_value());
  auto done = await_job(*t, job->job_id);
  REQUIRE(done.has_value());
  auto full = query_job_result(*t, job->job_id);
  REQUIRE(full.has_value());
  CHECK(std::holds_alternative<JobResult>(*full));
  auto phase = query_job(*t, job->job_id);
  REQUIRE(phase.has_value());
  CHECK(phase->phase == JobPhase::done);
  (void)t->close();
}


namespace {
class VectorTransport final : public Transport {
 public:
  std::string reply;
  bool closed = false;
  bool fail_send = false;
  bool async_capable() const noexcept override { return true; }
  Result<void> send_line(std::string_view) override {
    if (fail_send) return std::unexpected(Error(ErrorKind::io, "send failed"));
    return {};
  }
  Result<std::string> recv_line() override { return reply; }
  Result<long> close() override { closed = true; return 0; }
};
}

TEST_CASE("async: shared reply conformance vectors", "[integration][async]") {
  const char* path = std::getenv("MIRRORS_CLIENT_CONFORMANCE");
  std::ifstream input(path ? std::filesystem::path(path) :
      std::filesystem::path(MIRRORCPP_TEST_INTEGRATION_DIR) / "../fixtures/client-conformance/async-replies.json");
  REQUIRE(input.good());
  nlohmann::json corpus;
  input >> corpus;
  for (const auto& entry : corpus.at("cases")) {
    INFO(entry.at("name"));
    VectorTransport transport;
    transport.reply = entry.at("reply").dump();
    const auto& request = entry.at("request");
    const std::string tag = request.at("proto_step");
    bool accepted;
    if (tag == "register_validate_async") {
      accepted = submit_validate_async(transport, cfg(), 3).has_value();
    } else if (tag == "query_job") {
      accepted = query_job_result(transport, request.at("jobId").get<std::string>()).has_value();
    } else {
      accepted = await_job(transport, request.at("jobId").get<std::string>(), 1).has_value();
    }
    CHECK(accepted == entry.at("accept").get<bool>());
    CHECK(transport.closed == !accepted);
  }
}

TEST_CASE("async: malformed replies close the borrowed connection", "[integration][async]") {
  VectorTransport transport;
  transport.reply = "{bad json";
  CHECK_FALSE(await_job(transport, "job-1").has_value());
  CHECK(transport.closed);
}

TEST_CASE("registration send failure closes the borrowed transport", "[integration][async]") {
  VectorTransport transport;
  transport.fail_send = true;
  auto result = run_client_validate(transport, cfg(), 3);
  CHECK_FALSE(result.has_value());
  CHECK(transport.closed);
}
