#ifndef __TEST_H__
#define __TEST_H__

#include <stdbool.h>
#include <stdint.h>

// Scripted input for headless tests (test.c): loads /cd/q64test.txt if the disc has one.
bool test_init(void);
// Whether this is a test disc (gfx.c then marks the screen with a red bar).
bool test_disc(void);
// Adds the script's buttons and stick to controller 1's state.
void test_input(uint16_t* buttons, int8_t* stick_x, int8_t* stick_y);
// Sets controller 1's triggers (0-255) to the script's, if it holds them.
void test_triggers(uint8_t* ltrig, uint8_t* rtrig);

#endif
