#pragma once

namespace punto {

// Short bundled cue (data/sounds/switch.wav). Played when the daemon
// converts a word or fixes a typo. Does not wait for playback to finish.
void playActionSound(bool enabled);

} // namespace punto
