// src/spec.cpp — EXTENDS/INSTANCE closure resolution (design §5.5).
//
// Port of MirrorECMA's spec.ts. No exceptions escape this module: every
// failure path returns Error{ErrorKind::spec_source, message}.

#include <mirrorcpp/spec.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_set>

namespace mirrorcpp {
namespace {

// -----------------------------------------------------------------------
// Comment stripping (defensive; the regexes below are line-anchored)
// -----------------------------------------------------------------------
// Remove \* line comments and (* ... *) block comments so EXTENDS/INSTANCE
// clauses inside comments never count. TLA+ strings may contain these
// markers, but the pragmatism matches the sibling clients.
std::string strip_comments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  bool in_block = false;
  const std::size_t n = src.size();
  std::size_t i = 0;
  while (i < n) {
    if (in_block) {
      if (i + 1 < n && src[i] == '*' && src[i + 1] == ')') { in_block = false; i += 2; }
      else ++i;
      continue;
    }
    if (i + 1 < n && src[i] == '(' && src[i + 1] == '*') { in_block = true; i += 2; continue; }
    if (src[i] == '\\' && i + 1 < n && src[i + 1] == '*') {
      while (i < n && src[i] != '\n') ++i;
      continue;
    }
    out.push_back(src[i]);
    ++i;
  }
  return out;
}

// -----------------------------------------------------------------------
// Builtin modules that are always resolvable by Apalache (never vendored)
// -----------------------------------------------------------------------
bool is_builtin(std::string_view name) {
  static const std::string kBuiltins[] = {
      "Naturals", "Integers", "Reals", "Sequences",
      "FiniteSets", "TLC", "Bags", "Apalache",
  };
  for (const auto& b : kBuiltins) if (name == b) return true;
  return false;
}

// -----------------------------------------------------------------------
// EXTENDS / INSTANCE clause parsing (design §5.5)
// -----------------------------------------------------------------------
// Line-anchored, multiline: ^\s*EXTENDS\s+(.+)$ / ^\s*INSTANCE\s+(.+)$.
// Module refs = first whitespace-token of each comma-separated part, so
// "INSTANCE X WITH y <- z" yields X.
std::vector<std::string> import_names(const std::string& src) {
  static const std::regex extends_re(R"(^\s*EXTENDS\s+(.+)$)", std::regex::multiline);
  static const std::regex instance_re(R"(^\s*INSTANCE\s+(.+)$)", std::regex::multiline);

  std::vector<std::string> names;
  const std::string stripped = strip_comments(src);
  for (const auto& re : {&extends_re, &instance_re}) {
    auto it = std::sregex_iterator(stripped.begin(), stripped.end(), *re);
    const auto end = std::sregex_iterator();
    for (; it != end; ++it) {
      const std::string body = (*it)[1].str();
      std::istringstream parts(body);
      std::string part;
      while (std::getline(parts, part, ',')) {
        std::istringstream tokstream(part);
        std::string tok;
        tokstream >> tok;  // first whitespace-token of the part
        if (!tok.empty()) names.push_back(tok);
      }
    }
  }
  return names;
}

// -----------------------------------------------------------------------
// File helpers
// -----------------------------------------------------------------------
bool read_file(const std::filesystem::path& p, std::string& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return static_cast<bool>(f) || f.eof();
}

std::optional<std::filesystem::path> canonical_of(const std::filesystem::path& p) {
  std::error_code ec;
  auto c = std::filesystem::canonical(p, ec);
  if (ec) return std::nullopt;
  return c;
}

}  // namespace

// -----------------------------------------------------------------------
// default_search_dirs
// -----------------------------------------------------------------------
std::vector<std::filesystem::path> default_search_dirs() {
  std::vector<std::filesystem::path> dirs;
  const char* raw = std::getenv("TLA_LIBRARY_PATH");
  if (raw == nullptr) return dirs;
  std::string_view sv(raw);
  std::size_t start = 0;
  while (start <= sv.size()) {
    const std::size_t colon = sv.find(':', start);
    const std::string_view part = (colon == std::string_view::npos)
                                      ? sv.substr(start)
                                      : sv.substr(start, colon - start);
    if (!part.empty()) dirs.emplace_back(part);
    if (colon == std::string_view::npos) break;
    start = colon + 1;
  }
  return dirs;
}

// -----------------------------------------------------------------------
// spec_from_files
// -----------------------------------------------------------------------
Result<ApalacheSpec> spec_from_files(const std::filesystem::path& root,
                                     std::vector<std::filesystem::path> search_dirs) {
  // Resolve the root first; every module lives in `sources` with its
  // canonical path in `pending` at the same index (BFS dependency order).
  std::vector<std::string> sources;
  std::vector<std::filesystem::path> pending;
  std::unordered_set<std::string> visited;

  auto push_file = [&](const std::filesystem::path& file) -> Result<void> {
    auto canonical = canonical_of(file);
    if (!canonical) {
      return std::unexpected(Error(ErrorKind::spec_source,
                                   "cannot resolve spec file '" + file.string() + "'"));
    }
    const std::string key = canonical->string();
    if (!visited.insert(key).second) return {};  // diamond dedup
    std::string src;
    if (!read_file(*canonical, src)) {
      return std::unexpected(Error(ErrorKind::spec_source,
                                   "cannot read spec file '" + canonical->string() + "'"));
    }
    sources.push_back(src);
    pending.push_back(*canonical);
    return {};
  };

  if (auto r = push_file(root); !r) return std::unexpected(r.error());

  std::size_t idx = 0;
  while (idx < pending.size()) {
    // Copy out of the vectors: push_file() below push_back()s into them, which
    // may reallocate and invalidate any references/iterators held across calls.
    const std::filesystem::path importer = pending[idx];
    const std::string src = sources[idx];
    ++idx;

    // Candidate directories: the importer's own dir first, then each search dir.
    std::vector<std::filesystem::path> cand_dirs;
    auto importer_dir = importer.parent_path();
    cand_dirs.push_back(importer_dir.empty() ? std::filesystem::path(".") : importer_dir);
    cand_dirs.insert(cand_dirs.end(), search_dirs.begin(), search_dirs.end());

    for (const std::string& name : import_names(src)) {
      if (is_builtin(name)) continue;

      // Collect all candidate files for <Name>.tla, dedup by canonical path.
      std::vector<std::filesystem::path> candidates;
      std::set<std::string> seen;
      for (const auto& d : cand_dirs) {
        const auto cand = d / (name + ".tla");
        std::error_code ec;
        if (!std::filesystem::exists(cand, ec)) continue;
        auto cc = canonical_of(cand);
        if (cc && seen.insert(cc->string()).second) candidates.push_back(*cc);
      }

      const std::string importer_name = importer.filename().string();
      if (candidates.empty()) {
        std::string dirs_str;
        for (std::size_t i = 0; i < cand_dirs.size(); ++i) {
          if (i) dirs_str += ", ";
          dirs_str += cand_dirs[i].string();
        }
        return std::unexpected(Error(ErrorKind::spec_source,
            "module '" + name + "' imported by '" + importer_name +
            "' not found (searched: " + dirs_str + ")"));
      }
      if (candidates.size() > 1) {
        std::string cands_str;
        for (std::size_t i = 0; i < candidates.size(); ++i) {
          if (i) cands_str += ", ";
          cands_str += candidates[i].string();
        }
        return std::unexpected(Error(ErrorKind::spec_source,
            "module '" + name + "' imported by '" + importer_name +
            "' is ambiguous; found in multiple directories: " + cands_str));
      }

      if (auto r = push_file(candidates[0]); !r) return std::unexpected(r.error());
    }
  }

  ApalacheSpec spec;
  spec.sources = std::move(sources);
  return spec;
}

}  // namespace mirrorcpp
