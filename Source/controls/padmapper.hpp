#pragma once

#include <string_view>

#include "controls/controller_buttons.h"
#include "options.h"

namespace devilution {

void PadmapperPress(ControllerButton button, const PadmapperOptions::Action &action);
void PadmapperRelease(ControllerButton button, bool invokeAction);
void PadmapperReleaseAllActiveButtons();
/** @brief Removes a runtime action. If its button is down, this first runs the release, as a button up would. */
void PadmapperRemoveRuntimeAction(std::string_view key);
[[nodiscard]] bool PadmapperIsActionActive(std::string_view actionName);
[[nodiscard]] std::string_view PadmapperActionNameTriggeredByButtonEvent(ControllerButtonEvent ctrlEvent);

} // namespace devilution
