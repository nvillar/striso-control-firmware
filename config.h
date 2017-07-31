#ifndef _CONFIG_H_
#define _CONFIG_H_

//#define USE_BAS
//#define USE_UART
#define USE_AUX_BUTTONS
#define USE_MIDI_OUT
//#define USE_SYNTH_INTERFACE
#define USE_MPU6050
#define USE_WS2812

//#define CALIBRATION_MODE TRUE

#define AUX_BUTTON_DEBOUNCE_TIME 5

#define MAX_VOICECOUNT 15

typedef struct struct_config {
  int message_interval;
  int send_usb_bulk;
  int send_motion_interval;
  int midi_pres;
  int midi_bend;
} config_t;
extern config_t config;

#ifdef CONFIG_HERE
config_t config = {
  .message_interval = 10,     // interval in ms
  .send_usb_bulk = 0,         // send Striso binary protocol
  .send_motion_interval = 0,  // 0 = disable, else x10ms
  .midi_pres = 1,             // 1 = Channel Pressure, 2 = CC 70
  .midi_bend = 1,             // 2 = CC 71
};
#endif

#endif