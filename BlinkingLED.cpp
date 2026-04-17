
#include "BlinkingLED.h"
#include "PinsConfig.h"

void OnlineLED_Timer_On()
{
	OCR1A = LED_ONLINE_OFF_TICKS;
	TCCR1B |= (1 << CS12) | (1 << CS10);    // Start Timer1 w 1024 prescaler clock
}


void OnlineLED_Timer_Off()
{
	TCCR1B &= ~((1 << CS12) | (1 << CS11) | (1 << CS10)); // Stop timer
	digitalWrite(PIN_LED_ONLINE, LOW);
}