#include "tokamak/common/panic.h"

#include <cstdio>
#include <cstdlib>

namespace tokamak {

[[noreturn]] void panic(std::string_view message) {
    std::fprintf(stderr, "tokamak: invariant violation: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    std::abort();
}

}  // namespace tokamak
