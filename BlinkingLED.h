
#include <Arduino.h>

constexpr uint16_t LED_ONLINE_ON_TICKS  = 1562UL;	// ~100ms (1024 prescaler)	
constexpr uint16_t LED_ONLINE_OFF_TICKS = 10000UL - LED_ONLINE_ON_TICKS;


void OnlineLED_Timer_On();
void OnlineLED_Timer_Off();