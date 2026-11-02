// servo.h — RMT arm/position + LEDC PWM fallback.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SERVO_COUNT 4

esp_err_t servo_init(void);

void  servo_write(int i, float deg);
void  servo_flush(void);
float servo_pos(int i);
uint32_t servo_pulse_us(int i);
void  servo_power_on(void);
void  servo_release(void);
bool  servo_powered(void);
int   servo_gpio(int i);

void  servo_bitbang_test(int deg, int ms);
void  servo_rmt_poke(uint8_t cmd, uint16_t data);
void  servo_pin_buzz(int ms);

const char *servo_diag(void);
int servo_ack_ok(void);
int servo_ack_fail(void);

void    servo_set_rmt_cmd(uint8_t cmd);
uint8_t servo_rmt_cmd(void);
void    servo_set_encode(int mode);

#ifdef __cplusplus
}
#endif
