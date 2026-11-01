// The control for the stubs in zebkit_sandbox.lib (src/engine/win/sandbox_startup.cc):
// called on purpose, one ends the process at once. Exit code 0xC0000409 from
// __fastfail is what this test expects; if the stubs did nothing, it would print
// "returned" and exit 0, and win-*-sandbox's passing would prove nothing.

#include <stdio.h>

#include <zebkit/win_sandbox.h>

namespace logging::internal {
void init_rust_logging() noexcept;
}

int main() {
    int code = 0;
    if (zebkit_win_startup(&code))
        return code;
    printf("calling a stub\n");
    fflush(stdout);
    logging::internal::init_rust_logging();
    printf("returned\n");
    return 0;
}
