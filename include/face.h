#pragma once
#include <Adafruit_SSD1306.h>

enum class Expression
{
    Idle,
    Forward,
    Backward,
    Left,
    Right,
    Happy,
    Sleep, // set by the face itself after SLEEP_MS without a command
    Play,  // then PLAY_MS later it wakes up and plays games, a new one every GAME_MS
    Dance, // the dance command, hops and sways like the Dance game
};

// switch expression, the eyes ease over to it
// returns the expression shown (Idle right after a move turns Happy for a moment)
Expression faceSet(Expression expression);

// call every loop, redraws only when the face changed (force = redraw now)
void faceUpdate(Adafruit_SSD1306 &display, bool force);

// where the eyes are looking now, -1 screen left .. 1 screen right
float faceLookX();

// sd card mp3 that goes with what the face is doing now, 0 = quiet (Dance is 0, main.cpp picks the dance's own)
int faceTrack();
