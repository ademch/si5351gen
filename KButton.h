
#define KEY_PRESSED   LOW
#define KEY_RELEASED  HIGH

class KButton
{
public:
	// pin number assigned
	uint8_t iPinHW;					
	// HIGH/LOW HW state of the button (LOW is pressed)
	bool iButtonState;				
	// time when the button has been released
	unsigned long ulmillis_prevKeybReleased;
	// makes the handler to be called many times
	bool bAutoRepeat;
	// state preventing multiple keyDown event if the user has not stopped holding the button
	bool bButtonPrevStatePressed;

	KButton(uint8_t iPinHW, bool bAutoRepeat = false):
	iPinHW(iPinHW),
	bAutoRepeat(bAutoRepeat)
	{
		iButtonState = KEY_RELEASED;
		bButtonPrevStatePressed = false;
		ulmillis_prevKeybReleased = 0;
	}

	bool Pressed()
	{
		bool result = false;

		unsigned long ulmillis = millis();
		if (ulmillis - ulmillis_prevKeybReleased > 200)
		{
			// check if the button is pressed
			iButtonState = digitalRead(iPinHW);
			if (iButtonState == KEY_PRESSED)
			{
				if (!bButtonPrevStatePressed)
				{
					result = true;
					bButtonPrevStatePressed = true;
				}
				else // if bButtonPrevStatePressed
				{
					// by default we return false, with the yield the handler is called only once

					// in case of autorepeat then reset the state as if the handler has never been called
					// still return false to increase the delay between sequential handler calls
					if (bAutoRepeat) {
						ulmillis_prevKeybReleased = ulmillis;  // record the time when the button was released
						bButtonPrevStatePressed = false;
					}
				}
			}
			else
			{
				if (bButtonPrevStatePressed) {
					ulmillis_prevKeybReleased = ulmillis;  // record the time when the button was released
					bButtonPrevStatePressed = false;
				}
			}
		}
		return result;
	}

};


