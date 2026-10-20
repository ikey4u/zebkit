/* zebkit-helper: the engine's renderer, GPU and utility processes. The
 * engine starts it; it is not for running by hand. */

#include "zebkit/engine/include/zebkit/zebkit.h"

int main(int argc, char **argv) {
    return zebkit_helper_main(argc, argv);
}
