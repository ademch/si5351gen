
#include <stdint.h>

#include "Arduino.h"
#include "Wire.h"
#include "si_5351.h"


//------------ Public functions ----------------------------------------------------------------


Si5351::Si5351(uint8_t i2c_addr) : i2c_bus_addr(i2c_addr)
{
	xtal_freq = SI5351_XTAL_FREQ;
	bIsOnline = false;
}

/*
 * Setup communications to the Si5351 and set the crystal load capacitance.
 *
 * xtal_load_c	- Crystal load capacitance. Use the SI5351_CRYSTAL_LOAD_PF defines in the header file
 * corr			- Frequency correction constant in parts-per-billion
 *
 * Returns a boolean that indicates whether a device was found on I2C address
 */
bool Si5351::init(uint8_t xtal_load_c, int32_t corr)
{
	// Start I2C comms
	Wire.begin();

	// Check for a device on the bus, bail out if it is not there
	Wire.beginTransmission(i2c_bus_addr);

	uint8_t reg_val = Wire.endTransmission();
	if (reg_val != 0) return false;

	bIsOnline = true;
	
	// Wait for SYS_INIT flag to be clear, indicating that device is ready
	while (si5351_read(SI5351_DEVICE_STATUS_ADDR) & SI5351_STATUS_SYS_INIT) delay(100);

	// Set crystal load capacitance                                                   magic number
	si5351_write(SI5351_CRYSTAL_LOAD_ADDR, (xtal_load_c & SI5351_CRYSTAL_LOAD_MASK) | 0b00010010);

	ref_correction = corr;

	reset();

	return true;
}


// Reset sequence of the Si5351 to the library known state
void Si5351::reset()
{
	// Initialize the CLK outputs according to flowchart in datasheet

	// Disable all clk output buffers (set_clock_enable on all outputs)
	si5351_write(SI5351_OUTPUT_ENABLE_CTRL_ADDR, 0xFF);

	// Power down clock channels pipelines
	for (uint8_t i = 0; i < 8; i++)
		set_clock_powerON((enum si5351_clock)(SI5351_CLK0 + i), false);

	// Set PLLA and PLLB to 800 MHz
	set_pll_freq(SI5351_PLLA, SI5351_PLL_FIXED);
	set_pll_freq(SI5351_PLLB, SI5351_PLL_FIXED);

	// Assign CLK channels to default PLLA
	for (uint8_t i = 0; i < 8; i++)
		set_multisynth_source((enum si5351_clock)(SI5351_CLK0 + i), SI5351_PLLA);

	// Reset the VCXO param
	si5351_write(SI5351_VXCO_PARAMETERS_0_7_ADDR,   0);
	si5351_write(SI5351_VXCO_PARAMETERS_8_15_ADDR,  0);
	si5351_write(SI5351_VXCO_PARAMETERS_16_21_ADDR, 0);

	// Reset the PLLs
	pll_reset(SI5351_PLLA);
	pll_reset(SI5351_PLLB);

	// Assign multisynth as input to CLK0
	set_clock_source(SI5351_CLK0, SI5351_CLK_SRC_MS);

	// Clear initial frequencies
	for (uint8_t i = 0; i < 8; i++)
	{
		clk_freq[i]   = 0;
		clk_is_set[i] = false;
	}

	// Power ON CLK0
	set_clock_powerON(SI5351_CLK0, true);
}

/*
 * Sets the clock frequency of the specified CLK output.
 * Frequency range of 8 kHz to 150 MHz
 * Set PLL before calling
 *
 * clk	- Clock output channel
 * freq	- Output frequency in Hz
 */
uint8_t Si5351::set_freq(enum si5351_clock clk, uint64_t freq)
{
	if (!bIsOnline) return 10;

	// Check which Multisynth is being set
	if (clk <= SI5351_CLK5)
	{
		// Lower bounds check
		if (freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT)
			freq = SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT;

		// Upper bounds check
		if (freq > SI5351_MULTISYNTH_MAX_FREQ * SI5351_FREQ_MULT)
			freq = SI5351_MULTISYNTH_MAX_FREQ * SI5351_FREQ_MULT;

		// If requested freq > 100 MHz and no other outputs are already >100 MHz,
		// we need to recalculate PLLA and then recalculate all other CLK outputs on same PLL
		// The target clock gets integer mode multisynth, the other get fractional because of fixed PLL for the target clock
		if (freq > SI5351_MULTISYNTH_SHARE_MAX * SI5351_FREQ_MULT)
		{
			// Check other clocks on same PLL
			for (uint8_t i = 0; i < 6; i++)
			{
				if (i == clk) continue;

				if (clk_freq[i] > SI5351_MULTISYNTH_SHARE_MAX * SI5351_FREQ_MULT)
				{
					if (pll_assigned_to_clk[i] == pll_assigned_to_clk[clk])
						return 1; // won't set if any other clks already >100 MHz
				}
			}

			// Calculate proper PLL frequency
			uint64_t pll_freq = 0;
			struct Si5351RegSet ms_reg = {};
			multisynth_calc_p1p2p3(freq, pll_freq, &ms_reg);

			// Set multisynth registers
			set_multisynth_reg(clk, ms_reg, SI5351_OUTPUT_CLK_DIV_1);

			// Set PLL
			set_pll_freq(pll_assigned_to_clk[clk], pll_freq);

			// Reset the PLL
			pll_reset(pll_assigned_to_clk[clk]);

			// Enable the output on first set_freq only
			if (!clk_is_set[clk])
			{
				set_clock_enable(clk, true);
				clk_is_set[clk] = true;
			}

			// Set the freq in memory
			clk_freq[clk] = freq;

			// Recalculate params for other synths on the same PLL
			for (uint8_t i = 0; i < 6; i++)
			{
				// skip the target clock, it has been just setup
				if (i == clk) continue;

				// if the other clock has been setup and its PLL is the PLL used in reconfigured target
				if ( (clk_freq[i] != 0) && (pll_assigned_to_clk[i] == pll_assigned_to_clk[clk]) )
				{
						// Select the proper R div value
						uint64_t temp_freq = clk_freq[i];
						uint8_t r_div = select_RDIV_forLowFreq(temp_freq);

						struct Si5351RegSet ms_reg = {};
						multisynth_calc_p1p2p3(temp_freq, pll_freq, &ms_reg);

						// Set multisynth registers
						set_multisynth_reg((enum si5351_clock)i, ms_reg, r_div);
				}
			}
		}
		else  // requested freq < 100 MHz
		{
			// Select the proper R div value
			uint8_t r_div = select_RDIV_forLowFreq(freq);

			// Calculate the synth parameters
			struct Si5351RegSet ms_reg = {};
			if (pll_assigned_to_clk[clk] == SI5351_PLLA)
				multisynth_calc_p1p2p3(freq, plla_freq, &ms_reg);
			else
				multisynth_calc_p1p2p3(freq, pllb_freq, &ms_reg);

			// Set multisynth registers
			set_multisynth_reg(clk, ms_reg, r_div);

			// PLLs has been already set in reset!

			clk_freq[clk] = freq;

			// Enable the output on first set_freq only
			if (!clk_is_set[clk])
			{
				set_clock_enable(clk, true);
				clk_is_set[clk] = true;
			}
		}

		return 0;
	}
	else  // MS6 and MS7 logic
	{
		return 0;
	}
}

// Set the clock frequency of the specified CLK calculating integer multisynth frequency.
// PLL frequency is caluclated automatically
//
// clk	- Clock output
// freq	- Output frequency in Hz
uint8_t Si5351::set_freq_manual(enum si5351_clock clk, uint64_t freq)
{
	if (!bIsOnline) return 10;

	// Lower bounds check
	if (freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT)
		freq = SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT;

	// Upper bounds check
	if (freq > SI5351_CLKOUT_MAX_FREQ * SI5351_FREQ_MULT)
		freq = SI5351_CLKOUT_MAX_FREQ * SI5351_FREQ_MULT;

	// Select the proper R div value and frequency
	uint8_t r_div = select_RDIV_forLowFreq(freq);

	// Calculate multisynth parameters before PLL params
	struct Si5351RegSet ms_reg = {};
	uint64_t pll_freq = 0;	// ask to calculate pll_freq
	multisynth_calc_p1p2p3(freq, pll_freq, &ms_reg);

	Serial.print("IntMode: ");
	Serial.print(ms_reg.bIntMode);
	Serial.print(", bDivBy4: ");
	Serial.println(ms_reg.bDivBy4);

	set_multisynth_reg(clk, ms_reg, r_div);

	clk_freq[clk] = freq;

	set_pll_freq(pll_assigned_to_clk[clk], pll_freq);

	// Reset the PLL
	pll_reset(pll_assigned_to_clk[clk]);

	// Enable the output
	set_clock_enable(clk, true);

	return 0;
}


// Set the specified PLL to a specific oscillation frequency
//
// targetPLL	- PLLx to set
// pll_freq		- Desired PLL frequency in Hz*100
//
void Si5351::set_pll_freq(enum si5351_pll targetPLL, uint64_t pll_freq)
{
	if (!bIsOnline) return;

	struct Si5351RegSet pll_reg = {};
	pll_calc_p1p2p3(pll_freq, &pll_reg, ref_correction);

	// Prepare an array for parameters to be written to
	uint8_t *params = new uint8_t[8];

		// Registers 26-30
		params[0] =			((pll_reg.p3 >> 8) & 0xFF);
		params[1] = (uint8_t)(pll_reg.p3 & 0xFF);
		params[2] = (uint8_t)((pll_reg.p1 >> 16) & 0x03);
		params[3] = (uint8_t)((pll_reg.p1 >> 8) & 0xFF);
		params[4] = (uint8_t)(pll_reg.p1 & 0xFF);

		// Register 31
		params[5] = (uint8_t)((pll_reg.p3 >> 12) & 0xF0) | (uint8_t)((pll_reg.p2 >> 16) & 0x0F);

		// Registers 32-33
		params[6] = (uint8_t)((pll_reg.p2 >> 8) & 0xFF);
		params[7] = (uint8_t)(pll_reg.p2 & 0xFF);

		// Write the parameters
		if (targetPLL == SI5351_PLLA)
		{
			si5351_write_bulk(SI5351_PLLA_PARAMETERS_ADDR, 8, params);
			plla_freq = pll_freq;
		}
		else if (targetPLL == SI5351_PLLB)
		{
			si5351_write_bulk(SI5351_PLLB_PARAMETERS_ADDR, 8, params);
			pllb_freq = pll_freq;
		}

	delete params;
}

/*
 * Set the specified multisynth parameters. Not normally needed, but public for advanced users.
 *
 * clk		- Clock output
 * ms_reg	- Multisynth params
 * r_div	- Desired r_div
 */
void Si5351::set_multisynth_reg(enum si5351_clock clk, struct Si5351RegSet ms_reg, uint8_t r_div)
{
	uint8_t *params = new uint8_t[8];

		if (clk <= SI5351_CLK5)
		{
			// Registers 42-43 (CLK0)
			params[0] = (uint8_t)((ms_reg.p3 >> 8) & 0xFF);
			params[1] = (uint8_t)(ms_reg.p3 & 0xFF);

			// Register 44 (CLK0)
			uint8_t reg_val = si5351_read((SI5351_CLK0_PARAMETERS_ADDR + 2) + (clk * 8));
			reg_val &= ~0x03;
			params[2] = reg_val | ((uint8_t)((ms_reg.p1 >> 16) & 0x03));

			// Registers 45-46 for CLK0
			params[3] = (uint8_t)((ms_reg.p1 >> 8) & 0xFF);
			params[4] = (uint8_t)(ms_reg.p1 & 0xFF);

			// Register 47 for CLK0
			params[5] = (uint8_t)((ms_reg.p3 >> 12) & 0xF0) | (uint8_t)((ms_reg.p2 >> 16) & 0x0F);

			// Registers 48-49 for CLK0
			params[6] = (uint8_t)((ms_reg.p2 >> 8) & 0xFF);
			params[7] = (uint8_t)(ms_reg.p2 & 0xFF);

			// Write the parameters
			si5351_write_bulk(SI5351_CLK0_PARAMETERS_ADDR + 8 * (uint8_t)clk, 8, params);

			set_multisynth_integer_mode(clk, ms_reg.bIntMode);
			set_multisynth_rdiv(clk, r_div, ms_reg.bDivBy4);
		}
		else
		{
			// MS6 and MS7 only use one register
			params[0] = ms_reg.p1;

			// Write the parameters
			si5351_write(SI5351_CLK6_PARAMETERS_ADDR + (uint8_t)clk - 6, params[0]);
			set_multisynth_rdiv(clk, r_div, ms_reg.bDivBy4);
		}

	delete params;
}


// Enable/Disable a chosen output clock channel driver
// See also set_clock_powerON
//
// clk		- Clock output channel
// enable	- enable/disable
void Si5351::set_clock_enable(enum si5351_clock clk, bool bEnable)
{
	if (!bIsOnline) return;

	uint8_t reg_val = si5351_read(SI5351_OUTPUT_ENABLE_CTRL_ADDR);

	if (bEnable)
		reg_val &= ~(1 << (uint8_t)clk);
	else
		reg_val |=   1 << (uint8_t)clk;

	si5351_write(SI5351_OUTPUT_ENABLE_CTRL_ADDR, reg_val);
}

/*
 * Sets the drive strength of the specified clock output
 *
 * clk		- Clock output
 * drive	- Desired drive level
 */
void Si5351::drive_strength(enum si5351_clock clk, enum si5351_drive drive)
{
	if (!bIsOnline) return;

	uint8_t reg_val = si5351_read(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk);

	reg_val &= ~0x03;
	reg_val |= (uint8_t)drive;


	si5351_write(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk, reg_val);
}

/*
 * Call this to update the status structs, then access them
 * via the dev_status and dev_int_status global members.
 *
 * See the header file for the struct definitions. These
 * correspond to the flag names for registers 0 and 1 in the Si5351 datasheet.
 */
void Si5351::update_status(void)
{
	if (!bIsOnline) return;

	update_sys_status_(&dev_status);
	update_int_status_(&dev_int_status);
}

/*
 * corr		- Correction factor in ppb
 *
 * Use this to set the ref_freq correction factor. This value is a signed 32-bit integer of the
 * parts-per-billion value that the actual oscillation frequency deviates from the specified frequency.
 *
 * The frequency calibration is done as a one-time procedure.
 * Any desired test frequency within the normal range of the Si5351 should be set, then the actual output frequency
 * should be measured as accurately as possible. The difference between the measured and specified frequencies
 * should be calculated in Hertz, then multiplied by 10 in order to get the parts-per-billion value.
 *
 * Since the Si5351 itself has an intrinsic 0 PPM error, this correction factor is good across the entire tuning range of
 * the Si5351. Once this calibration is done accurately, it should not have to be done again for the same Si5351 and crystal.
 */
void Si5351::set_correction(int32_t corr)
{
	ref_correction = corr;

	// Recalculate and set PLL freqs based on correction value
	set_pll_freq(SI5351_PLLA, plla_freq);
	set_pll_freq(SI5351_PLLB, pllb_freq);
}

/*
 * clk		- Clock output (use the si5351_clock enum)
 * phase	- 7-bit phase word (in units of VCO/4 period)
 *
 * Write the 7-bit phase register. This must be used with a user-set PLL frequency
 * so that the user can calculate the proper tuning word based on the PLL period.
 */
void Si5351::set_phase(enum si5351_clock clk, uint8_t phase)
{
	if (!bIsOnline) return;

	// Mask off the upper bit since it is reserved
	phase = phase & 0b01111111;

	si5351_write(SI5351_CLK0_PHASE_OFFSET + (uint8_t)clk, phase);
}


// Returns the oscillator correction factor stored in RAM.
int32_t Si5351::get_correction()
{
	return ref_correction;
}

// Apply a reset to the indicated PLL
//
// targetPLL - Which PLL to reset
//
void Si5351::pll_reset(enum si5351_pll targetPLL)
{
	if (!bIsOnline) return;

	if		(targetPLL == SI5351_PLLA)
		si5351_write(SI5351_PLL_RESET, SI5351_PLL_RESET_A);
	else if (targetPLL == SI5351_PLLB)
		si5351_write(SI5351_PLL_RESET, SI5351_PLL_RESET_B);
}

// Put the indicated multisynth into integer mode
// Integer mode provides shorter path ignoring b and c values
//
// clk		- Clock output channel
// bEnable	- Set true/false
//
void Si5351::set_multisynth_integer_mode(enum si5351_clock clk, bool bEnable)
{
	uint8_t reg_val = si5351_read(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk);

	if (bEnable)
		reg_val |=  SI5351_CLK_INTEGER_MODE;
	else
		reg_val &= ~SI5351_CLK_INTEGER_MODE;

	si5351_write(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk, reg_val);
}

/*
 * clk		- Clock output (use the si5351_clock enum)
 * bPowerUp - Set to 1 to enable, 0 to disable
 *
 * Enable or disable power to a clock output (a power saving feature).
 */
void Si5351::set_clock_powerON(enum si5351_clock clk, bool bPowerUp)
{
	if (!bIsOnline) return;

	uint8_t reg_val = si5351_read(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk);

	if (bPowerUp)
		reg_val &= 0b01111111;
	else
		reg_val |= 0b10000000;

	si5351_write(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk, reg_val);
}

/*
 * clk - Clock output
 * inv - false/true
 *
 * Enable to invert the clock output waveform.
 */
void Si5351::set_clock_invert(enum si5351_clock clk, bool bInvert)
{
	if (!bIsOnline) return;

	uint8_t reg_val = si5351_read(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk);

	if (bInvert)
		reg_val |=  SI5351_CLK_INVERT;
	else
		reg_val &= ~SI5351_CLK_INVERT;

	si5351_write(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk, reg_val);
}

// Set the clock source (Registers 16-23 in the Silicon Labs AN619 document)
// Choices are XTAL, CLKIN, MS0, or the multisynth associated with the clock output
//
// clk - Clock output channel
// src - Which clock source to use for the multisynth
//
void Si5351::set_clock_source(enum si5351_clock clk, enum si5351_clock_source src)
{
	if (!bIsOnline) return;

	uint8_t reg_val = si5351_read(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk);

	// Clear the bits first
	reg_val &= ~SI5351_CLK_INPUT_MASK;

	switch (src)
	{
		case SI5351_CLK_SRC_XTAL:
			reg_val |= SI5351_CLK_INPUT_XTAL;	// default
			break;
		case SI5351_CLK_SRC_CLKIN:
			reg_val |= SI5351_CLK_INPUT_CLKIN;
			break;
		case SI5351_CLK_SRC_MS_RELAY:
			if (clk == SI5351_CLK0) return;		// not allowed combination for CLK0
			if (clk == SI5351_CLK4) return;		// not allowed combination for CLK4

			reg_val |= SI5351_CLK_INPUT_MULTISYNTH_RELAY;
			break;
		case SI5351_CLK_SRC_MS:
			reg_val |= SI5351_CLK_INPUT_MULTISYNTH;
			break;
	}

	si5351_write(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk, reg_val);
}


// Set multisynth source PLL
//
// clk - Clock output
// pll - Which PLL to use as the source
//
void Si5351::set_multisynth_source(enum si5351_clock clk, enum si5351_pll pll)
{
	if (!bIsOnline) return;

	uint8_t reg_val = si5351_read(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk);

	if (pll == SI5351_PLLA)
		reg_val &= ~SI5351_CLK_PLL_SELECT;
	else if (pll == SI5351_PLLB)
		reg_val |=  SI5351_CLK_PLL_SELECT;

	si5351_write(SI5351_CLK0_CTRL_ADDR + (uint8_t)clk, reg_val);

	pll_assigned_to_clk[clk] = pll;
}

/*
 * clk - Clock output (use the si5351_clock enum)
 * dis_state - Desired state of the output upon disable (use the si5351_clock_disable enum)
 *
 * Set the state of the clock output when it is disabled.
 * Per page 27 of AN619 (Registers 24 and 25), there are four possible values: low, high, high impedance, and never disabled.
 */
void Si5351::set_clock_disabled_state(enum si5351_clock clk, enum si5351_clock_disable dis_state)
{
	if (!bIsOnline) return;

	uint8_t reg_val, reg;

	if (clk <= SI5351_CLK3)
	{
		reg = SI5351_CLK3_0_DISABLE_STATE;

		reg_val = si5351_read(reg);
		reg_val &= ~(0b11 << (clk * 2));
		reg_val |= dis_state << (clk * 2);
	}
	else if (clk <= SI5351_CLK7)
	{
		reg = SI5351_CLK7_4_DISABLE_STATE;

		reg_val = si5351_read(reg);
		reg_val &= ~(0b11 << ((clk - 4) * 2));
		reg_val |= dis_state << ((clk - 4) * 2);
	}
	else
		return;

	si5351_write(reg, reg_val);
}

/*
 * fanout	- Desired clock fanout (use the si5351_clock_fanout enum)
 * bEnable	- false/true
 *
 * Use this function to enable or disable the clock fanout options
 * for individual clock outputs. If you intend to output the XO or
 * CLKIN on the clock outputs, enable this first.
 *
 * By default, only the Multisynth fanout is enabled at startup.
 */
void Si5351::set_clock_fanout(enum si5351_clock_fanout fanout, bool bEnable)
{
	if (!bIsOnline) return;
	
	uint8_t reg_val = si5351_read(SI5351_FANOUT_ENABLE);

	switch (fanout)
	{
		case SI5351_FANOUT_CLKIN:
			if (bEnable)
				reg_val |= SI5351_CLKIN_ENABLE;
			else
				reg_val &= ~SI5351_CLKIN_ENABLE;
			break;
		case SI5351_FANOUT_XO:
			if (bEnable)
				reg_val |= SI5351_XTAL_ENABLE;
			else
				reg_val &= ~SI5351_XTAL_ENABLE;
			break;
		case SI5351_FANOUT_MS:
			if (bEnable)
				reg_val |= SI5351_MULTISYNTH_ENABLE;
			else
				reg_val &= ~SI5351_MULTISYNTH_ENABLE;
			break;
	}

	si5351_write(SI5351_FANOUT_ENABLE, reg_val);
}

uint8_t Si5351::si5351_write_bulk(uint8_t addr, uint8_t bytes, uint8_t *data)
{
	Wire.beginTransmission(i2c_bus_addr);
	Wire.write(addr);
	for(int i = 0; i < bytes; i++)
	{
		Wire.write(data[i]);
	}
	return Wire.endTransmission();
}

uint8_t Si5351::si5351_write(uint8_t addr, uint8_t data)
{
	Wire.beginTransmission(i2c_bus_addr);
	Wire.write(addr);
	Wire.write(data);
	return Wire.endTransmission();
}

uint8_t Si5351::si5351_read(uint8_t addr)
{
	uint8_t reg_val = 0;

	Wire.beginTransmission(i2c_bus_addr);
	Wire.write(addr);
	Wire.endTransmission();

	Wire.requestFrom(i2c_bus_addr, (uint8_t)1);

	while (Wire.available())
	{
		reg_val = Wire.read();
	}

	return reg_val;
}


/* ---------------------------------------Private functions ------------------------------------------------------*/


uint64_t Si5351::pll_calc_p1p2p3(uint64_t pll_freq, struct Si5351RegSet *reg, int32_t correction)
{
	uint64_t ref_freq = xtal_freq * SI5351_FREQ_MULT;

	// Factor calibration value into nominal crystal frequency measured in parts-per-billion
	ref_freq = ref_freq + (int32_t)((((((int64_t)correction) << 31) / 1000000000LL) * ref_freq) >> 31);

	// PLL bounds checking
	if (pll_freq < SI5351_PLL_VCO_MIN * SI5351_FREQ_MULT)
		pll_freq = SI5351_PLL_VCO_MIN * SI5351_FREQ_MULT;

	if (pll_freq > SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT)
		pll_freq = SI5351_PLL_VCO_MAX * SI5351_FREQ_MULT;

	// Determine integer part of feedback equation
	uint32_t a, b, c;

	a = (uint32_t)(pll_freq / ref_freq);

	if (a < SI5351_PLL_A_MIN)
		pll_freq = ref_freq * SI5351_PLL_A_MIN;

	if (a > SI5351_PLL_A_MAX)
		pll_freq = ref_freq * SI5351_PLL_A_MAX;

	// Find the best approximation for b/c = fPLL mod fRef
	//
	// eg: 690MHZ/25MHZ = 27.6 ---> a=27, b/c=0.6
	// Following integer math we do not perform division untill the last operation:
	// Remainder from integer devision fRef*0.6=15MHz we
	// multiply by precision units to understand how many of those are needed and
	// as soon as remainder was in ref_freq multiples we divide by ref_freq
	//
	// Equation is: 25MHz(27 + b/c) = 690MHz ---> 690MHz/25MHz-27 = b/c
	// 15MHz * 1`000`000 / 25
	b = (uint32_t)(((pll_freq % ref_freq) * RFRAC_DENOM) / ref_freq);
	c = b ? RFRAC_DENOM : 1;


	Serial.print("PLL a: ");
	Serial.print(a);
	Serial.print(", b: ");
	Serial.print(b);
	Serial.print(", c: ");
	Serial.println(c);

	// Calculate parameters
	uint32_t p1, p2, p3;

	p1 = 128 * a + ((128 * b) / c) - 512;
	p2 = 128 * b - c * ((128 * b) / c);
	p3 = c;

	// Recalculate frequency as fIN * (a + b/c)
	pll_freq = ref_freq * a;
	pll_freq += ref_freq * b / c;

	reg->p1 = p1;
	reg->p2 = p2;
	reg->p3 = p3;

	return pll_freq;
}


// Calc p1p2p3 out of provided frequency and pll frequency
void Si5351::multisynth_calc_p1p2p3(uint64_t freq, uint64_t& pll_freq, struct Si5351RegSet *reg)
{
	// Multisynth bounds checking
	if (freq > SI5351_MULTISYNTH_MAX_FREQ * SI5351_FREQ_MULT)
		freq = SI5351_MULTISYNTH_MAX_FREQ * SI5351_FREQ_MULT;

	if (freq < SI5351_MULTISYNTH_MIN_FREQ * SI5351_FREQ_MULT)
		freq = SI5351_MULTISYNTH_MIN_FREQ * SI5351_FREQ_MULT;

	// If freq > 150 MHz, we need to use DIVBY4 and integer mode
	if (freq >= SI5351_MULTISYNTH_DIVBY4_FREQ * SI5351_FREQ_MULT)
	{
		reg->p1 = 0;
		reg->p2 = 0;
		reg->p3 = 1;

		pll_freq = 4 * freq;

		reg->bDivBy4  = true;
		reg->bIntMode = true;

		Serial.print("PLL freq: ");
		print_u64(pll_freq);
		Serial.println("");

		return;
	}

	uint32_t a, b, c;

	if (pll_freq == 0)
	{
		// Calc pll frequency when asked providing multisynth integer mode

		// Find the smallest even integer divider for min VCO frequency and given target frequency
		uint64_t llmin = SI5351_PLL_VCO_MIN * SI5351_FREQ_MULT;
		uint64_t lla = llmin / freq;

		// if there is fractional part then take higher integer
		if (llmin % freq != 0)
			lla += 1;

		// take next even integer
		if (lla % 2 != 0)
			lla += 1;

		a = (uint32_t)lla;
		b = 0;
		c = 1;

		pll_freq = a * freq;

		Serial.print("PLL freq: ");
		print_u64(pll_freq);
		Serial.println("");
		Serial.print("MS a: ");
		Serial.print(a);
		Serial.print(", b: ");
		Serial.print(b);
		Serial.print(", c: ");
		Serial.println(c);

		reg->bIntMode = true;
	}
	else  // pll_freq is provided on input
	{
		// a+b/c = pll/freq ---> a = pll/freq, b/c = pll%freq ---> b = pll%freq * c

		// Determine integer part of feedback equation
		a = (uint32_t)(pll_freq / freq);

		if (a < SI5351_MULTISYNTH_A_MIN)
			freq = pll_freq / SI5351_MULTISYNTH_A_MIN;

		if (a > SI5351_MULTISYNTH_A_MAX)
			freq = pll_freq / SI5351_MULTISYNTH_A_MAX;

		b = (uint32_t)((pll_freq % freq * RFRAC_DENOM) / freq);
		c = b ? RFRAC_DENOM : 1;
	}

	// Calculate parameters
	reg->p1 = 128*a + ((128*b) / c) - 512;
	reg->p2 = 128*b - c * ((128*b) / c);
	reg->p3 = c;
}



void Si5351::update_sys_status_(struct Si5351Status *status)
{
	uint8_t reg_val = si5351_read(SI5351_DEVICE_STATUS_ADDR);

	// Parse the register
	status->SYS_INIT = (reg_val >> 7) & 0x01;
	status->LOL_B	 = (reg_val >> 6) & 0x01;
	status->LOL_A	 = (reg_val >> 5) & 0x01;
	status->LOS		 = (reg_val >> 4) & 0x01;
	status->REVID	 =  reg_val & 0x03;
}


// By default all interrupts are enabled
void Si5351::update_int_status_(struct Si5351IntStatus *int_status)
{
	uint8_t reg_val = si5351_read(SI5351_INTERRUPT_STATUS_ADDR);

	// Parse the register
	int_status->SYS_INIT_STKY = (reg_val >> 7) & 0x01;
	int_status->LOL_B_STKY	  = (reg_val >> 6) & 0x01;
	int_status->LOL_A_STKY	  = (reg_val >> 5) & 0x01;
	int_status->LOS_STKY	  = (reg_val >> 4) & 0x01;
}

// bDivBy4 is a bypass for Multisynth divider
void Si5351::set_multisynth_rdiv(enum si5351_clock clk, uint8_t r_div, bool bDivBy4)
{
	uint8_t reg_addr = 0;

	if (clk <= SI5351_CLK5)
		reg_addr = SI5351_CLK0_PARAMETERS_ADDR + 2 + 8 * (uint8_t)clk;
	else
		reg_addr = SI5351_CLK6_7_OUTPUT_DIVIDER;

	uint8_t reg_val = si5351_read(reg_addr);

	if (clk <= SI5351_CLK5)
	{
		// Clear the relevant bits
		reg_val &= ~0x7c;

		if (!bDivBy4)
			reg_val &= ~SI5351_OUTPUT_CLK_DIVBY4;
		else
			reg_val |=  SI5351_OUTPUT_CLK_DIVBY4;

		reg_val |= r_div << SI5351_OUTPUT_CLK_DIV_SHIFT;
	}
	else if (clk == SI5351_CLK6)
	{
		// Clear the relevant bits
		reg_val &= ~0x07;
		reg_val |= r_div;
	}
	else if (clk == SI5351_CLK7)
	{
		// Clear the relevant bits
		reg_val &= ~0x70;
		reg_val |= r_div << SI5351_OUTPUT_CLK_DIV_SHIFT;
	}

	si5351_write(reg_addr, reg_val);
}


// R divider by logic of the library provides low frequencies
// In that case the frequency is updated to be higher than minimal
// E.g. user asks for 7.8Khz, then 7.8kHz*128 is asked from vco and /128 from R divider
uint8_t Si5351::select_RDIV_forLowFreq(uint64_t& freq)
{
	if (freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT * 2)
	{
		freq *= 128ULL;
		return SI5351_OUTPUT_CLK_DIV_128;
	}
	else if (freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT * 4)
	{
		freq *= 64ULL;
		return SI5351_OUTPUT_CLK_DIV_64;
	}
	else if (freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT * 8)
	{
		freq *= 32ULL;
		return SI5351_OUTPUT_CLK_DIV_32;
	}
	else if (freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT * 16)
	{
		freq *= 16ULL;
		return SI5351_OUTPUT_CLK_DIV_16;
	}
	else if (freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT * 32)
	{
		freq *= 8ULL;
		return SI5351_OUTPUT_CLK_DIV_8;
	}
	else if (freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT * 64)
	{
		freq *= 4ULL;
		return SI5351_OUTPUT_CLK_DIV_4;
	}
	else if (freq < SI5351_CLKOUT_MIN_FREQ * SI5351_FREQ_MULT * 128)
	{
		freq *= 2ULL;
		return SI5351_OUTPUT_CLK_DIV_2;
	}
	else
		return SI5351_OUTPUT_CLK_DIV_1;

}

void Si5351::print_ram()
{
	for (uint16_t i = 0; i < 256; i++)
	{
		if ((i >= 4)   && (i <= 8))   continue;
		if ((i >= 10)  && (i <= 14))  continue;
		if ((i >= 171) && (i <= 176)) continue;
		if ((i >= 178) && (i <= 182)) continue;
		if ((i >= 184) && (i <= 186)) continue;
		if ((i >= 188) && (i <= 255)) continue;

		uint8_t reg_val = si5351_read(i);

		Serial.print("[");
		Serial.print(i);
		Serial.print("]=");

		Serial.println(reg_val);
	}
}


void print_u64(uint64_t v)
{
	char buf[21];            // max 20 digits + NUL
	char *p = &buf[20];
	*p = '\0';

	if (v == 0)
	{
		Serial.print('0');
		return;
	}

	while (v > 0)
	{
		*--p = '0' + (v % 10);
		v /= 10;
	}

	Serial.print(p);
}