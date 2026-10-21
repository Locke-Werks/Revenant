// EngineLink's side of the constellation and eye panel (#17): the one flag
// that asks the engine for a receiver's symbol scope. The scope itself rides
// on the passband frame and is read by render/constellation_item.h and
// render/eye_item.h.

#include "models/engine_link.h"

namespace revenant::ui {

void EngineLink::setDisplaySymbols(bool on)
{
    if (wanted_.display_symbols == on) {
        return;
    }
    wanted_.display_symbols = on;
    if (receiverId() == 0) {
        emit receiverChanged();
        return;
    }
    post_receiver_request(false);
}

}  // namespace revenant::ui
