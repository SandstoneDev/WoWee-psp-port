// System on-screen keyboard (sceUtilityOsk) for text entry: character names
// now, chat later.
//
// Frame rule for every PSP system dialog: the game keeps drawing its own
// frame, and sceUtilityOskUpdate() runs after the frame's GE list finished
// (sceGuSync) and before the buffer swap; the dialog renders on top. This
// module hooks that point in platform::psp::frameEnd, so screens only call
// oskBegin() and poll oskFinished().
//
// Memory: the dialog allocates from the user partition, not our heap - the
// 4 MB PSP_HEAP_THRESHOLD_SIZE_KB in main_psp.cpp is what it lives on (the
// source-psp port lost that memory to its heap and the OSK failed there).
#pragma once

#include <cstdint>
#include <string>

namespace wowee::platform::psp {

enum class OskCharset : uint8_t {
    Any,          // Latin + Cyrillic + digits (SELECT switches)
    LatinLetters,
    Cyrillic,
    Text,         // Chat - Cyrillic + Latin + digits + symbols ('/', '.', punctuation)
    Digits,       // the money boxes (MoneyInputFrame: numeric)
};

// Starts the keyboard. `title` and `initial` are UTF-8. Returns false (and
// logs the SDK code) when the dialog could not start.
bool oskBegin(const std::string& title, const std::string& initial, int maxChars,
              OskCharset charset = OskCharset::Any);
bool oskActive();
// True once, after the dialog closed. accepted = the player confirmed (text
// may still equal the initial value).
bool oskFinished(bool& accepted, std::string& textUtf8);

// Called by frameEnd() after sceGuSync, before the swap.
void oskAfterGeSync();

// A keyboard another part of a screen owns (the mail frame's edit boxes). The plain
// oskFinished() never hands out an owned session's text - the chat polls it every frame and would send a
// letter's body as a chat line; the owner takes it with oskFinishedFor(). owner != 0.
bool oskBeginFor(int owner, const std::string& title, const std::string& initial, int maxChars,
                 OskCharset charset = OskCharset::Any);
bool oskFinishedFor(int owner, bool& accepted, std::string& textUtf8);

} // namespace wowee::platform::psp
