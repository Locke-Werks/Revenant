// The launch rules live in core/launch so webgate shares them; this keeps the
// Qt client's unqualified names and include path working.

#pragma once

#include "core/launch/engine_start.h"

namespace revenant::ui {
using namespace revenant::launch;
}  // namespace revenant::ui
