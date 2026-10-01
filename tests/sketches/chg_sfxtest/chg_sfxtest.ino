// Plays CHChess's capture effect every 1.5 s: the sound tools/fit_piezo.py
// fits the emulator's piezo filter to (record the device playing it).
//
// Needs CHChess's own sound code next to this file: copy Audio.cpp and
// Audio.h from https://github.com/bateske/CHChess (src/audio, MPL-2.0), then
//   arduino-cli compile -b CHGame:ch32v:CHGame:periph=game .
#include "Audio.h"

void setup() { audio::begin(true); delay(200); }
void loop() { audio::sfx(Sfx::Capture); delay(1500); }
