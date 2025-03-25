// std
#include <string>
// 3rd party
#include <fmt/format.h>

#include <cxxopts.hpp>
// project
#include "service_record/service_record.h"
#include "service_record/version.h"

auto main(int argc, char **argv) -> int {
  cxxopts::Options options(*argv, "A program to welcome the world!");

  auto name = std::string{"bob"};
  auto home_dir = std::string{"/home/bob"};

  // clang-format off
  options.add_options()
    ("h,help", "Show help")
    ("v,version", "Print the current version number")
    ("name", "user name", cxxopts::value(name))
    ("path", "home directory", cxxopts::value(home_dir))
  ;
  // clang-format on

  auto result = options.parse(argc, argv);

  if (result["help"].as<bool>()) {
    fmt::println("{}", options.help());
    return 0;
  }

  if (result["version"].as<bool>()) {
    fmt::println("version: {}", SERVICE_RECORD_VERSION);
    return 0;
  }

  fmt::println("service_record/{}, user name: {}, home directory: {}, answer: {}", SERVICE_RECORD_VERSION,
               name, home_dir, service_record_lib_answer());

  return 0;
}
