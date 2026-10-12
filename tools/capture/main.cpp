// revenant-capture. All of it is in capture_tool.cpp, where the tests reach it;
// this file is the process around it.

#include <chrono>
#include <cstdio>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "tools/capture/capture_tool.h"

int main(int argc, char** argv)
{
    namespace tool = revenant::capture_tool;

    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args.front() == "--help" || args.front() == "-h" ||
        args.front() == "help") {
        std::print("{}", tool::usage());
        return args.empty() ? 2 : 0;
    }

    tool::Environment env;
    env.now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    env.out = [](std::string_view line) {
        std::println("{}", line);
        std::fflush(stdout);
    };

    if (auto status = tool::run(args, env); !status) {
        std::fflush(stdout);
        std::println(stderr, "revenant-capture: {}", status.error().message);
        return 1;
    }
    return 0;
}
