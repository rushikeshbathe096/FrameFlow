#include <iostream>

#include "frameflow/version.hpp"

int main() {
  std::cout << "frameflow " << frameflow::version() << '\n';
  return 0;
}
