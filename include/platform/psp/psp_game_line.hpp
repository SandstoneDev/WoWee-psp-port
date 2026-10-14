// Where a game line shows - the port's own uniform rule for the GlobalStrings ERR_* keys and similar notices:
//   - Refused: the line says that something the player tried was refused or failed -> the red error line
//     (UIErrorsFrame, the UI_ERROR_MESSAGE colour);
//   - News: the line reports news - a success, another player's action, a change of state -> a system chat line.
// Where upstream WoWee shows the same message in one place only, that place is used and the call site cites it.
// No extra sound is played for either kind.
#pragma once

#include <cstdint>

namespace wowee::platform::psp {

enum class GameLine : uint8_t { Refused, News };

}  // namespace wowee::platform::psp
