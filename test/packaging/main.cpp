// Downstream packaging smoke consumer (test/packaging/run_smoke.sh).
// Exercises the INSTALLED mirrorcpp package through find_package(mirrorcpp
// CONFIG): the umbrella header, the version function, and a real protocol
// encode round-trip through the public API.
#include <mirrorcpp/mirrorcpp.hpp>

#include <cstdio>
#include <string>

int main() {
  using namespace mirrorcpp;

  const std::string version = mirrorcpp_version();
  std::printf("consumer ok, version=%s\n", version.c_str());
  if (version != MIRRORCPP_VERSION) return 1;

  // Exercise the public API surface a little (design §5.2): a register
  // message with an inline spec, encoded to its wire form.
  ApalacheConfig cfg;
  cfg.spec_path = "HourClock.tla";
  cfg.invariant = "TraceComplete";
  cfg.length_bound = 13;

  ApalacheSpec spec;
  spec.sources.push_back("---- MODULE HourClock ----\nVARIABLE hr\nInit == hr = 1\nNext == hr' = hr\n====");

  Register msg;
  msg.cfg = cfg;
  msg.tc.num_traces = 1;
  msg.spec = spec;

  const std::string wire = encode_client_message(msg);
  if (wire.find("\"proto_step\":\"register\"") == std::string::npos) return 2;
  if (wire.find("\"specPath\":\"HourClock.tla\"") == std::string::npos) return 3;
  if (wire.find("\"invariant\":\"TraceComplete\"") == std::string::npos) return 4;

  std::puts("consumer: register encode round-trip OK");
  return 0;
}
