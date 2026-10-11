// The hook Potluck's firmware build calls for this component: its name is app_rows_<directory name>.

#include "hello/hello.hpp"

namespace pot {

const ActorKind* app_rows_pot_hello(size_t* n) {
    static const ActorKind kRows[] = {hello::peers_row()};
    *n = sizeof(kRows) / sizeof(kRows[0]);
    return kRows;
}

}  // namespace pot
