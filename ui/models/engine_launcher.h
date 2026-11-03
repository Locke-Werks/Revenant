// The launcher lives in core/launch so webgate shares it; this keeps the
// Qt client's unqualified names and include path working.

#pragma once

#include "core/launch/engine_launcher.h"

namespace revenant::ui {
using namespace revenant::launch;
}  // namespace revenant::ui
