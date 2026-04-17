
#include "si_5351.h"
#include <Wire.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "PinsConfig.h"
#include "BlinkingLED.h"
#include "KButton.h"
#include "Command.h"



Si5351 si5351(SI5351_BUS_BASE_ADDR);

#define OLED_RESET -1
	Adafruit_SSD1306 display(OLED_RESET);

	#if (SSD1306_LCDHEIGHT != 64)
		#error("Height incorrect, please fix Adafruit_SSD1306.h!");
#endif


KButton keyLeft(PIN_KEYB_LEFT);
KButton keyRight(PIN_KEYB_RIGHT);
KButton keyOK(PIN_KEYB_OK);
KButton keyUP(PIN_KEYB_UP, true);
KButton keyDOWN(PIN_KEYB_DOWN, true);


bool bLED_ONLINE_ON = false;

ISR(TIMER1_COMPA_vect) // timer compare interrupt service routine
{
	if (bLED_ONLINE_ON)
	{
		// turn LED OFF
		OCR1A = LED_ONLINE_OFF_TICKS;
		digitalWrite(PIN_LED_ONLINE, LOW);
	}
	else
	{
		// turn LED ON
		OCR1A = LED_ONLINE_ON_TICKS;
		digitalWrite(PIN_LED_ONLINE, HIGH);
	}
	bLED_ONLINE_ON = !bLED_ONLINE_ON;
}

void setup()
{
	Serial.begin(57600);
	delay(100);
	Serial.println("Hi");

	// setup onboard led
	pinMode(PIN_LED_ONLINE, OUTPUT);
	digitalWrite(PIN_LED_ONLINE, 0);

	cli(); // disable all interrupts

#pragma region set up Timer1 for blinking LED control
	TCCR1A = 0;
	TCCR1B = 0;
	TCNT1  = 0;

	TCCR1B |= (1 << WGM12);              // Clear Timer on Compare Match mode ("CTC" mode)
	TIMSK1 |= (1 << OCIE1A);             // enable timer Output Compare Register A Match interrupt
#pragma endregion

	sei(); // enable all interrupts

	// by default, we'll generate the high voltage from the 3.3v line internally! (neat!)
	display.begin(SSD1306_SWITCHCAPVCC, 0x3C);

	// Clear the default buffer (splashscreen)
	display.clearDisplay();

	display.setCursor(0, 0);
	display.println(F("Build date:"));
	display.println(__DATE__);
	display.println(__TIME__);
	display.display();
	delay(2000);

#pragma region Configure si5351
	display.print(F("si5351 init..."));
	display.display();
	if (!si5351.init(SI5351_CRYSTAL_LOAD_8PF, 0))
	{
		display.println(F("fail"));

		Serial.print("si5351 is not found on I2C bus at address ");
		Serial.println(SI5351_BUS_BASE_ADDR);
	}
	else
	{
		display.println(F("done"));
	}
	display.display();
	delay(3000);
#pragma endregion

#pragma region Configure Keyboard pins
	pinMode(PIN_KEYB_LEFT,  INPUT_PULLUP);
	pinMode(PIN_KEYB_RIGHT, INPUT_PULLUP);
	pinMode(PIN_KEYB_OK,	INPUT_PULLUP);
	pinMode(PIN_KEYB_UP,	INPUT_PULLUP);
	pinMode(PIN_KEYB_DOWN,  INPUT_PULLUP);
	delay(50); // wait for debounce capacitor charging
#pragma endregion

	display.clearDisplay();

	DrawInfo();
}

void loop()
{
  // Read the Status Register and print it every 10 seconds
  //si5351.update_status();
  //Serial.print("SYS_INIT: ");
  //Serial.print(si5351.dev_status.SYS_INIT);
  //Serial.print("  LOL_A: ");
  //Serial.print(si5351.dev_status.LOL_A);
  //Serial.print("  LOL_B: ");
  //Serial.print(si5351.dev_status.LOL_B);
  //Serial.print("  LOS: ");
  //Serial.print(si5351.dev_status.LOS);
  //Serial.print("  REVID: ");
  //Serial.println(si5351.dev_status.REVID);

  if (keyLeft.Pressed())  ProcessMenuNavigationLeftRight(-1);
  if (keyRight.Pressed()) ProcessMenuNavigationLeftRight(+1);
  if (keyOK.Pressed())    ProcessMenuItemOkClick();
  if (keyUP.Pressed())    ProcessMenuItemUpDownClick(true);
  if (keyDOWN.Pressed())  ProcessMenuItemUpDownClick(false);

  delay(100);
}
