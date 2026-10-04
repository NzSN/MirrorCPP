#include "scheduled_counter.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc, char** argv) {
  try {
    auto plan = scheduled_counter::plan(true);
    if (argc == 3 && std::string_view(argv[1]) == "--replay") {
      std::ifstream file(argv[2], std::ios::binary);
      if (!file) throw std::runtime_error("cannot open schedule");
      std::string text;
      char buffer[4096];
      while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
        text.append(buffer, static_cast<std::size_t>(file.gcount()));
        if (text.size() > 1'048'576) throw std::runtime_error("schedule exceeds byte bound");
      }
      plan = mirrorcpp::schedule::parse_schedule(text);
    } else if (argc != 1) {
      throw std::runtime_error("usage: scheduled_counter [--replay SCHEDULE.json]");
    }
    auto execution = mirrorcpp::schedule::run_schedule(plan, scheduled_counter::adapter());
    std::cout << execution->receipt().dump(2) << '\n';
    return execution->report().passed() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}
