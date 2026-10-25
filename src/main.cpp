#include <iostream>

#include "frameflow/cli.hpp"

int main(int argc, char** argv) { return frameflow::cli_main(argc, argv, std::cout, std::cerr); }
