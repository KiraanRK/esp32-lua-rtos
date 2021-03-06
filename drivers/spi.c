/*
 * Lua RTOS, SPI driver
 *
 * Copyright (C) 2015 - 2016
 * IBEROXARXA SERVICIOS INTEGRALES, S.L. & CSS IBÉRICA, S.L.
 * 
 * Author: Jaume Olivé (jolive@iberoxarxa.com / jolive@whitecatboard.org)
 * 
 * All rights reserved.  
 *
 * Permission to use, copy, modify, and distribute this software
 * and its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that the copyright notice and this
 * permission notice and warranty disclaimer appear in supporting
 * documentation, and that the name of the author not be used in
 * advertising or publicity pertaining to distribution of the
 * software without specific, written prior permission.
 *
 * The author disclaim all warranties with regard to this
 * software, including all implied warranties of merchantability
 * and fitness.  In no event shall the author be liable for any
 * special, indirect or consequential damages or any damages
 * whatsoever resulting from loss of use, data or profits, whether
 * in an action of contract, negligence or other tortious action,
 * arising out of or in connection with the use or performance of
 * this software.
 */

/*
 * This driver is inspired and takes code from the following projects:
 *
 * arduino-esp32 (https://github.com/espressif/arduino-esp32)
 * esp32-nesemu (https://github.com/espressif/esp32-nesemu
 * esp-open-rtos (https://github.com/SuperHouse/esp-open-rtos)
 *
 */

/*
 * Whitecat's original driver is modified
 * to allow spi operations on non-default spi pins configured using gpio matrix
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_attr.h"
#include "soc/io_mux_reg.h"
#include "soc/spi_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_reg.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include <sys/syslog.h>
#include <sys/driver.h>

#include <drivers/spi.h>
#include <drivers/gpio.h>
#include <drivers/cpu.h>

// Native pins of the spi peripherals 1-3
// Used to check if used spi pins are default for given spi interface
static const spi_signal_conn_t io_signal[3]={
    {
        .spiclk_out=SPICLK_OUT_IDX,
        .spid_out=SPID_OUT_IDX,
        .spiq_out=SPIQ_OUT_IDX,
        .spid_in=SPID_IN_IDX,
        .spiq_in=SPIQ_IN_IDX,
        .spics_out=SPICS0_OUT_IDX,
        .spiclk_native=6,
        .spid_native=8,
        .spiq_native=7,
        .spics0_native=11,
    }, {
        .spiclk_out=HSPICLK_OUT_IDX,
        .spid_out=HSPID_OUT_IDX,
        .spiq_out=HSPIQ_OUT_IDX,
        .spid_in=HSPID_IN_IDX,
        .spiq_in=HSPIQ_IN_IDX,
        .spics_out=HSPICS0_OUT_IDX,
        .spiclk_native=14,
        .spid_native=13,
        .spiq_native=12,
        .spics0_native=15,
    }, {
        .spiclk_out=VSPICLK_OUT_IDX,
        .spid_out=VSPID_OUT_IDX,
        .spiq_out=VSPIQ_OUT_IDX,
        .spid_in=VSPID_IN_IDX,
        .spiq_in=VSPIQ_IN_IDX,
        .spics_out=VSPICS0_OUT_IDX,
        .spiclk_native=18,
        .spid_native=23,
        .spiq_native=19,
        .spics0_native=5,
    }
};

#define MATRIX_DETACH_OUT_SIG 0x100
#define MATRIX_DETACH_IN_LOW_PIN 0x30
#define MATRIX_DETACH_IN_LOW_HIGH 0x38

static int last_unit = NSPI*NSPI_DEV+1;

// Driver message errors
DRIVER_REGISTER_ERROR(SPI, spi, CannotSetup, "can't setup",  SPI_ERR_CANT_INIT);
DRIVER_REGISTER_ERROR(SPI, spi, InvalidMode, "invalid mode", SPI_ERR_INVALID_MODE);
DRIVER_REGISTER_ERROR(SPI, spi, InvalidUnit, "invalid unit", SPI_ERR_INVALID_UNIT);
DRIVER_REGISTER_ERROR(SPI, spi, SlaveNotAllowed, "slave mode not allowed", SPI_ERR_SLAVE_NOT_ALLOWED);

/*
// SPI structures
struct spi {
    int          cs;	  // cs pin for device (if 0 use default cs pin)
    unsigned int speed;   // spi device speed
    unsigned int divisor; // clock divisor
    unsigned int mode;    // device spi mode
    unsigned int dirty;   // if 1 device must be reconfigured at next spi_select
};
*/

//struct spi spi[CPU_LAST_SPI + 1];
spi_interface_t spi[NSPI*NSPI_DEV] = {0};

/*
 * Helper functions
 */
static void _spi_init() {
	//memset(spi, 0, sizeof(struct spi) * (CPU_LAST_SPI + 1));
}

/*
 * Extracted from arduino-esp32 (cores/esp32/esp32-hal-spi.c) for get the clock divisor
 * needed for setup the SIP bus at a desired baud rate
 *
 */
#define PIN_FUNC_SPI 1
#define ClkRegToFreq(reg) (CPU_CLK_FREQ / (((reg)->regPre + 1) * ((reg)->regN + 1)))

typedef union {
    uint32_t regValue;
    struct {
        unsigned regL :6;
        unsigned regH :6;
        unsigned regN :6;
        unsigned regPre :13;
        unsigned regEQU :1;
    };
} spiClk_t;

static uint32_t spiClockDivToFrequency(uint32_t clockDiv)
{
    spiClk_t reg = { clockDiv };
    return ClkRegToFreq(&reg);
}

static uint32_t spiFrequencyToClockDiv(uint32_t freq) {

    if(freq >= CPU_CLK_FREQ) {
        return SPI_CLK_EQU_SYSCLK;
    }

    const spiClk_t minFreqReg = { 0x7FFFF000 };
    uint32_t minFreq = ClkRegToFreq((spiClk_t*) &minFreqReg);
    if(freq < minFreq) {
        return minFreqReg.regValue;
    }

    uint8_t calN = 1;
    spiClk_t bestReg = { 0 };
    int32_t bestFreq = 0;

    while(calN <= 0x3F) {
        spiClk_t reg = { 0 };
        int32_t calFreq;
        int32_t calPre;
        int8_t calPreVari = -2;

        reg.regN = calN;

        while(calPreVari++ <= 1) {
            calPre = (((CPU_CLK_FREQ / (reg.regN + 1)) / freq) - 1) + calPreVari;
            if(calPre > 0x1FFF) {
                reg.regPre = 0x1FFF;
            } else if(calPre <= 0) {
                reg.regPre = 0;
            } else {
                reg.regPre = calPre;
            }
            reg.regL = ((reg.regN + 1) / 2);
            calFreq = ClkRegToFreq(&reg);
            if(calFreq == (int32_t) freq) {
                memcpy(&bestReg, &reg, sizeof(bestReg));
                break;
            } else if(calFreq < (int32_t) freq) {
                if(abs(freq - calFreq) < abs(freq - bestFreq)) {
                    bestFreq = calFreq;
                    memcpy(&bestReg, &reg, sizeof(bestReg));
                }
            }
        }
        if(calFreq == (int32_t) freq) {
            break;
        }
        calN++;
    }
    return bestReg.regValue;
}


/*
 * End of extracted code from arduino-esp32
 */

void spi_pins(int unit, unsigned char *sdi, unsigned char *sdo, unsigned char *sck, unsigned char* cs) {
	// Get default SPI pins
    switch (unit) {
    	case 1:
        	*sdi = GPIO7;
            *sdo = GPIO8;
            *sck = GPIO6;
            *cs =  GPIO11;
            break;

        case 2:
            *sdi = GPIO12;
            *sdo = GPIO13;
            *sck = GPIO14;
            *cs =  GPIO15;
            break;

        case 3:
            *sdi = GPIO19;
            *sdo = GPIO23;
            *sck = GPIO18;
            *cs =  GPIO5;
            break;
    }
}

// Lock resources needed by the SPI
driver_error_t *spi_lock_resources(int unit, void *resources) {
	spi_interface_t *dev = &spi[unit];
	spi_resources_t *spi_resources = dev->res;

    driver_unit_lock_error_t *lock_error = NULL;

	if (spi_resources->sck == 0) {
	    // Get default pins
		spi_pins(unit, &spi_resources->sdi, &spi_resources->sdo, &spi_resources->sck, &spi_resources->cs);
	}

    // Lock this pins
    if ((lock_error = driver_lock(SPI_DRIVER, unit, GPIO_DRIVER, spi_resources->sdi))) {
    	// Revoked lock on pin
    	return driver_lock_error(SPI_DRIVER, lock_error);
    }

    if ((lock_error = driver_lock(SPI_DRIVER, unit, GPIO_DRIVER, spi_resources->sdo))) {
    	// Revoked lock on pin
    	return driver_lock_error(SPI_DRIVER, lock_error);
    }

    if ((lock_error = driver_lock(SPI_DRIVER, unit, GPIO_DRIVER, spi_resources->sck))) {
    	// Revoked lock on pin
    	return driver_lock_error(SPI_DRIVER, lock_error);
    }

    if ((lock_error = driver_lock(SPI_DRIVER, unit, GPIO_DRIVER, spi_resources->cs))) {
    	// Revoked lock on pin
    	return driver_lock_error(SPI_DRIVER, lock_error);
    }

    return NULL;
}

/*
 * Operation functions
 *
 */

/*
 * Init pins for a device, and return used pins
 */
void spi_pin_config(int unit, unsigned char sdi, unsigned char sdo, unsigned char sck, unsigned char cs) {
	spi_interface_t *dev = &spi[unit];

    // Set the cs pin to the default cs pin for device
	spi_set_cspin(unit, cs);

	// Configure pins
	dev->res->sdi = sdi;
	dev->res->sdo = sdo;
	dev->res->sck = sck;
	dev->res->cs = cs;
	dev->dirty = 1;
}

void get_spi_pin_config(int unit, unsigned char *sdi, unsigned char *sdo, unsigned char *sck, unsigned char *cs) {
	spi_interface_t *dev = &spi[unit];

	// Get pin configuration
	*sdi = dev->res->sdi;
	*sdo = dev->res->sdo;
	*sck = dev->res->sck;
    *cs = dev->res->cs;
}

void IRAM_ATTR spi_master_op(int unit, unsigned int word_size, unsigned int len, unsigned char *out, unsigned char *in) {
	unsigned int bytes = word_size * len; // Number of bytes to write / read
	unsigned int idx = 0;

	unit &= 3;
	/*
	 * SPI data buffers hardware registers are 32-bit size, so we use a
	 * transfer buffer for adapt user buffers to buffers expected by hardware, this
	 * buffer is 16-word size (64 bytes)
	 *
	 */
	uint32_t buffer[16]; // Transfer buffer
	uint32_t wd;         // Current word
	unsigned int wdb; 	 // Current byte into current word

	// This is the number of bits to transfer for current chunk
	unsigned int bits;

	bytes = word_size * len;
	while (bytes) {
		// Populate transfer buffer in chunks of 64 bytes
		idx = 0;
		bits = 0;
		while (bytes && (idx < 16)) {
			wd = 0;
			wdb = 4;
			while (bytes && wdb) {
				wd = (wd >> 8);
				if (out) {
					wd |= *out << 24;
					out++;
				} else {
					wd |= 0xff << 24;
				}
				wdb--;
				bytes--;
				bits += 8;
			}

			while (wdb) {
				wd = (wd >> 8);
				wdb--;
			}

			buffer[idx] = wd;
			idx++;
		}

		// Wait for SPI bus ready
		while (READ_PERI_REG(SPI_CMD_REG(unit))&SPI_USR);

		// Load send buffer
	    SET_PERI_REG_BITS(SPI_MOSI_DLEN_REG(unit), SPI_USR_MOSI_DBITLEN, bits - 1, SPI_USR_MOSI_DBITLEN_S);
	    SET_PERI_REG_BITS(SPI_MISO_DLEN_REG(unit), SPI_USR_MISO_DBITLEN, bits - 1, SPI_USR_MISO_DBITLEN_S);

	    idx = 0;
	    while ((idx << 5) < bits) {
		    WRITE_PERI_REG((SPI_W0_REG(unit) + (idx << 2)), buffer[idx]);
		    idx++;
	    }

	    // Start transfer
	    SET_PERI_REG_MASK(SPI_CMD_REG(unit), SPI_USR);

	    // Wait for SPI bus ready
		while (READ_PERI_REG(SPI_CMD_REG(unit))&SPI_USR);

		if (in) {
			// Read data into buffer
			idx = 0;
			while ((idx << 5) < bits) {
				buffer[idx] = READ_PERI_REG((SPI_W0_REG(unit) + (idx << 2)));
				idx++;
			}

			memcpy((void *)in, (void *)buffer, bits >> 3);
			in += (bits >> 3);
		}
	}
}

/*
 * Set the SPI mode for a device. Nothing is changed at hardware level.
 *
 */
driver_error_t *spi_set_mode(int unit, int mode) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if ((mode < 0) || (mode > 3)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_MODE, NULL);
	}

	spi_interface_t *dev = &spi[unit];

    dev->mode = mode;
    dev->dirty = 1;

    return NULL;
}

/*
 * Set the SPI bit rate for a device and stores the clock divisor needed
 * for this bit rate. Nothing is changed at hardware level.
 */

driver_error_t *spi_set_speed(int unit, unsigned int sck) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	spi_interface_t *dev = &spi[unit];

	dev->speed = sck;
    dev->divisor = spiFrequencyToClockDiv(sck * 1000);
    dev->dirty = 1;

    return NULL;
}

/*
 * Select the device. Prior this we reconfigure the SPI bus
 * to the required settings.
 */
driver_error_t *spi_select(int unit) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	spi_interface_t *dev = &spi[unit];
	int lunit = last_unit;
	spi_interface_t *ldev = NULL;
	if (lunit < (NSPI*NSPI_DEV)) {
		ldev = &spi[lunit];
		lunit &= (NSPI-1);
	}
	if (last_unit != unit) {
		// spi interface changed
		dev->dirty = 1;
		last_unit = unit;
	}
	unit &= (NSPI-1);

    if (dev->dirty) {
        int pin_func = PIN_FUNC_SPI; // Native spi pins
    	// === SPI (re)initialization is necessary ===
        if ((ldev) && (unit > 0)) {
        	// Complete operations, if pending
            CLEAR_PERI_REG_MASK(SPI_SLAVE_REG(lunit), SPI_TRANS_DONE << 5);
            SET_PERI_REG_MASK(SPI_USER_REG(lunit), SPI_CS_SETUP);

            //Check if the last unit pins correspond to the native pins of the peripheral
			if (ldev->res->sdo != io_signal[lunit-1].spid_native)   pin_func = PIN_FUNC_GPIO;
			if (ldev->res->sdi != io_signal[lunit-1].spiq_native)   pin_func = PIN_FUNC_GPIO;
			if (ldev->res->sck != io_signal[lunit-1].spiclk_native) pin_func = PIN_FUNC_GPIO;
			if (ldev->res->cs  != io_signal[lunit-1].spics0_native) pin_func = PIN_FUNC_GPIO;
			if (pin_func == PIN_FUNC_GPIO) {
				// Detach gpios from spi interface
	        	gpio_matrix_in(ldev->res->sdi,  MATRIX_DETACH_IN_LOW_PIN, 0);
				gpio_matrix_out(ldev->res->sdo, MATRIX_DETACH_OUT_SIG, 0, 0);
				gpio_matrix_out(ldev->res->sck, MATRIX_DETACH_OUT_SIG, 0, 0);
				gpio_matrix_out(ldev->res->cs,  MATRIX_DETACH_OUT_SIG, 0, 0);

				gpio_pad_select_gpio(ldev->res->sdi);
				gpio_pad_select_gpio(ldev->res->sdo);
				gpio_pad_select_gpio(ldev->res->sck);
				gpio_pad_select_gpio(ldev->res->cs);

	        	gpio_set_direction(ldev->res->sdo, GPIO_MODE_OUTPUT);
	        	gpio_set_direction(ldev->res->sck, GPIO_MODE_OUTPUT);
	        	gpio_set_direction(ldev->res->cs, GPIO_MODE_OUTPUT);
	        	gpio_set_direction(ldev->res->sdi, GPIO_MODE_INPUT);
	        	gpio_set_pull_mode(ldev->res->sdi, GPIO_PULLUP_ONLY);

	        	gpio_pin_set(ldev->res->cs);
			}
        }

    	// Complete operations, if pending
        CLEAR_PERI_REG_MASK(SPI_SLAVE_REG(unit), SPI_TRANS_DONE << 5);
        SET_PERI_REG_MASK(SPI_USER_REG(unit), SPI_CS_SETUP);

        // Configure pins
        pin_func = PIN_FUNC_SPI; // Native spi pins
		if (unit > 0) {
			//Check if the selected pins correspond to the native pins of the peripheral
			if (dev->res->sdo != io_signal[unit-1].spid_native)   pin_func = PIN_FUNC_GPIO;
			if (dev->res->sdi != io_signal[unit-1].spiq_native)   pin_func = PIN_FUNC_GPIO;
			if (dev->res->sck != io_signal[unit-1].spiclk_native) pin_func = PIN_FUNC_GPIO;
			if (dev->res->cs  != io_signal[unit-1].spics0_native) pin_func = PIN_FUNC_GPIO;
        }
        else pin_func = PIN_FUNC_GPIO; // Use GPIO for spi pins

    	PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[dev->res->sdi], pin_func);
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[dev->res->sdo], pin_func);
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[dev->res->sck], pin_func);
        // *** DO NOT USE SPI HW CS ***
        PIN_FUNC_SELECT(GPIO_PIN_MUX_REG[dev->res->cs],  PIN_FUNC_GPIO);
    	gpio_set_direction(dev->res->cs, GPIO_MODE_OUTPUT);

        if (pin_func == PIN_FUNC_GPIO) {
        	gpio_set_direction(dev->res->sdo, GPIO_MODE_OUTPUT);
        	gpio_set_direction(dev->res->sck, GPIO_MODE_OUTPUT);
        	gpio_set_direction(dev->res->cs, GPIO_MODE_OUTPUT);
        	gpio_set_direction(dev->res->sdi, GPIO_MODE_INPUT);
        	gpio_set_pull_mode(dev->res->sdi, GPIO_PULLUP_ONLY);

        	gpio_matrix_in(dev->res->sdi,  io_signal[unit-1].spiq_in,    0);
			gpio_matrix_out(dev->res->sdo, io_signal[unit-1].spid_out,   0, 0);
			gpio_matrix_out(dev->res->sck, io_signal[unit-1].spiclk_out, 0, 0);
			gpio_matrix_out(dev->res->cs,  io_signal[unit-1].spics_out,  0, 0);
        }

        WRITE_PERI_REG(SPI_USER_REG(unit), 0);
    	// Set mode
    	switch (dev->mode) {
    		case 0:
    		    // Set CKP to 0
    		    CLEAR_PERI_REG_MASK(SPI_PIN_REG(unit),  SPI_CK_IDLE_EDGE);

    		    // Set CPHA to 0
    		    CLEAR_PERI_REG_MASK(SPI_USER_REG(unit), SPI_CK_OUT_EDGE);

    		    break;

    		case 1:
    		    // Set CKP to 0
    		    CLEAR_PERI_REG_MASK(SPI_PIN_REG(unit),  SPI_CK_IDLE_EDGE);

    		    // Set CPHA to 1
    		    SET_PERI_REG_MASK(SPI_USER_REG(unit), SPI_CK_OUT_EDGE);

    		    break;

    		case 2:
    		    // Set CKP to 1
    		    SET_PERI_REG_MASK(SPI_PIN_REG(unit),  SPI_CK_IDLE_EDGE);

    		    // Set CPHA to 0
    		    CLEAR_PERI_REG_MASK(SPI_USER_REG(unit), SPI_CK_OUT_EDGE);

    		    break;

    		case 3:
    		    // Set CKP to 1
    		    SET_PERI_REG_MASK(SPI_PIN_REG(unit),  SPI_CK_IDLE_EDGE);

    		    // Set CPHA to 1
    		    SET_PERI_REG_MASK(SPI_USER_REG(unit), SPI_CK_OUT_EDGE);
    	}

    	// Set bit order to MSB
        CLEAR_PERI_REG_MASK(SPI_CTRL_REG(unit), SPI_WR_BIT_ORDER | SPI_RD_BIT_ORDER);

        // Set byte order to litte_endian
        //CLEAR_PERI_REG_MASK(SPI_USER_REG(unit), SPI_WR_BYTE_ORDER);

        // Enable full-duplex communication
        if (dev->res->duplex) {
        	SET_PERI_REG_MASK(SPI_USER_REG(unit), SPI_DOUTDIN);
        }

        // Configure as master
        WRITE_PERI_REG(SPI_USER1_REG(unit), 0);
    	SET_PERI_REG_BITS(SPI_CTRL2_REG(unit), SPI_MISO_DELAY_MODE, 0, SPI_MISO_DELAY_MODE_S);
    	CLEAR_PERI_REG_MASK(SPI_SLAVE_REG(unit), SPI_SLAVE_MODE);

        // Set clock
        CLEAR_PERI_REG_MASK(SPI_CLOCK_REG(unit), SPI_CLK_EQU_SYSCLK);
        WRITE_PERI_REG(SPI_CLOCK_REG(unit), dev->divisor);

        // Enable MOSI / MISO / CS
        SET_PERI_REG_MASK(SPI_USER_REG(unit), SPI_CS_SETUP | SPI_CS_HOLD | SPI_USR_MOSI | SPI_USR_MISO);
        SET_PERI_REG_MASK(SPI_CTRL2_REG(unit), ((0x4 & SPI_MISO_DELAY_NUM) << SPI_MISO_DELAY_NUM_S));

        CLEAR_PERI_REG_MASK(SPI_USER_REG(unit), SPI_USR_COMMAND);
        SET_PERI_REG_BITS(SPI_USER2_REG(unit), SPI_USR_COMMAND_BITLEN, 0, SPI_USR_COMMAND_BITLEN_S);
        CLEAR_PERI_REG_MASK(SPI_USER_REG(unit), SPI_USR_ADDR);
        SET_PERI_REG_BITS(SPI_USER1_REG(unit), SPI_USR_ADDR_BITLEN, 0, SPI_USR_ADDR_BITLEN_S);

        dev->dirty = 0;
    }

	// Select device
    if (dev->res->cs) {
       gpio_pin_clr(dev->res->cs);
    }

    return NULL;
}

/*
 * Deselect the device
 */
driver_error_t *spi_deselect(int unit) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	spi_interface_t *dev = &spi[unit];

    if (dev->res->cs) {
		// Wait for SPI bus ready
		while (READ_PERI_REG(SPI_CMD_REG(unit))&SPI_USR);
        gpio_pin_set(dev->res->cs);
    }

    return NULL;
}

/*
 * Set the CS pin for the device. This function stores CS pin in
 * device data, and setup CS pin as output and set to the high state
 * (device is not select)
 */
driver_error_t *spi_set_cspin(int unit, int pin) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	spi_interface_t *dev = &spi[unit];

    if (pin != dev->res->cs) {
    	dev->res->cs = pin;

        if (pin) {
            gpio_pin_output(dev->res->cs);
            gpio_pin_set(dev->res->cs);

            dev->dirty = 1;
        }
    }

    return NULL;
}

/*
 * Transfer one word of data, and return the read word of data.
 * The actual number of bits sent depends on the mode of the transfer.
 * This is blocking, and waits for the transfer to complete
 * before returning.  Times out after a certain period.
 */
driver_error_t * IRAM_ATTR spi_transfer(int unit, unsigned int data, unsigned char *read) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

    spi_master_op(unit, 1, 1, (unsigned char *)(&data), read);

    return NULL;
}

/*
 * Send a chunk of 8-bit data.
 */
driver_error_t *spi_bulk_write(int unit, unsigned int nbytes, unsigned char *data) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

    taskDISABLE_INTERRUPTS();
    spi_master_op(unit, 1, nbytes, data, NULL);
    taskENABLE_INTERRUPTS();

    return NULL;
}

/*
 * Receive a chunk of 8-bit data.
 */
driver_error_t *spi_bulk_read(int unit, unsigned int nbytes, unsigned char *data) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

    taskDISABLE_INTERRUPTS();
    spi_master_op(unit, 1, nbytes, NULL, data);
    taskENABLE_INTERRUPTS();

    return NULL;
}

/*
 * Send and receive a chunk of 8-bit data.
 */
driver_error_t *spi_bulk_rw(int unit, unsigned int nbytes, unsigned char *data) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	unsigned char *read = (unsigned char *)malloc(nbytes);
	if (read) {
	    taskDISABLE_INTERRUPTS();
	    spi_master_op(unit, 1, nbytes, data, read);
	    taskENABLE_INTERRUPTS();
	}

	memcpy(data, read, nbytes);
	free(read);

    return NULL;
}

/*
 * Send a chunk of 16-bit data.
 */
driver_error_t *spi_bulk_write16(int unit, unsigned int words, short *data) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

    taskDISABLE_INTERRUPTS();
    spi_master_op(unit, 2, words, (unsigned char *)data, NULL);
    taskENABLE_INTERRUPTS();

    return NULL;
}

/*
 * Receive a chunk of 16-bit data.
 */
driver_error_t *spi_bulk_read16(int unit, unsigned int nbytes, short *data) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

    taskDISABLE_INTERRUPTS();
    spi_master_op(unit, 2, nbytes, NULL, (unsigned char *)data);
    taskENABLE_INTERRUPTS();

    return NULL;
}

/*
 * Send a chunk of 32-bit data.
 */
driver_error_t *spi_bulk_write32(int unit, unsigned int words, int *data) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

    taskDISABLE_INTERRUPTS();
    spi_master_op(unit, 4, words, (unsigned char *)data, NULL);
    taskENABLE_INTERRUPTS();

    return NULL;
}

driver_error_t *spi_bulk_write32_be(int unit, unsigned int words, int *data) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	int i = 0;

    taskDISABLE_INTERRUPTS();

    if (GET_PERI_REG_MASK(SPI_CTRL_REG(unit), (SPI_WR_BIT_ORDER | SPI_RD_BIT_ORDER))) {
        for(;i < words;i++) {
        	data[i] = __builtin_bswap32(data[i]);
        }
    }

    spi_master_op(unit, 4, words, (unsigned char *)data, NULL);

    taskENABLE_INTERRUPTS();

    return NULL;
}

// Read a huge chunk of data as fast and as efficiently as
// possible.  Switches in to 32-bit mode regardless, and uses
// the enhanced buffer mode.
// Data should be a multiple of 32 bits.
driver_error_t *spi_bulk_read32_be(int unit, unsigned int words, int *data) {
	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	int i = 0;

    taskDISABLE_INTERRUPTS();

    spi_master_op(unit, 4, words, NULL, (unsigned char *)data);

    if (GET_PERI_REG_MASK(SPI_CTRL_REG(unit), (SPI_WR_BIT_ORDER | SPI_RD_BIT_ORDER))) {
        for(;i < words;i++) {
        	data[i] = __builtin_bswap32(data[i]);
        }
    }

    taskENABLE_INTERRUPTS();

    return NULL;
}

/*
 * Return the name of the SPI bus for a device.
 */
const char *spi_name(int unit) {
    static const char *name[NSPI] = { "spi0", "spi1", "spi2", "spi3" };
    return name[unit&3];
}

/*
 * Return the pin index of the chip select pin for a device.
 */
int spi_cs_gpio(int unit) {
	spi_interface_t *dev = &spi[unit];

    return dev->res->cs;
}

/*
 * Return the speed in kHz.
 */
unsigned int spi_get_speed(int unit) {
	spi_interface_t *dev = &spi[unit];

    //return dev->speed;
    return spiClockDivToFrequency(dev->divisor);
}

// Init a spi device
driver_error_t *spi_init(int unit, int master) {
	spi_interface_t *dev = &spi[unit];

	// Sanity checks
	if (((unit&3) > CPU_LAST_SPI) || ((unit&3) < CPU_FIRST_SPI)) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_INVALID_UNIT, NULL);
	}

	if (master != 1) {
		return driver_operation_error(SPI_DRIVER, SPI_ERR_SLAVE_NOT_ALLOWED, NULL);
	}

    // Configure default pins if necessary
    if (dev->res->sck == 0) {
    	spi_pin_config(unit, dev->res->sdi, dev->res->sdo, dev->res->sck, dev->res->cs);
    }

    // Lock resources
    driver_error_t *error;
    if ((error = spi_lock_resources(unit, NULL))) {
		return error;
	}

    /*
	// There are not errors, continue with init ...

    // Cotinue with init
	syslog(LOG_INFO,
        "SPI_INIT: spi%u at pins sdi=%s%d/sdo=%s%d/sck=%s%d/cs=%s%d\r\n", unit,
        gpio_portname(dev->res->sdi), gpio_name(dev->res->sdi),
        gpio_portname(dev->res->sdo), gpio_name(dev->res->sdo),
        gpio_portname(dev->res->sck), gpio_name(dev->res->sck),
        gpio_portname(dev->res->cs), gpio_name(dev->res->cs)
    );
	*/
    spi_set_mode(unit, 0);
    spi_set_duplex(unit, 1);

    dev->dirty = 1;
    
    return NULL;
}

DRIVER_REGISTER(SPI,spi,NULL,_spi_init,spi_lock_resources);

void spi_set_dirty(int unit) {
	spi_interface_t *dev = &spi[unit];

    dev->dirty = 1;
}

void spi_set_duplex(int unit, int duplex) {
	spi_interface_t *dev = &spi[unit];

    dev->res->duplex = duplex;
    dev->dirty = 1;
}
