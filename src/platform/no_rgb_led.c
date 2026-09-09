// IROM lever (combo envs only, -DCROSSPOINT_NO_RGB_LED): keep the Arduino
// core's RGB-LED helper -- and the entire RMT driver stack behind it -- out of
// the link.
//
// The pull-in chain, read off firmware.map's archive-member log:
//   esp32-hal-gpio.c.o        -- __digitalWrite() has an unconditional
//                                `pin == RGB_BUILTIN` branch calling rgbLedWrite()
//     -> esp32-hal-rgb-led.c.o  (references rmtInit/rmtWrite)
//       -> esp32-hal-rmt.c.o    (references rmt_new_tx_channel & friends)
//         -> libesp_driver_rmt.a(rmt_tx, rmt_rx, rmt_common, rmt_encoder,
//                                rmt_encoder_copy)
//           -> libhal.a(rmt_hal.c.o), libsoc.a(rmt_periph.c.o)
// Every step is an undefined-symbol reference, so breaking the first one drops
// the whole tail.
//
// Nothing in this firmware -- app, lib/ or freeink-sdk -- calls any rmt* or
// rgbLed* entry point. The branch that references rgbLedWrite() is unreachable
// here: RGB_BUILTIN is the esp32-s3-devkitc1 variant's WS2812 on GPIO48,
// encoded as SOC_GPIO_PIN_COUNT + 48 = 97, and no X4 Pro pin is ever that
// value. gc-sections cannot see that, because the comparison is a runtime test.
//
// Defining the three exported entry points here means the archive member is
// never extracted, so the chain never starts. The stubs are dead code that
// exists only to satisfy the linker; they are deliberately no-ops rather than
// aborts, since the only caller is that dead branch.
//
// To restore RGB-LED / RMT support on an env, drop -DCROSSPOINT_NO_RGB_LED.
#ifdef CROSSPOINT_NO_RGB_LED

#include <esp32-hal-rgb-led.h>

void rgbLedWriteOrdered(uint8_t pin, rgb_led_color_order_t order, uint8_t red_val, uint8_t green_val,
                        uint8_t blue_val) {
  (void)pin;
  (void)order;
  (void)red_val;
  (void)green_val;
  (void)blue_val;
}

void rgbLedWrite(uint8_t pin, uint8_t red_val, uint8_t green_val, uint8_t blue_val) {
  (void)pin;
  (void)red_val;
  (void)green_val;
  (void)blue_val;
}

void neopixelWrite(uint8_t p, uint8_t r, uint8_t g, uint8_t b) {
  (void)p;
  (void)r;
  (void)g;
  (void)b;
}

#endif  // CROSSPOINT_NO_RGB_LED
