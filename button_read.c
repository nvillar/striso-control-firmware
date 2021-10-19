/**
 * Copyright (C) 2019 Piers Titus van der Torren
 *
 * This file is part of Striso Control.
 *
 * Striso Control is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * Striso Control is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * Striso Control. If not, see <http://www.gnu.org/licenses/>.
 */
#include "button_read.h"
#include "ch.h"
#include "hal.h"

#include "ccportab.h"

#include "config.h"
#include "striso.h"
#include "synth.h"
#include "usbcfg.h"
#include "led.h"

#include "messaging.h"
#ifdef STM32F4XX
#include "adc_multi.h"
#endif

#define INTERNAL_ONE (1<<24)
#define ADCFACT (1<<12)
#define VELOFACT 32
#define MSGFACT (1<<11)
#define MSGFACT_VELO (MSGFACT/VELOFACT)
#define FILT 8  // min: 1 (no filter), max: 64 (1<<32 / INTERNAL_ONE)
#define FILTV 8 // min: 1 (no filter), max: 64 (1<<32 / INTERNAL_ONE)
#define ZERO_LEVEL_OFFSET CALIB_OFFSET
#define ZERO_LEVEL_MAX_VELO 500
#define COMMON_CHANNEL_FILT 0.5

#define INTEGRATED_PRES_TRESHOLD (INTERNAL_ONE/8)
#define SENDFACT    config.message_interval

#ifdef STM32F4XX
//#define ADC_SAMPLE_DEF ADC_SAMPLE_3   // 0.05 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_15  // 0.11 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_28  // 0.18 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_56  // 0.33 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_84  // 0.50 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_112 // 0.65 ms per cycle
#define ADC_SAMPLE_DEF ADC_SAMPLE_144 // 0.83 ms per cycle
//#define ADC_SAMPLE_DEF ADC_SAMPLE_480 // 2.7 ms per cycle

/* Number of ADCs used in multi ADC mode (2 or 3) */
#define ADC_N_ADCS 3

/* Total number of channels to be sampled by a single ADC operation.*/
#define ADC_GRP1_NUM_CHANNELS_PER_ADC   2

/* Depth of the conversion buffer, channels are sampled one time each.*/
#define ADC_GRP1_BUF_DEPTH      1 // must be 1 or even

#define ADC_GRP1_NUM_CHANNELS (ADC_GRP1_NUM_CHANNELS_PER_ADC * ADC_N_ADCS)

#elif defined(STM32H7XX)
// timing calculation: ADCCLK/(SMP+6.5)/51/4 (*2 for dual ADC)
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_1P5   //
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_2P5   //
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_8P5   //
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_16P5  //
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_32P5 // 1174 Hz @10MHz single ADC measured (1257 Hz calculated)
#define ADC_SAMPLE_DEF ADC_SMPR_SMP_64P5  // 1289 Hz @10MHz dual ADC measured (1380 Hz calculated)
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_384P5 // 123.6 Hz @10MHz single ADC measured (125.4 Hz calculated) 245 Hz dual ADC
//#define ADC_SAMPLE_DEF ADC_SMPR_SMP_810P5 //

/* Total number of channels to be sampled by a single ADC operation.*/
#define ADC_GRP1_NUM_CHANNELS   4

/* Depth of the conversion buffer, channels are sampled one time each.*/
#define ADC_GRP1_BUF_DEPTH      1 // must be 1 or even. Strange behaviour when it is 2.

#endif

#define ADC_OFFSET (16>>1)

#define OUT_NUM_CHANNELS        51
#define N_BUTTONS               68
#define N_BUTTONS_BAS           51

static const ioportid_t out_channels_port[51] = {
  GPIOC, GPIOC, GPIOC, GPIOG, GPIOG, GPIOG, GPIOG, GPIOG,
  GPIOG, GPIOG, GPIOD, GPIOD, GPIOD, GPIOD, GPIOD, GPIOD, GPIOD,
  GPIOD, GPIOB, GPIOB, GPIOB, GPIOH, GPIOH, GPIOH, GPIOH,
  GPIOH, GPIOH, GPIOH, GPIOB, GPIOB, GPIOE, GPIOE, GPIOE, GPIOE,
  GPIOE, GPIOE, GPIOE, GPIOE, GPIOE, GPIOG, GPIOG, GPIOF,
  GPIOF, GPIOF, GPIOF, GPIOF, GPIOB, GPIOB, GPIOC, GPIOC, GPIOA,
};
static const iopadid_t out_channels_pad[51] = {
   8,  7,  6,  8,  7,  6,  5,  4,
   3,  2, 15, 14, 13, 12, 11, 10,  9,
   8, 15, 14, 13, 12, 11, 10,  9,
   8,  7,  6, 11, 10, 15, 14, 13, 12,
  11, 10,  9,  8,  7,  1,  0, 15,
  14, 13, 12, 11,  1,  0,  5,  4,  7,
};
static const ioportmask_t out_channels_portmask[51] = {
  1<< 8, 1<< 7, 1<< 6, 1<< 8, 1<< 7, 1<< 6, 1<< 5, 1<< 4,
  1<< 3, 1<< 2, 1<<15, 1<<14, 1<<13, 1<<12, 1<<11, 1<<10, 1<< 9,
  1<< 8, 1<<15, 1<<14, 1<<13, 1<<12, 1<<11, 1<<10, 1<< 9,
  1<< 8, 1<< 7, 1<< 6, 1<<11, 1<<10, 1<<15, 1<<14, 1<<13, 1<<12,
  1<<11, 1<<10, 1<< 9, 1<< 8, 1<< 7, 1<< 1, 1<< 0, 1<<15,
  1<<14, 1<<13, 1<<12, 1<<11, 1<< 1, 1<< 0, 1<< 5, 1<< 4, 1<< 7,
};

#ifdef USE_BAS
static const ioportid_t out_channels_bas_port[51] = {
  GPIOA, GPIOC, GPIOC, GPIOC, GPIOD, GPIOD, GPIOD, GPIOD, GPIOD,
  GPIOD, GPIOD, GPIOD, GPIOG, GPIOG, GPIOG, GPIOG, GPIOG,
  GPIOG, GPIOG, GPIOB, GPIOB, GPIOB, GPIOB, GPIOB, GPIOB, GPIOB,
  GPIOE, GPIOE, GPIOI, GPIOI, GPIOI, GPIOI, GPIOE, GPIOE,
  GPIOE, GPIOE, GPIOE, GPIOI, GPIOI, GPIOI, GPIOF, GPIOF, GPIOF,
  GPIOF, GPIOF, GPIOF, GPIOF, GPIOF, GPIOF, GPIOF, GPIOF,
};
static const iopadid_t out_channels_bas_pad[51] = {
  15, 10, 11, 12,  0,  1,  2,  3,  4,
   5,  6,  7,  9, 10, 11, 12, 13,
  14, 15,  3,  4,  5,  6,  7,  8,  9,
   0,  1,  4,  5,  6,  7,  2,  3,
   4,  5,  6,  9, 10, 11,  0,  1,  2,
   3,  4,  5,  6,  7,  8,  9, 10,
};
static const ioportmask_t out_channels_bas_portmask[51] = {
  1<<15, 1<<10, 1<<11, 1<<12, 1<< 0, 1<< 1, 1<< 2, 1<< 3, 1<< 4,
  1<< 5, 1<< 6, 1<< 7, 1<< 9, 1<<10, 1<<11, 1<<12, 1<<13,
  1<<14, 1<<15, 1<< 3, 1<< 4, 1<< 5, 1<< 6, 1<< 7, 1<< 8, 1<< 9,
  1<< 0, 1<< 1, 1<< 4, 1<< 5, 1<< 6, 1<< 7, 1<< 2, 1<< 3,
  1<< 4, 1<< 5, 1<< 6, 1<< 9, 1<<10, 1<<11, 1<< 0, 1<< 1, 1<< 2,
  1<< 3, 1<< 4, 1<< 5, 1<< 6, 1<< 7, 1<< 8, 1<< 9, 1<<10,
};
#endif // USE_BAS

static int cur_channel = 0;
static int next_conversion = 0;
static int proc_conversion = 0;
static int common_channel = 0;

enum button_status {
  OFF = 0,
  STARTING = 1,
  ON = 2,
  PHANTOM_FLAG = 4,
};

typedef struct struct_button button_t;
struct struct_button {
  int32_t s0;
  int32_t s1;
  int32_t s2;
  int32_t p;
  int32_t v0;
  int32_t v1;
  int32_t v2;
  int32_t c_force;
  int32_t c_offset;
  int32_t c_breakpoint;
  int32_t c_force2;
  int32_t zero_time;
  int32_t zero_max;
  enum button_status status;
  int timer;
  int but_id;
  int src_id;
  button_t* prev_but;
};

typedef struct struct_slider {
  int32_t s[27];
  int32_t v[27];
  int timer;
  int dbtimer;
  int pres[4];
  int pos[4];
  int velo[4];
  int sort[4];
  int n_press;
  int move;
  int zoom;
} slider_t;

static slider_t sld;

static button_t buttons[N_BUTTONS];
#ifdef USE_BAS
static button_t buttons_bas[N_BUTTONS_BAS];
#endif
static int buttons_pressed[2] = {0};
static int col_pressed[2][17] = {0};
static int32_t max_pres = 0, max_pres1 = 0;

static float octave_factor[6] = {1.0f};
static int32_t octave_sum[6] = {0};

#ifdef USE_AUX_BUTTONS
// #define LINE_BUTTON_PORT   PAL_LINE(GPIOI,  2U)
// #define LINE_BUTTON_UP     PAL_LINE(GPIOI,  1U)
// #define LINE_BUTTON_DOWN   PAL_LINE(GPIOA,  9U) // GPIOA_UART1_TX
// #define LINE_BUTTON_ALT    PAL_LINE(GPIOA, 10U) // GPIOA_UART1_RX

static const ioline_t aux_buttons_line[4] = {LINE_BUTTON_PORT, LINE_BUTTON_UP, LINE_BUTTON_DOWN, LINE_BUTTON_ALT};
static const bool aux_buttons_on[4] = {false, true, false, false};
static const int aux_buttons_msg[4] = {IDC_PORTAMENTO, IDC_OCT_UP, IDC_OCT_DOWN, IDC_ALT};
static uint32_t aux_buttons_state[4] = {0};
#endif

/*
 * ADC samples buffer.
 */
/* Buffers are allocated with size and address aligned to the cache
   line size.*/
#if CACHE_LINE_SIZE > 0
#define CC_CACHE_ALIGN CC_ALIGN(CACHE_LINE_SIZE)
#else
#define CC_CACHE_ALIGN
#endif
//CC_SECTION(".ram3")
CC_CACHE_ALIGN static adcsample_t adc_samples[CACHE_SIZE_ALIGN(adcsample_t, ADC_GRP1_NUM_CHANNELS * ADC_GRP1_BUF_DEPTH)];
// static adcsample_t adc_samples[ADC_GRP1_NUM_CHANNELS * ADC_GRP1_BUF_DEPTH];
static adcsample_t samples0[102] = {0};
static adcsample_t samples1[102] = {0};
static adcsample_t samples2[102] = {0};
static adcsample_t samples3[102] = {0};
static adcsample_t* samples[4] = {samples0, samples1, samples2, samples3};

#ifdef USE_BAS
static adcsample_t samples_bas0[102] = {0};
static adcsample_t samples_bas1[102] = {0};
static adcsample_t* samples_bas[2] = {samples_bas0, samples_bas1};
#endif

static adcsample_t samples_common[6] = {0};

static thread_t *tpReadButtons = NULL;

static void adccallback(ADCDriver *adcp) {
  adcsample_t *buffer = adcp->samples; // = adc_samples

#ifdef COMMON_CHANNEL_FILT
  /* process common channel sampling (for crosstalk compensation) */
  if (common_channel) {
    /* Open old channels */
    for (int c = 0; c<51; c++) {
      palSetPort(out_channels_port[c], out_channels_portmask[c]);
    }

    /* Drain new channel */
    palClearPort(out_channels_port[cur_channel], out_channels_portmask[cur_channel]);
#ifdef USE_BAS
    palClearPort(out_channels_bas_port[cur_channel], out_channels_bas_portmask[cur_channel]);
#endif

    /* copy adc_samples */
    cacheBufferInvalidate(adc_samples, sizeof (adc_samples) / sizeof (adcsample_t));
    samples_common[0] = buffer[0];
    samples_common[1] = buffer[1];
    samples_common[2] = buffer[2];
    samples_common[3] = buffer[3];
#ifdef USE_BAS
    samples_common[4] = buffer[4];
    samples_common[5] = buffer[5];
#endif

    common_channel = 0;

    // start next ADC conversion
#if defined(STM32F4XX)
    adcp->adc->CR2 |= ADC_CR2_SWSTART;
#elif defined(STM32H7XX)
    adcp->adcm->CR |= ADC_CR_ADSTART;
#endif
    return;
  }
#endif // COMMON_CHANNEL_FILT

  /* Open old channel */
  palSetPort(out_channels_port[cur_channel], out_channels_portmask[cur_channel]);
#ifdef USE_BAS
  palSetPort(out_channels_bas_port[cur_channel], out_channels_bas_portmask[cur_channel]);
#endif

  cur_channel = (next_conversion+1) % OUT_NUM_CHANNELS;

#ifdef COMMON_CHANNEL_FILT
  /* Start common channel sampling before next channel */
  if (cur_channel == 0) {
    /* Drain all channels */
    for (int c = 0; c<51; c++) {
      palClearPort(out_channels_port[c], out_channels_portmask[c]);
    }
    common_channel = 1;
  }
#endif

#ifdef TWO_WAY_SAMPLING
  if ((next_conversion+1) % 102 >= 51) {
    cur_channel -= ((cur_channel % 3) - 1) * 2;
  }
#endif
  /* Drain new channel */
  palClearPort(out_channels_port[cur_channel], out_channels_portmask[cur_channel]);
#ifdef USE_BAS
  palClearPort(out_channels_bas_port[cur_channel], out_channels_bas_portmask[cur_channel]);
#endif

  cacheBufferInvalidate(adc_samples, sizeof (adc_samples) / sizeof (adcsample_t));
  /* copy adc_samples */
  samples0[next_conversion] = buffer[0];
  samples1[next_conversion] = buffer[1];
  samples2[next_conversion] = buffer[2];
  samples3[next_conversion] = buffer[3];

#ifdef USE_BAS
  samples_bas0[next_conversion] = buffer[4];
  samples_bas1[next_conversion] = buffer[5];
#endif

  next_conversion = (next_conversion+1) % 102;

  // start next ADC conversion
#if defined(STM32F4XX)
  adcp->adc->CR2 |= ADC_CR2_SWSTART;
#elif defined(STM32H7XX)
  adcp->adcm->CR |= ADC_CR_ADSTART;
#endif

  // Wake up processing thread
  chSysLockFromISR();
  if (tpReadButtons != NULL && (next_conversion % 3) == 2) {
    chSchReadyI(tpReadButtons);
    tpReadButtons = NULL;
  }
  chSysUnlockFromISR();
}

#ifdef STM32F4XX
/*
 * ADC conversion group for ADC0 as multi ADC mode master.
 * Mode:        Circular buffer, triple ADC mode master, SW triggered.
 * Channels:    PA0, PA3
 */
static const ADCConversionGroup adcgrpcfg1 = {
  TRUE, // Circular conversion
  ADC_GRP1_NUM_CHANNELS_PER_ADC * ADC_N_ADCS,
  adccallback, /* end of conversion callback */
  NULL, /* error callback */
  /* HW dependent part.*/
  0, // CR1
  ADC_CR2_SWSTART, // CR2
  0, // SMPR1
  ADC_SMPR2_SMP_AN0(ADC_SAMPLE_DEF)
   | ADC_SMPR2_SMP_AN3(ADC_SAMPLE_DEF), // SMPR2
  0, // HTR
  0, // LTR
  ADC_SQR1_NUM_CH(ADC_GRP1_NUM_CHANNELS_PER_ADC), // SQR1
  0, // SQR2
  ADC_SQR3_SQ1_N(ADC_CHANNEL_IN0)
   | ADC_SQR3_SQ2_N(ADC_CHANNEL_IN3) // SQR3
};

/*
 * ADC conversion group for ADC2.
 * Mode:        triple ADC mode slave.
 * Channels:    PA1, PC0
 */
static const ADCConversionGroup adcgrpcfg2 = {
  TRUE,
  0,
  NULL, /* end of conversion callback */
  NULL, /* error callback */
  /* HW dependent part.*/
  0, // CR1
  0, // CR2
  ADC_SMPR1_SMP_AN12(ADC_SAMPLE_DEF), // SMPR1
  ADC_SMPR2_SMP_AN1(ADC_SAMPLE_DEF), // SMPR2
  0, // HTR
  0, // LTR
  ADC_SQR1_NUM_CH(ADC_GRP1_NUM_CHANNELS_PER_ADC), // SQR1
  0, // SQR2
  ADC_SQR3_SQ1_N(ADC_CHANNEL_IN1)
   | ADC_SQR3_SQ2_N(ADC_CHANNEL_IN12) // SQR3
};

/*
 * ADC conversion group for ADC3.
 * Mode:        triple ADC mode slave.
 * Channels:    PA2, PC2
 */
static const ADCConversionGroup adcgrpcfg3 = {
  TRUE,
  0,
  NULL, /* end of conversion callback */
  NULL, /* error callback */
  /* HW dependent part.*/
  0, // CR1
  0, // CR2
  ADC_SMPR1_SMP_AN10(ADC_SAMPLE_DEF), // SMPR1
  ADC_SMPR2_SMP_AN2(ADC_SAMPLE_DEF), // SMPR2
  0, // HTR
  0, // LTR
  ADC_SQR1_NUM_CH(ADC_GRP1_NUM_CHANNELS_PER_ADC), // SQR1
  0, // SQR2
  ADC_SQR3_SQ1_N(ADC_CHANNEL_IN2)
   | ADC_SQR3_SQ2_N(ADC_CHANNEL_IN10) // SQR3
};
#endif

#ifdef STM32H7XX
const ADCConfig adccfg1 = {
  .difsel       = 0U,
  .calibration  = 0U
};
#if STM32_ADC_DUAL_MODE == TRUE
/*
 * ADC conversion group 1.
 * Mode:        One shot, 2 channels, SW triggered.
 * Channels:    IN0, IN5.
 */
const ADCConversionGroup adcgrpcfg1 = {
  .circular     = TRUE,
  .num_channels = ADC_GRP1_NUM_CHANNELS,
  .end_cb       = adccallback,
  .error_cb     = NULL,
  .cfgr         = ADC_CFGR_RES_12BITS,
  .cfgr2        = 0U,
  .ccr          = ADC_CCR_DUAL_REG_SIMULT, // 6U
  .pcsel        = ADC_SELMASK_IN3 | ADC_SELMASK_IN19 | ADC_SELMASK_IN18 | ADC_SELMASK_IN15,
  .ltr1         = 0x00000000U,
  .htr1         = 0x03FFFFFFU,
  .ltr2         = 0x00000000U,
  .htr2         = 0x03FFFFFFU,
  .ltr3         = 0x00000000U,
  .htr3         = 0x03FFFFFFU,
  .smpr         = {
    0U,
    ADC_SMPR2_SMP_AN15(ADC_SAMPLE_DEF) | ADC_SMPR2_SMP_AN19(ADC_SAMPLE_DEF)
  },
  .sqr          = {
    ADC_SQR1_SQ1_N(ADC_CHANNEL_IN15) | ADC_SQR1_SQ2_N(ADC_CHANNEL_IN19),
    0U,
    0U,
    0U
  },
  .ssmpr        = {
    ADC_SMPR1_SMP_AN3(ADC_SAMPLE_DEF),
    ADC_SMPR2_SMP_AN18(ADC_SAMPLE_DEF)
  },
  .ssqr         = {
    ADC_SQR1_SQ1_N(ADC_CHANNEL_IN18) | ADC_SQR1_SQ2_N(ADC_CHANNEL_IN3),
    0U,
    0U,
    0U
  }
};
#else // STM32_ADC_DUAL_MODE == FALSE
const ADCConversionGroup adcgrpcfg1 = {
  .circular     = TRUE,
  .num_channels = ADC_GRP1_NUM_CHANNELS,
  .end_cb       = adccallback,
  .error_cb     = NULL,
  .cfgr         = ADC_CFGR_RES_12BITS,
  .cfgr2        = 0U,
  .ccr          = 0U,
  .pcsel        = ADC_SELMASK_IN3 | ADC_SELMASK_IN19 | ADC_SELMASK_IN18 | ADC_SELMASK_IN15,
  .ltr1         = 0x00000000U,
  .htr1         = 0x03FFFFFFU,
  .ltr2         = 0x00000000U,
  .htr2         = 0x03FFFFFFU,
  .ltr3         = 0x00000000U,
  .htr3         = 0x03FFFFFFU,
  .smpr         = {
    ADC_SMPR1_SMP_AN3(ADC_SAMPLE_DEF),
    ADC_SMPR2_SMP_AN19(ADC_SAMPLE_DEF) |
    ADC_SMPR2_SMP_AN18(ADC_SAMPLE_DEF) |
    ADC_SMPR2_SMP_AN15(ADC_SAMPLE_DEF)
  },
  .sqr          = {
    ADC_SQR1_SQ1_N(ADC_CHANNEL_IN15) | ADC_SQR1_SQ2_N(ADC_CHANNEL_IN18) |
    ADC_SQR1_SQ3_N(ADC_CHANNEL_IN19) | ADC_SQR1_SQ4_N(ADC_CHANNEL_IN3),
    0U,
    0U,
    0U
  }
};
#endif // STM32_ADC_DUAL_MODE
#endif // STM32H7XX

// Schlick power function, approximation of power function
float powf_schlick(const float a, const float b) {
  return (a / (b - a * b + a));
}

/*
 * Second order Kalman like filter with fast signal end conditions
 */
void update_and_filter(int32_t* s, int32_t* v, int32_t s_new) {
  int32_t old_s = *s;
  *s = ((FILT-1) * (old_s + *v) + s_new) / FILT;
  if (*s < 0 && old_s < 0) {
    *v = 0;
  } else if (*s >= INTERNAL_ONE) {
    *s = INTERNAL_ONE - 1;
    *v = 0;
  } else {
    *v = ((FILTV-1) * (*v) + (*s - old_s)) / FILTV;
    if (*v >= (INTERNAL_ONE/VELOFACT)) {
      *v = (INTERNAL_ONE/VELOFACT) - 1;
    } else if (*v <= -(INTERNAL_ONE/VELOFACT)) {
      *v = -(INTERNAL_ONE/VELOFACT) + 1;
    }
  }
}

int32_t linearize(int32_t s) {
#ifdef CALIBRATION_MODE
  /* keep linear voltage for calibration */
  return ADCFACT * (4095-s);
#else
  /* convert adc value to force */
  return (ADCFACT>>6) * (4095-s)/(s+1);
#endif // CALIBRATION_MODE
}

int32_t calibrate(int32_t s, button_t* but) {
#ifdef CALIBRATION_MODE
  return s;
#endif
  // c is the normalisation value for the force
  //    2^18   * 2^12 / 2^12 * ADCFACT/2^6 / c
  // s = (but->c_force * (4095-s)/(s+1)) * (ADCFACT>>6);
  s -= but->c_offset;
  s = but->c_force * s;
  #ifdef BREAKPOINT_CALIBRATION
  // breakpoint calibration
  if (s > but->c_breakpoint) {
    s += but->c_force2 * ((s - but->c_breakpoint)>>8);
  }
  #endif // BREAKPOINT_CALIBRATION
  return s;
}

#define max(x,y) (x>y?x:y)
#define min(x,y) (x<y?x:y)

void update_button(button_t* but, adcsample_t* inp) {
  int but_id = but->but_id;
  int oct = but_id/17;
  int32_t s_new;
  int32_t s_max = 0;
  int msg[8];
  msg[0] = but->src_id;

#ifdef TWO_WAY_SAMPLING
  s_new = linearize(max(inp[0], inp[53]));
  if (s_new > s_max) s_max = s_new;
  octave_sum[oct] += s_new;
  s_new = s_new * octave_factor[oct];
  s_new = calibrate(s_new, but);
  update_and_filter(&but->s0, &but->v0, s_new);
  s_new = linearize(max(inp[1], inp[52]));
  if (s_new > s_max) s_max = s_new;
  octave_sum[oct] += s_new;
  s_new = s_new * octave_factor[oct];
  s_new = calibrate(s_new, but);
  update_and_filter(&but->s1, &but->v1, s_new);
  s_new = linearize(max(inp[2], inp[51]));
  if (s_new > s_max) s_max = s_new;
  octave_sum[oct] += s_new;
  s_new = s_new * octave_factor[oct];
  s_new = calibrate(s_new, but);
  update_and_filter(&but->s2, &but->v2, s_new);
#else
  s_new = linearize(inp[0]);
  if (s_new > s_max) s_max = s_new;
  octave_sum[oct] += s_new;
  s_new = s_new * octave_factor[oct];
  s_new = calibrate(s_new, but);
  update_and_filter(&but->s0, &but->v0, s_new);
  s_new = linearize(inp[1]);
  if (s_new > s_max) s_max = s_new;
  octave_sum[oct] += s_new;
  s_new = s_new * octave_factor[oct];
  s_new = calibrate(s_new, but);
  update_and_filter(&but->s1, &but->v1, s_new);
  s_new = linearize(inp[2]);
  if (s_new > s_max) s_max = s_new;
  octave_sum[oct] += s_new;
  s_new = s_new * octave_factor[oct];
  s_new = calibrate(s_new, but);
  update_and_filter(&but->s2, &but->v2, s_new);
#endif
  but->p = but->s0 + but->s1 + but->s2;

#ifdef DETECT_STUCK_NOTES
  // adjust zero pressure level dynamically
  if (s_max < (INTERNAL_ONE/32)
      && (but->v0 + but->v1 + but->v2) < ZERO_LEVEL_MAX_VELO
      && (but->v0 + but->v1 + but->v2) > -ZERO_LEVEL_MAX_VELO) {
    if (s_max > but->zero_max) but->zero_max = s_max;
    but->zero_time++;
    if (but->zero_time > 500) {
      but->zero_time = 0;
      but->c_offset = but->zero_max + ZERO_LEVEL_OFFSET;
    }
  } else {
    but->zero_time = 0;
    but->zero_max = 0;
  }
#endif

#ifdef BUTTON_FILT
#ifdef TWO_WAY_SAMPLING
  int min_pres1 = max_pres/32;
#else
  int min_pres1 = ((but->prev_but->s2 > MSGFACT) + (but->prev_but->s2 > INTERNAL_ONE/4)) * (INTERNAL_ONE/64) // stop ADC reaction time phantom presses
    + max_pres/32;   // stop three nearby corner phantom presses TODO: fix for bas
#endif
  int min_pres = min_pres1 + __USAT(buttons_pressed[0], 2) * (INTERNAL_ONE/256); // reduce sensitivity a bit when many buttons are pressed TODO: fix for bas
#else
  int min_pres1 = max_pres/32;
  int min_pres = min_pres1;
#endif
  if (but->s0 > MSGFACT + min_pres || but->s1 > MSGFACT + min_pres || but->s2 > MSGFACT + min_pres) {
    // if button is off start integration timer
    if (but->status == OFF) {
      but->status = STARTING;
      but->timer = INTEGRATED_PRES_TRESHOLD;
      buttons_pressed[but->src_id]++;
      col_pressed[but->src_id][but_id % 17]++;
    }
    // if button is in start integration reduce timer
    if (but->status == STARTING) {
      but->timer -= but->p; //(but->s0 + but->s1 + but->s2);
    }
    // if button is phantom pressed release it
    if (but->status & PHANTOM_FLAG) {
      if (but->status == (ON | PHANTOM_FLAG)) {
          msg[1] = but_id;
          msg[2] = 0;
          msg[3] = 0;
          msg[4] = 0;
          msg[5] = 0;
          msg[6] = 0;
          msg[7] = 0;
          msgSend(8, msg);
      }
      // reset phantom flag since next round it can change
      but->status = STARTING;
    }
    // if integration is succesful and interval is ready send note message
    else if (--but->timer <= 0) {
      but->status = ON;
      msg[1] = but_id;
      if (but->s0 <= 0)
        msg[2] = 0;
      else
        msg[2] = but->s0 / MSGFACT;
        //msg[2] = (int32_t)(vsqrtf((float)but->s0 / INTERNAL_ONE) * (INTERNAL_ONE / MSGFACT));
      if (but->s1 <= 0)
        msg[3] = 0;
      else
        msg[3] = but->s1 / MSGFACT;
        //msg[3] = (int32_t)(vsqrtf((float)but->s1 / INTERNAL_ONE) * (INTERNAL_ONE / MSGFACT));
      if (but->s2 <= 0)
        msg[4] = 0;
      else
        msg[4] = but->s2 / MSGFACT;
        //msg[4] = (int32_t)(vsqrtf((float)but->s2 / INTERNAL_ONE) * (INTERNAL_ONE / MSGFACT));
      msg[5] = but->v0 / MSGFACT_VELO;
      msg[6] = but->v1 / MSGFACT_VELO;
      msg[7] = but->v2 / MSGFACT_VELO;
      msgSend(8, msg);
      but->timer = (buttons_pressed[0] + buttons_pressed[1]) * SENDFACT;
    }
  }
  else if (but->status && !(but->s0 > MSGFACT + min_pres1 || but->s1 > MSGFACT + min_pres1 || but->s2 > MSGFACT + min_pres1)) {
    if (but->status & ON) {
      msg[1] = but_id;
      msg[2] = 0;
      msg[3] = 0;
      msg[4] = 0;
      msg[5] = but->v0 / MSGFACT_VELO;
      msg[6] = but->v1 / MSGFACT_VELO;
      msg[7] = but->v2 / MSGFACT_VELO;
      while (msgSend(8, msg)) { // note off messages are more important so keep trying
        chThdSleep(1);
      }
    }
    but->status = OFF;
    buttons_pressed[but->src_id]--;
    col_pressed[but->src_id][but_id % 17]--;
  }
}

/*
typedef struct struct_slider {
  int32_t s[27];
  int32_t v[27];
  int timer;
  int pres[4];
  int pos[4];
  int velo[4];
  int sort[4];
  int n_press;
  int move;
  int zoom;
} slider_t;
*/
#define SLD_MAX_DIST 2
#define N_PEAKS 8
#define N_PRESS 4
#define N_SENS 27
#define SLD_STEP (1<<8)
int slider_interp(int n) {
  int a = sld.s[n-1];
  int b = sld.s[n];
  int c = sld.s[n+1];
  if (a > c) {
    return n * SLD_STEP + (c-a) / ((b-c)/SLD_STEP) / 2;
  } else {
    return n * SLD_STEP + (c-a) / ((b-a)/SLD_STEP) / 2;
  }
}

#ifdef USE_BAS
void update_slider(void) {
  int n;
  int np = 0;
  int peaks[N_PEAKS];
  int msg[8];
  msg[0] = ID_CONTROL;

  // make slider less sensitive when buttons are pressed, because of crosstalk
  int min_pres = 0;
  for (n=18; n<(18+17); n+=2) {
    if (min_pres < buttons_bas[n].s0)
      min_pres = buttons_bas[n].s0;
    if (min_pres < buttons_bas[n].s1)
      min_pres = buttons_bas[n].s1;
    if (min_pres < buttons_bas[n].s2)
      min_pres = buttons_bas[n].s2;
  }

  // find peaks
  for (n=1; n<27-1; n++) {
    if (sld.s[n] > min_pres + (INTERNAL_ONE/64) && sld.s[n-1] <= sld.s[n] && sld.s[n] > sld.s[n+1]) {
      peaks[np++] = n;
    }
  }

  // debug output
  #ifdef DEBUG_SDU1
  if (SDU1.state == SDU_READY) {
    if (sld.dbtimer <= 0) {
      for (n=0; n<27; n++) {
        chprintf((BaseSequentialStream *)&SDU1, " %4d", sld.s[n]>>10);
      }
      chprintf((BaseSequentialStream *)&SDU1, "\r\n    ");
      for (n=1; n<27-1; n++) {
        chprintf((BaseSequentialStream *)&SDU1, " %4d", sld.s[n] > 0 && sld.s[n-1] <= sld.s[n] && sld.s[n] > sld.s[n+1]);
      }
      chprintf((BaseSequentialStream *)&SDU1, "     npeaks: %d\r\n",np);
      sld.dbtimer = 100;
    } else {
      sld.dbtimer--;
    }
  }
  #endif

  /*
  signals:
  slide
  zoom
  2up
  2down

   */
  if (np != sld.n_press) {
    msg[1] = IDC_SLD_NPRESS;
    msg[2] = np;
    msgSend(3, msg);
  }

  // single slide (volume)
  if (np == 1) {
    int pos = slider_interp(peaks[0]);
    if (sld.n_press == 1) {
      if (sld.timer <= 0) {
        msg[1] = IDC_SLD_SLIDE;
        msg[2] = -(pos - sld.pos[0]);
        msgSend(3, msg);
        sld.timer = 16 * SENDFACT;
        sld.pos[0] = pos;
      } else {
        sld.timer--;
      }
    } else {
      // new press
      sld.pos[0] = pos;
      sld.timer = 16 * SENDFACT;
    }
  }
  // slide/zoom (tuning)
  else if (np == 2) {
    int pos0 = slider_interp(peaks[0]);
    int pos1 = slider_interp(peaks[1]);
    if (sld.n_press == 2) {
      if (sld.timer <= 0) {
        msg[1] = IDC_SLD_SLIDEZOOM;
        msg[2] = ((pos0 + pos1) - (sld.pos[0] + sld.pos[1]))/2;
        msg[3] = (pos1 - pos0) - (sld.pos[1] - sld.pos[0]);
        msgSend(4, msg);
        sld.timer = 16 * SENDFACT;
        sld.pos[0] = pos0;
        sld.pos[1] = pos1;
      } else {
        sld.timer--;
      }
    } else {
      // new press
      sld.pos[0] = pos0;
      sld.pos[1] = pos1;
      sld.timer = 16 * SENDFACT;
    }
  }
  sld.n_press = np;
  // 1 up/down
  // 2 up/down/middle
  // n press


/*
  // match to presses
  for (n = 0; n<np; n++) {
    int dp = INTERNAL_ONE;
    for (k = 0; k<N_PRESS; k++) {
      if (sld.pres[k] > 0) {
        d = abs(sld.pos[k] - peaks[n]);
        if (d < SLD_MAX_DIST && d < dp && dists[k] == INTERNAL_ONE) {
          dp = d;
          pp[n] = k;
          //if (dists[k] < INTERNAL_ONE) {
          //  dists[k] = INTERNAL_ONE;
          //  for (m = 0; m<n; m++)
          //    if pp[m]
          //}
        }
      }
    }
    if (dp != INTERNAL_ONE) {
      dists[pp[n]] = dp;
    }
  }
  // add new presses
  for (n = 0; n<np; n++) {
    if (pp[n] == -1) {
      for (k = 0; k<N_PRESS; k++) {
        if (sld.pres[k] == 0) {
          pp[n] = k;
          dists[k] = 0;
          break;
        }
      }
    }
  }
  // delete ended presses
  for (k = 0; k<N_PRESS; k++) {
    if (sld.pres[k] > 0 && dists[k]== INTERNAL_ONE) {
      sld.pres[k] = 0;
    }
  }

  // update
  for (n = 0; n<N_PRESS; n++) {
    if (sld.pres[n] > 0) {
      update_peak(n);
      // remove doubles
    }
  }*/
}
#endif

/*
 * Read out buttons and create messages.
 */
static THD_WORKING_AREA(waThreadReadButtons, 128);
static void ThreadReadButtons(void *arg) {
  (void)arg;

  chRegSetThreadName("read_buttons");
  int cur_conv, but_id, note_id;
  button_t* but;

#ifdef DETECT_STUCK_NOTES
  for (int n=0; n<N_BUTTONS; n++) {
    buttons[n].p = 0;
  }
  int count = 0;
  while (count < 100) {
    while (count < 100 && proc_conversion != next_conversion) {
      // process 3 buttons if all 3 values * 3 buttons are available
      if ((proc_conversion % 3) == 2) {
        note_id = (proc_conversion / 3) % 17;
        cur_conv = (proc_conversion - 2);
        for (int n = 0; n < 4; n++) {
          but_id = note_id + n * 17;
          but = &buttons[but_id];
          int s_new = samples[n][cur_conv];
          s_new = linearize(s_new);
          if (s_new > but->p) but->p = s_new;
          s_new = samples[n][cur_conv+1];
          s_new = linearize(s_new);
          if (s_new > but->p) but->p = s_new;
          s_new = samples[n][cur_conv+2];
          s_new = linearize(s_new);
          if (s_new > but->p) but->p = s_new;
        }
        // Once per cycle, after the last buttons
        if (note_id == 16) {
          count++;
        }
      }
      proc_conversion = (proc_conversion+1) % 102;
    }

    chSysLock();
    tpReadButtons = chThdGetSelfX();
    chSchGoSleepS(CH_STATE_SUSPENDED);
    chSysUnlock();
  }
  for (int n=0; n<N_BUTTONS; n++) {
    but = &buttons[n];
    if (but->p > but->c_offset) {
      but->c_offset = but->p + ZERO_LEVEL_OFFSET;
    }
  }
#endif // DETECT_STUCK_NOTES

  while (TRUE) {
    while (proc_conversion != next_conversion) {
      // process 3 buttons if all 3 values * 3 buttons are available
      if ((proc_conversion % 3) == 2) {
        note_id = (proc_conversion / 3) % 17;
        cur_conv = (proc_conversion - 2);

        /* Octave crosstalk
           if note in multiple octaves:
             subtract f * (max - n) from n
             max = 8.5 * n (from test with v1.9, on sensor values)
             f = 1/7.5
             but for very light touches
         */
#ifdef BUTTON_FILT_OCT
        for (int m = 0; m < 3; m++) {
          int max = samples[0][cur_conv + m];
          for (int n = 1; n < 4; n++) {
            if (max > samples[n][cur_conv + m]) { // samples is invert so >
              max = samples[n][cur_conv + m];
            }
          }
          for (int n = 0; n < 4; n++) {
            samples[n][cur_conv + m] -= (max - samples[n][cur_conv + m]) / 6; // / 7; // TODO: - k if other notes in this octave
            if (samples[n][cur_conv + m] > 4095) samples[n][cur_conv + m] = 4095;
          }
        }
#endif
        // Update button in each octave/adc-channel
        for (int n = 0; n < 4; n++) {
          but_id = note_id + n * 17;
          but = &buttons[but_id];
#ifdef TWO_WAY_SAMPLING
          update_button(but, &samples[n][cur_conv % 51]);
#else
          update_button(but, &samples[n][cur_conv]);
#endif
        }
        // Once per cycle, after the last buttons
        if (note_id == 16) {
#ifdef COMMON_CHANNEL_FILT
          // process common channel for crosstalk compensation
          for (int i=0; i<4; i++) {
            int s_new = linearize(samples_common[i]);
            if (octave_sum[i] > 0 && config.common_channel_filter == 1) {
              float new = 1.0f * (float)(s_new+51) / (float)octave_sum[i];
              float f = min(octave_sum[i],1024)/1024.0f;
              new = f*new + (1.0f-f);
              octave_factor[i] = (1-COMMON_CHANNEL_FILT) * octave_factor[i] + COMMON_CHANNEL_FILT * new;
            } else {
              octave_factor[i] = 1.0f;
            }
          }
          octave_sum[0] = 0;
          octave_sum[1] = 0;
          octave_sum[2] = 0;
          octave_sum[3] = 0;
#endif

#ifdef BUTTON_FILT
          // Find maximum pressure (and second to maximum)
          max_pres1 = 0;
          max_pres = 0;
          for (int n = 0; n<17; n++) {
            for (int nr = 0; nr<4; nr++) {
              if (buttons[n + 17*nr].p > max_pres1) {
                max_pres1 = buttons[n + 17*nr].p;
                if (max_pres1 > max_pres) {
                  max_pres1 = max_pres;
                  max_pres = buttons[n + 17*nr].p;
                }
              }
            }
          }
          // Find phantom presses (presses on the fourth corner that appear when 3 corners are pressed)
          // Specific for dis side, on bas side there's only one octave
          for (int n = 0; n<17; n++) {
            if (col_pressed[0][n] >= 2) {
              for (int k = n+1; k<17; k++) {
                if (col_pressed[0][k] >= 2) {
                  for (int nr = 0; nr<4; nr++) {
                    if (buttons[n + 17*nr].status && buttons[k + 17*nr].status) {
                      for (int kr = nr+1; kr<4; kr++) {
                        if (buttons[n + 17*kr].status && buttons[k + 17*kr].status) {
                          // 4 pressed corners are found, give lowest pressure a phantom flag
                          button_t* min = &buttons[n + 17*nr];
                          button_t* b1  = &buttons[k + 17*nr];
                          button_t* b2  = &buttons[n + 17*kr];
                          button_t* b3  = &buttons[k + 17*kr];
                          if (b1->p < min->p) {min = b1;}
                          if (b2->p < min->p) {min = b2;}
                          if (b3->p < min->p) {min = b3;}
                          min->status |= PHANTOM_FLAG;
                        }
                      }
                    }
                  }
                }
              }
            }
          }
#endif
#ifdef USE_AUX_BUTTONS
          int msg[8];
          for (int n = 0; n < 4; n++) {
            if (aux_buttons_state[n] & 0xff) {
              aux_buttons_state[n]--;
            } else if (palReadLine(aux_buttons_line[n]) == !aux_buttons_state[n]) {
              msg[0] = ID_CONTROL;
              msg[1] = aux_buttons_msg[n];
              if (aux_buttons_state[n]) {
                msg[2] = !aux_buttons_on[n];
                aux_buttons_state[n] = AUX_BUTTON_DEBOUNCE_TIME;
              } else {
                msg[2] = aux_buttons_on[n];
                aux_buttons_state[n] = AUX_BUTTON_DEBOUNCE_TIME | 0x100;
              }
              msgSend(3, msg);
            }
          }
#endif // USE_AUX_BUTTONS
        }

#ifdef USE_BAS
        // bas side
        but_id = note_id;
        but = &buttons_bas[but_id];
        update_button(but, &samples_bas[0][cur_conv]);
        if (note_id % 2) {
          but_id = note_id + 17;
          but = &buttons_bas[but_id];
          update_button(but, &samples_bas[1][cur_conv]);
        } else {
          // slider
          //but_id = note_id + 2*17;
          //but = &buttons_bas[but_id];
          //update_button(but, &samples_bas[1][cur_conv]);

          but_id = (note_id / 2) * 3;

          int32_t s_new;
          s_new = calibrate(samples_bas[1][cur_conv + 0], (ADCFACT>>6) / 6, ADC_OFFSET);
          update_and_filter(&sld.s[but_id + 0], &sld.v[but_id + 0], s_new);
          s_new = calibrate(samples_bas[1][cur_conv + 1], (ADCFACT>>6) / 6, ADC_OFFSET);
          update_and_filter(&sld.s[but_id + 1], &sld.v[but_id + 1], s_new);
          s_new = calibrate(samples_bas[1][cur_conv + 2], (ADCFACT>>6) / 6, ADC_OFFSET);
          update_and_filter(&sld.s[but_id + 2], &sld.v[but_id + 2], s_new);

          if (note_id == 16) {
            update_slider();
          }
        }
#endif // USE_BAS
      }
      proc_conversion = (proc_conversion+1) % 102;
    }

    chSysLock();
    tpReadButtons = chThdGetSelfX();
    chSchGoSleepS(CH_STATE_SUSPENDED);
    chSysUnlock();
  }
}

void ButtonBoardTest(void) {
  // test for shorted pins
  for (int n=1; n<OUT_NUM_CHANNELS; n++) {
    /* Drain channel */
    palClearPad(out_channels_port[n-1], out_channels_pad[n-1]);
#ifdef USE_BAS
    palClearPad(out_channels_bas_port[n-1], out_channels_bas_pad[n-1]);
#endif

    palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_INPUT_PULLUP);
#ifdef USE_BAS
    palSetPadMode(out_channels_bas_port[n], out_channels_bas_pad[n], PAL_MODE_INPUT_PULLUP);
#endif

    chThdSleep(1);
    if (!palReadPad(out_channels_port[n], out_channels_pad[n])) {
      // pin n and n-1 shorted

    }

    palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_OUTPUT_OPENDRAIN);
#ifdef USE_BAS
    palSetPadMode(out_channels_bas_port[n], out_channels_bas_pad[n], PAL_MODE_OUTPUT_OPENDRAIN);
#endif
    /* Open channel */
    palSetPad(out_channels_port[n-1], out_channels_pad[n-1]);
#ifdef USE_BAS
    palSetPad(out_channels_bas_port[n-1], out_channels_bas_pad[n-1]);
#endif
  }
}

void buttonSetCalibration(uint32_t c_force, uint32_t c_offset) {
  for (int n=0; n<N_BUTTONS; n++) {
    chprintf((BaseSequentialStream *)&BDU1, "but[%d] c_force: %d c_offset: %d\r\n", n, buttons[n].c_force, buttons[n].c_offset);
    buttons[n].c_force = c_force;
    buttons[n].c_offset = c_offset;
    buttons[n].c_breakpoint = INT32_MAX;
  }
}

void ButtonReadStart(void) {

#if defined(USE_AUX_BUTTONS) && defined(STM32F4XX)
  palSetLineMode(LINE_BUTTON_PORT, PAL_MODE_INPUT_PULLDOWN);
  palSetLineMode(LINE_BUTTON_UP,   PAL_MODE_INPUT_PULLDOWN);
  palSetLineMode(LINE_BUTTON_DOWN, PAL_MODE_INPUT_PULLDOWN);
  palSetLineMode(LINE_BUTTON_ALT,  PAL_MODE_INPUT_PULLDOWN);
#endif

  for (int n=0; n<OUT_NUM_CHANNELS; n++) {
    palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_OUTPUT_OPENDRAIN | PAL_STM32_OSPEED_HIGHEST);
    // palSetPadMode(out_channels_port[n], out_channels_pad[n], PAL_MODE_OUTPUT_PUSHPULL | PAL_STM32_OSPEED_HIGHEST);
  }

  /*
   * Initializes the ADC driver.
   */
#ifdef STM32F4XX
  adcMultiStart();
#elif defined(STM32H7XX)
  adcStart(&ADCD1, &adccfg1);
#endif

  // Initialize buttons
  for (int n=0; n<N_BUTTONS; n++) {
    buttons[n].but_id = n;
    buttons[n].src_id = ID_DIS;
    buttons[n].c_force = CALIB_FORCE;
    buttons[n].c_offset = CALIB_OFFSET;
    buttons[n].prev_but = &buttons[(n/17) * 17 + ((n+17-1) % 17)];
    buttons[n].zero_time = 0;
    buttons[n].zero_max = 0;
  }
#ifdef USE_BAS
  for (int n=0; n<N_BUTTONS_BAS; n++) {
    buttons_bas[n].but_id = n;
    buttons_bas[n].src_id = ID_BAS;
    buttons_bas[n].c_force = (ADCFACT>>6) / 6;//calib_bas[n];//(ADCFACT>>6) / 2;
    buttons_bas[n].c_offset = ADC_OFFSET;
    buttons_bas[n].prev_but = &buttons_bas[(n/17) * 17 + ((n+17-1) % 17)];
  }
#endif

  uint32_t* const UID = (uint32_t*)UID_BASE;

  if (calib_dis_force->UID[0] == UID[0] &&
      calib_dis_force->UID[1] == UID[1] &&
      calib_dis_force->UID[2] == UID[2]) {
    for (int n=0; n<N_BUTTONS; n++) {
      buttons[n].c_force = calib_dis_force->calib[n];
    }
  } else {
    led_rgb(0xff0000);
    chThdSleepMilliseconds(200);
  }

  if (calib_dis_breakpoint->UID[0] == UID[0] &&
      calib_dis_breakpoint->UID[1] == UID[1] &&
      calib_dis_breakpoint->UID[2] == UID[2] &&
      calib_dis_breakpoint->type == 0xb1) {
    for (int n=0; n<N_BUTTONS; n++) {
      buttons[n].c_breakpoint = calib_dis_breakpoint->calib[n] << 16;
    }
  } else {
    for (int n=0; n<N_BUTTONS; n++) {
      buttons[n].c_breakpoint = INT32_MAX;
    }
  }
  if (calib_dis_force2->UID[0] == UID[0] &&
      calib_dis_force2->UID[1] == UID[1] &&
      calib_dis_force2->UID[2] == UID[2] &&
      calib_dis_force2->type == 0x01) {
    for (int n=0; n<N_BUTTONS; n++) {
      buttons[n].c_force2 = calib_dis_force2->calib[n];
    }
  }

  /*
   * Start first ADC conversion. Next conversions are triggered from the adc callback.
   */
#ifdef STM32F4XX
  adcMultiStartConversion(&adcgrpcfg1, &adcgrpcfg2, &adcgrpcfg3, adc_samples, ADC_GRP1_BUF_DEPTH);
#elif defined(STM32H7XX)
  adcStartConversion(&ADCD1, &adcgrpcfg1, adc_samples, ADC_GRP1_BUF_DEPTH);
#endif

  /*
   * Creates the thread to process the adc samples
   */
  chThdCreateStatic(waThreadReadButtons, sizeof(waThreadReadButtons), NORMALPRIO, ThreadReadButtons, NULL);
}
