// mirrorcpp/mirrorcpp.hpp — umbrella header re-exporting the public API (design §4.2).
#ifndef MIRRORCPP_MIRRORCPP_HPP
#define MIRRORCPP_MIRRORCPP_HPP

#include "error.hpp"
#include "value.hpp"
#include "protocol.hpp"
#include "transport.hpp"
#include "registry.hpp"
#include "spec.hpp"
#include "client.hpp"

// Library version. Lives in the umbrella so every consumer sees one definition.
#define MIRRORCPP_VERSION "0.1.0"

inline const char* mirrorcpp_version() noexcept { return MIRRORCPP_VERSION; }

#endif
