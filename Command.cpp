
#include "Command.h"
#include "Adafruit_SSD1306.h"
#include "si_5351.h"
#include "BlinkingLED.h"

extern Adafruit_SSD1306 display;
extern Si5351 si5351;

int8_t iDigitPos = 2;

bool bCLK0isON = false;

uint8_t digits6[6] = {1,0,0,0,0,0};
int8_t  driveStrength = 0;


void DrawInfo()
{
	display.clearDisplay();
	display.setTextSize(2);

	// Channel block
	display.setCursor(0, 0);
	
		display.print("  CLK0 ");
		if (bCLK0isON) display.print("ON");
		else     	   display.print("OFF");

		display.InvertBlockFast(0, 0, 127, 2);					// menu highlight  

	// Frequency block
	display.setCursor(1, 24);

		for (int8_t i = 0; i <= 5; i++)
		{
			if (i == 3) display.print(".");
			display.print(digits6[i]);
		}
		display.print("MHz");

		if (!bCLK0isON && (iDigitPos <= 6))
			display.InvertBlockFast(iDigitPos * 12, 3, 12, 2);  // cursor highlight

	// Drive strength block
	display.setCursor(1, 48);

		display.print( 2*(driveStrength + 1) );
		display.print("      ma");

		if (!bCLK0isON && (iDigitPos == 7))
			display.InvertBlockFast(0, 6, 12, 2);				// cursor highlight
		
	display.display();
}

void ProcessMenuNavigationLeftRight(int8_t iPosDelta)
{
	if (bCLK0isON) return;
	
	if (iPosDelta > 0) {
		iDigitPos++;
		if (iDigitPos == 3) iDigitPos = 4;
		if (iDigitPos > 7)  iDigitPos = 0;
	}
	else if (iPosDelta < 0) {
		iDigitPos--;
		if (iDigitPos == 3) iDigitPos = 2;
		if (iDigitPos < 0)  iDigitPos = 7;
	}

	DrawInfo();
}

void ProcessMenuItemUpDownClick(bool bUp)
{
	if (bCLK0isON) return;

	int delta = bUp ? 1 : -1;

	int8_t iPos = iDigitPos;
	if (iPos >= 3) iPos--;

	if (iPos <= 5)	// frequency
	{
		digits6[iPos] = (digits6[iPos] + delta) % 10;
		if (digits6[iPos] == 255) digits6[iPos] = 9;

		// prevent other digits than 0 or 1 to stay below frequncy limit of 200MHZ
		if (iPos == 0)
		{
			if      (digits6[0] == 9) digits6[0] = 0;
			else if (digits6[0] == 2) digits6[0] = 1;
		}
	}
	else
	{
		// current
		driveStrength += delta;
		if (driveStrength > 3) driveStrength = 3;
		if (driveStrength < 0) driveStrength = 0;
	}

	DrawInfo();
}

void ProcessMenuItemOkClick()
{
	uint64_t frequency = digits6[0]*100000ULL + digits6[1]*10000ULL + digits6[2]*1000ULL + digits6[3]*100ULL + digits6[4]*10ULL + digits6[5];
	frequency *= 1000ULL;	// kHz to Hz
	frequency *= SI5351_FREQ_MULT;

	if (frequency < 4000ULL) return;

	bCLK0isON = !bCLK0isON;

	if (bCLK0isON)
	{
		Serial.println("");
		Serial.print("Frequency ON: ");
		print_u64(frequency);
		Serial.println("");

		si5351.drive_strength(SI5351_CLK0, (enum si5351_drive)driveStrength);
		si5351.set_freq_manual(SI5351_CLK0, frequency);

		OnlineLED_Timer_On();
		//si5351.print_ram();
	}
	else
	{
		si5351.set_clock_enable(SI5351_CLK0, false);

		Serial.println("Frequency OFF");
		OnlineLED_Timer_Off();
	}

	DrawInfo();
}

