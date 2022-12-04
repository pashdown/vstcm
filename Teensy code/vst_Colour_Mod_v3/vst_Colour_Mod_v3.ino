/*
   VSTCM

   Vector Signal Transceiver Colour Mod using MCP4922 DACs on the Teensy 4.1

   Based on: https://trmm.net/V.st by Trammell Hudson
   incorporating mods made by "Swapfile" (Github) for Advanced Mame compatibility

   robin@robinchampion.com 2022

   (This is a modified version by fcawth which attempts to speed things up)

*/

#include <Audio.h>
#include "advmame.h"
#include "drawing.h"
#include "settings.h"
#include "spi_fct.h"
#include "buttons.h"

// GUItool: begin automatically generated code
/*AudioPlaySdWav           playWav1;     //xy=146,440
AudioAmplifier           amp2;           //xy=350,430
AudioAmplifier           amp1;           //xy=367,492
AudioOutputI2S           i2s1;           //xy=565,433
AudioConnection          patchCord1(playWav1, 0, amp2, 0);
AudioConnection          patchCord2(playWav1, 1, amp1, 0);
AudioConnection          patchCord3(amp2, 0, i2s1, 0);
AudioConnection          patchCord4(amp1, 0, i2s1, 1);*/
// GUItool: end automatically generated code

// This works with audio out on VSTCM 3.1 prototype
AudioPlaySdWav playWav1;
// Use one of these 3 output types: Digital I2S, Digital S/PDIF, or Analog DAC
AudioOutputI2S audioOutput;
//AudioOutputSPDIF       audioOutput;
//AudioOutputAnalog      audioOutput;
//On Teensy LC, use this for the Teensy Audio Shield:
//AudioOutputI2Sslave    audioOutput;

AudioConnection patchCord1(playWav1, 0, audioOutput, 0);
AudioConnection patchCord2(playWav1, 1, audioOutput, 1);
//AudioControlSGTL5000 sgtl5000_1;

const int REST_X = 2048;  // Wait in the middle of the screen
const int REST_Y = 2048;

//For spot killer fix - if the total distance in x or y is less than SPOT_MAX, it will go to the corners to try to stop
//the spot killer from triggering
const int SPOT_MAX = 3400;
const int SPOT_GOTOMAX = 4076;
const int SPOT_GOTOMIN = 20;

bool spot_triggered;

//EXPERIMENTAL automatic draw rate adjustment based on how much idle time there is between frames
//Defines and global for the auto-speed feature
#define NORMAL_SHIFT_SCALING 2.0
#define MAX_DELTA_SHIFT 6  // These are the limits on the auto-shift for speeding up drawing complex frames
#define MIN_DELTA_SHIFT -3
#define DELTA_SHIFT_INCREMENT 0.1
#define SPEEDUP_THRESHOLD_MS 2   // If the dwell time is less than this then the drawing rate will try to speed up (lower resolution)
#define SLOWDOWN_THRESHOLD_MS 8  // If the dwell time is greater than this then the drawing rate will slow down (higher resolution)
//If the thresholds are too close together there can be "blooming" as the rate goes up and down too quickly - maybe make it limit the
//speed it can change??
float delta_shift = 0;

long fps;  // Approximate FPS used to benchmark code performance improvements

volatile bool show_vstcm_splash;    // Shows splash screen if true (usually if nothing else is running such as a a game)
volatile bool show_vstcm_settings;  // Shows settings screen if true
volatile bool show_something;       // Shows either settings or splash screen if true

unsigned long dwell_time = 0;

extern params_t v_setting[NB_SETTINGS];
extern float line_draw_speed;
extern int frame_max_x;
extern int frame_min_x;
extern int frame_max_y;
extern int frame_min_y;

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 4000)
    ;

  AudioMemory(8);  // Reserve memory for audio
                   // Comment these out if not using the audio adaptor board.
  // This may wait forever if the SDA & SCL pins lack
  // pullup resistors
  //sgtl5000_1.enable();
 // sgtl5000_1.volume(0.5);

  init_gamma();
  read_vstcm_config();  // Read saved settings from Teensy SD card
  IR_remote_setup();    // Configure the infra red remote control, if present
  buttons_setup();      // Configure buttons on vstcm for input using built in pullup resistors
  SPI_init();           // Set up pins on Teensy as well as SPI registers

  line_draw_speed = (float)v_setting[5].pval / NORMAL_SHIFT_SCALING;

  show_something = true;
  show_vstcm_splash = true;     // Start off showing the splash screen until serial data received
  show_vstcm_settings = false;  // Don't show settings screen until selected from splash screen

  make_test_pattern();  // Prepare buffer of data to draw test patterns quicker
}

void loop() {
  elapsedMicros waiting;  // Auto updating, used for FPS calculation
  unsigned long draw_start_time, loop_start_time;
  int serial_flag;

  frame_max_x = 0;
  frame_min_x = 4095;
  frame_max_y = 0;
  frame_min_y = 4095;

  serial_flag = 0;
  loop_start_time = millis();

  if (!Serial) {
    read_data(1);  //init read_data if the serial port is not open
    Serial.flush();
  }

  draw_start_time = 0;  // Just to prevent a compiler warning

  while (1) {
    if (Serial.available()) {
      if (serial_flag == 0) {
        draw_start_time = millis();
        serial_flag = 1;
      }

      show_something = false;  // Turn off splash or settings screen

      if (read_data(0) == 1)  // Try to read some incoming data from MAME
        break;
    } else if ((millis() - loop_start_time) > SERIAL_WAIT_TIME)  //Changed this to check only if serial is not available
      show_something = true;                                     // Show splash screen

    if (show_something)
      break;
  }

  dwell_time = draw_start_time - loop_start_time;  //This is how long it waited after drawing a frame - better than FPS for tuning

  if (show_something) {
    delta_shift = 0;
    line_draw_speed = (float)v_setting[5].pval / NORMAL_SHIFT_SCALING + 3.0;  //Make things a little bit faster for the menu

    if (show_vstcm_splash)
      show_vstcm_splash_screen();  // Show splash screen and manage associated control buttons

    if (show_vstcm_settings)
      show_vstcm_settings_screen();  // Show settings screen and manage associated control buttons
  } else {
    if (dwell_time < SPEEDUP_THRESHOLD_MS) {
      delta_shift += DELTA_SHIFT_INCREMENT;

      if (delta_shift > MAX_DELTA_SHIFT)
        delta_shift = MAX_DELTA_SHIFT;
    }
    //Try to only allow speedups
    //   else if (dwell_time > SLOWDOWN_THRESHOLD_MS) {
    //     delta_shift -= DELTA_SHIFT_INCREMENT;
    //     if (delta_shift < MIN_DELTA_SHIFT) delta_shift = MIN_DELTA_SHIFT;
    //   }

    line_draw_speed = (float)v_setting[5].pval / NORMAL_SHIFT_SCALING + delta_shift;

    if (line_draw_speed < 1)
      line_draw_speed = 1;
  }

  // Go to the center of the screen, turn the beam off (prevents stray coloured lines from appearing)
  brightness(0, 0, 0);
  dwell(v_setting[3].pval);

  if (!show_something) {
    if (((frame_max_x - frame_min_x) < SPOT_MAX) || ((frame_max_y - frame_min_y) < SPOT_MAX) || (dwell_time > 10)) {
      spot_triggered = true;
      draw_moveto(SPOT_GOTOMAX, SPOT_GOTOMAX);
      SPI_flush();
      if (dwell_time > 5) delayMicroseconds(200);
      else delayMicroseconds(100);
      draw_moveto(SPOT_GOTOMIN, SPOT_GOTOMIN);
      SPI_flush();
      if (dwell_time > 5) delayMicroseconds(200);
      else delayMicroseconds(100);
      if (dwell_time > 10)                        // For really long dwell times, do the moves again
        draw_moveto(SPOT_GOTOMAX, SPOT_GOTOMAX);  //If we have time, do the moves again
      SPI_flush();
      delayMicroseconds(200);
      draw_moveto(SPOT_GOTOMIN, SPOT_GOTOMIN);  //Try to move back to the min again
      SPI_flush();
      delayMicroseconds(200);
    } else spot_triggered = false;
  }

  goto_xy(REST_X, REST_Y);
  SPI_flush();

  if (show_something)  // If we are not playing MAME, we need to show either of the menu screens instead
    manage_buttons();  //Moved here to avoid bright spot on the monitor when doing SD card operations

  fps = 1000000 / waiting;

  if (show_something)
    delay(5);  //The 6100 monitor likes to spend some time in the middle
  else
    delayMicroseconds(100);  //Wait 100 microseconds in the center if displaying a game (tune this?)
}