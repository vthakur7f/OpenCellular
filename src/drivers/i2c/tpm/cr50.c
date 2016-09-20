/*
 * Copyright 2016 Google Inc.
 *
 * Based on Linux Kernel TPM driver by
 * Peter Huewe <peter.huewe@infineon.com>
 * Copyright (C) 2011 Infineon Technologies
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * cr50 is a TPM 2.0 capable device that requries special
 * handling for the I2C interface.
 *
 * - Use an interrupt for transaction status instead of hardcoded delays
 * - Must use write+wait+read read protocol
 * - All 4 bytes of status register must be read/written at once
 * - Burst count max is 63 bytes, and burst count behaves
 *   slightly differently than other I2C TPMs
 * - When reading from FIFO the full burstcnt must be read
 *   instead of just reading header and determining the remainder
 */

#include <arch/early_variables.h>
#include <commonlib/endian.h>
#include <stdint.h>
#include <string.h>
#include <types.h>
#include <delay.h>
#include <console/console.h>
#include <device/i2c.h>
#include <endian.h>
#include <timer.h>
#include "tpm.h"


#define CR50_MAX_BUFSIZE	63
#define CR50_TIMEOUT_LONG_MS	2000	/* Long timeout while waiting for TPM */
#define CR50_TIMEOUT_SHORT_MS	2	/* Short timeout during transactions */
#define CR50_DID_VID		0x00281ae0L

struct tpm_inf_dev {
	int bus;
	unsigned int addr;
	uint8_t buf[CR50_MAX_BUFSIZE + sizeof(uint8_t)];
};

static struct tpm_inf_dev g_tpm_dev CAR_GLOBAL;

/*
 * iic_tpm_read() - read from TPM register
 *
 * @addr: register address to read from
 * @buffer: provided by caller
 * @len: number of bytes to read
 *
 * 1) send register address byte 'addr' to the TPM
 * 2) wait for TPM to indicate it is ready
 * 3) read 'len' bytes of TPM response into the provided 'buffer'
 *
 * Return -1 on error, 0 on success.
 */
static int iic_tpm_read(uint8_t addr, uint8_t *buffer, size_t len)
{
	struct tpm_inf_dev *tpm_dev = car_get_var_ptr(&g_tpm_dev);

	if (tpm_dev->addr == 0)
		return -1;

	/* Send the register address byte to the TPM */
	if (i2c_write_raw(tpm_dev->bus, tpm_dev->addr, &addr, 1)) {
		printk(BIOS_ERR, "%s: Address write failed\n", __func__);
		return -1;
	}

	/* Wait for TPM to be ready with response data */
	mdelay(CR50_TIMEOUT_SHORT_MS);

	/* Read response data from the TPM */
	if (i2c_read_raw(tpm_dev->bus, tpm_dev->addr, buffer, len)) {
		printk(BIOS_ERR, "%s: Read response failed\n", __func__);
		return -1;
	}

	return 0;
}

/*
 * iic_tpm_write() - write to TPM register
 *
 * @addr: register address to write to
 * @buffer: data to write
 * @len: number of bytes to write
 *
 * 1) prepend the provided address to the provided data
 * 2) send the address+data to the TPM
 * 3) wait for TPM to indicate it is done writing
 *
 * Returns -1 on error, 0 on success.
 */
static int iic_tpm_write(uint8_t addr, uint8_t *buffer, size_t len)
{
	struct tpm_inf_dev *tpm_dev = car_get_var_ptr(&g_tpm_dev);

	if (tpm_dev->addr == 0)
		return -1;
	if (len > CR50_MAX_BUFSIZE)
		return -1;

	/* Prepend the 'register address' to the buffer */
	tpm_dev->buf[0] = addr;
	memcpy(tpm_dev->buf + 1, buffer, len);

	/* Send write request buffer with address */
	if (i2c_write_raw(tpm_dev->bus, tpm_dev->addr, tpm_dev->buf, len + 1)) {
		printk(BIOS_ERR, "%s: Error writing to TPM\n", __func__);
		return -1;
	}

	/* Wait for TPM to be ready */
	mdelay(CR50_TIMEOUT_SHORT_MS);

	return 0;
}

static int check_locality(struct tpm_chip *chip, int loc)
{
	uint8_t buf;

	if (iic_tpm_read(TPM_ACCESS(loc), &buf, 1) < 0)
		return -1;

	if ((buf & (TPM_ACCESS_ACTIVE_LOCALITY | TPM_ACCESS_VALID)) ==
		(TPM_ACCESS_ACTIVE_LOCALITY | TPM_ACCESS_VALID)) {
		chip->vendor.locality = loc;
		return loc;
	}

	return -1;
}

static void release_locality(struct tpm_chip *chip, int loc, int force)
{
	uint8_t buf;
	if (iic_tpm_read(TPM_ACCESS(loc), &buf, 1) < 0)
		return;

	if (force || (buf & (TPM_ACCESS_REQUEST_PENDING | TPM_ACCESS_VALID)) ==
			(TPM_ACCESS_REQUEST_PENDING | TPM_ACCESS_VALID)) {
		buf = TPM_ACCESS_ACTIVE_LOCALITY;
		iic_tpm_write(TPM_ACCESS(loc), &buf, 1);
	}
}

static int request_locality(struct tpm_chip *chip, int loc)
{
	uint8_t buf = TPM_ACCESS_REQUEST_USE;

	if (check_locality(chip, loc) >= 0)
		return loc; /* we already have the locality */

	iic_tpm_write(TPM_ACCESS(loc), &buf, 1);

	/* wait for burstcount */
	int timeout = 2 * 1000; /* 2s timeout */
	while (timeout) {
		if (check_locality(chip, loc) >= 0)
			return loc;
		mdelay(TPM_TIMEOUT);
		timeout--;
	}

	return -1;
}

/* cr50 requires all 4 bytes of status register to be read */
static uint8_t cr50_tis_i2c_status(struct tpm_chip *chip)
{
	uint8_t buf[4];
	if (iic_tpm_read(TPM_STS(chip->vendor.locality),
			 buf, sizeof(buf)) < 0) {
		printk(BIOS_ERR, "%s: Failed to read status\n", __func__);
		return 0;
	}
	return buf[0];
}

/* cr50 requires all 4 bytes of status register to be written */
static void cr50_tis_i2c_ready(struct tpm_chip *chip)
{
	uint8_t buf[4] = { TPM_STS_COMMAND_READY };
	iic_tpm_write(TPM_STS(chip->vendor.locality), buf, sizeof(buf));
	mdelay(CR50_TIMEOUT_SHORT_MS);
}

/* cr50 uses bytes 3:2 of status register for burst count and
 * all 4 bytes must be read */
static int cr50_wait_burst_status(struct tpm_chip *chip, uint8_t mask,
				  size_t *burst, int *status)
{
	uint8_t buf[4];
	struct stopwatch sw;

	stopwatch_init_msecs_expire(&sw, CR50_TIMEOUT_LONG_MS);

	while (!stopwatch_expired(&sw)) {
		if (iic_tpm_read(TPM_STS(chip->vendor.locality),
				 buf, sizeof(buf)) != 0) {
			printk(BIOS_WARNING, "%s: Read failed\n", __func__);
			mdelay(CR50_TIMEOUT_SHORT_MS);
			continue;
		}

		*status = buf[0];
		*burst = read_le16(&buf[1]);

		/* Check if mask matches and burst is valid */
		if ((*status & mask) == mask &&
		    *burst > 0 && *burst <= CR50_MAX_BUFSIZE)
			return 0;

		mdelay(CR50_TIMEOUT_SHORT_MS);
	}

	printk(BIOS_ERR, "%s: Timeout reading burst and status\n", __func__);
	return -1;
}

static int cr50_tis_i2c_recv(struct tpm_chip *chip, uint8_t *buf,
			     size_t buf_len)
{
	size_t burstcnt, current, len, expected;
	uint8_t addr = TPM_DATA_FIFO(chip->vendor.locality);
	int status;
	int ret = -1;

	if (buf_len < TPM_HEADER_SIZE)
		goto out;

	if (cr50_wait_burst_status(chip, TPM_STS_VALID, &burstcnt, &status) < 0)
		goto out;
	if (!(status & TPM_STS_DATA_AVAIL)) {
		printk(BIOS_ERR, "%s: First chunk not available\n", __func__);
		goto out;
	}

	/* Read first chunk of burstcnt bytes */
	if (iic_tpm_read(addr, buf, burstcnt) != 0) {
		printk(BIOS_ERR, "%s: Read failed\n", __func__);
		goto out;
	}

	/* Determine expected data in the return buffer */
	expected = read_be32(buf + TPM_RSP_SIZE_BYTE);
	if (expected > buf_len) {
		printk(BIOS_ERR, "%s: Too much data: %zu > %zu\n",
		       __func__, expected, buf_len);
		goto out;
	}

	/* Now read the rest of the data */
	current = burstcnt;
	while (current < expected) {
		/* Read updated burst count and check status */
		if (cr50_wait_burst_status(chip, TPM_STS_VALID,
					   &burstcnt, &status) < 0)
			goto out;
		if (!(status & TPM_STS_DATA_AVAIL)) {
			printk(BIOS_ERR, "%s: Data not available\n", __func__);
			goto out;
		}

		len = min(burstcnt, expected - current);
		if (iic_tpm_read(addr, buf + current, len) != 0) {
			printk(BIOS_ERR, "%s: Read failed\n", __func__);
			goto out;
		}

		current += len;
	}

	/* Ensure TPM is done reading data */
	if (cr50_wait_burst_status(chip, TPM_STS_VALID, &burstcnt, &status) < 0)
		goto out;
	if (status & TPM_STS_DATA_AVAIL) {
		printk(BIOS_ERR, "%s: Data still available\n", __func__);
		goto out;
	}

	ret = current;

out:
	return ret;
}

static int cr50_tis_i2c_send(struct tpm_chip *chip, uint8_t *buf, size_t len)
{
	int status;
	size_t burstcnt, limit, sent = 0;
	uint8_t tpm_go[4] = { TPM_STS_GO };
	struct stopwatch sw;

	stopwatch_init_msecs_expire(&sw, CR50_TIMEOUT_LONG_MS);

	/* Wait until TPM is ready for a command */
	while (!(cr50_tis_i2c_status(chip) & TPM_STS_COMMAND_READY)) {
		if (stopwatch_expired(&sw)) {
			printk(BIOS_ERR, "%s: Command ready timeout\n",
			       __func__);
			return -1;
		}

		cr50_tis_i2c_ready(chip);
	}

	while (len > 0) {
		/* Read burst count and check status */
		if (cr50_wait_burst_status(chip, TPM_STS_VALID,
					   &burstcnt, &status) < 0)
			goto out;
		if (sent > 0 && !(status & TPM_STS_DATA_EXPECT)) {
			printk(BIOS_ERR, "%s: Data not expected\n", __func__);
			goto out;
		}

		/* Use burstcnt - 1 to account for the address byte
		 * that is inserted by iic_tpm_write() */
		limit = min(burstcnt - 1, len);
		if (iic_tpm_write(TPM_DATA_FIFO(chip->vendor.locality),
				  &buf[sent], limit) != 0) {
			printk(BIOS_ERR, "%s: Write failed\n", __func__);
			goto out;
		}

		sent += limit;
		len -= limit;
	}

	/* Ensure TPM is not expecting more data */
	if (cr50_wait_burst_status(chip, TPM_STS_VALID, &burstcnt, &status) < 0)
		goto out;
	if (status & TPM_STS_DATA_EXPECT) {
		printk(BIOS_ERR, "%s: Data still expected\n", __func__);
		goto out;
	}

	/* Start the TPM command */
	if (iic_tpm_write(TPM_STS(chip->vendor.locality), tpm_go,
			  sizeof(tpm_go)) < 0) {
		printk(BIOS_ERR, "%s: Start command failed\n", __func__);
		goto out;
	}
	return sent;

out:
	/* Abort current transaction if still pending */
	if (cr50_tis_i2c_status(chip) & TPM_STS_COMMAND_READY)
		cr50_tis_i2c_ready(chip);
	return -1;
}

static void cr50_vendor_init(struct tpm_chip *chip)
{
	memset(&chip->vendor, 0, sizeof(struct tpm_vendor_specific));
	chip->vendor.req_complete_mask = TPM_STS_DATA_AVAIL | TPM_STS_VALID;
	chip->vendor.req_complete_val = TPM_STS_DATA_AVAIL | TPM_STS_VALID;
	chip->vendor.req_canceled = TPM_STS_COMMAND_READY;
	chip->vendor.status = &cr50_tis_i2c_status;
	chip->vendor.recv = &cr50_tis_i2c_recv;
	chip->vendor.send = &cr50_tis_i2c_send;
	chip->vendor.cancel = &cr50_tis_i2c_ready;
}

int tpm_vendor_probe(unsigned bus, uint32_t addr)
{
	struct tpm_inf_dev *tpm_dev = car_get_var_ptr(&g_tpm_dev);
	struct stopwatch sw;
	uint8_t buf = 0;
	int ret;
	long sw_run_duration = CR50_TIMEOUT_LONG_MS;

	tpm_dev->bus = bus;
	tpm_dev->addr = addr;

	/* Wait for TPM_ACCESS register ValidSts bit to be set */
	stopwatch_init_msecs_expire(&sw, sw_run_duration);
	do {
		ret = iic_tpm_read(TPM_ACCESS(0), &buf, 1);
		if (!ret && (buf & TPM_STS_VALID)) {
			sw_run_duration = stopwatch_duration_msecs(&sw);
			break;
		}
		mdelay(CR50_TIMEOUT_SHORT_MS);
	} while (!stopwatch_expired(&sw));

	printk(BIOS_INFO,
	       "%s: ValidSts bit %s(%d) in TPM_ACCESS register after %ld ms\n",
	       __func__, (buf & TPM_STS_VALID) ? "set" : "clear",
	       (buf & TPM_STS_VALID) >> 7, sw_run_duration);

	/* Claim failure if the ValidSts (bit 7) is clear */
	if (!(buf & TPM_STS_VALID))
		return -1;

	return 0;
}

int tpm_vendor_init(struct tpm_chip *chip, unsigned bus, uint32_t dev_addr)
{
	struct tpm_inf_dev *tpm_dev = car_get_var_ptr(&g_tpm_dev);
	uint32_t vendor;

	if (dev_addr == 0) {
		printk(BIOS_ERR, "%s: missing device address\n", __func__);
		return -1;
	}

	tpm_dev->bus = bus;
	tpm_dev->addr = dev_addr;

	cr50_vendor_init(chip);

	/* Disable interrupts (not supported) */
	chip->vendor.irq = 0;

	if (request_locality(chip, 0) != 0)
		return -1;

	/* Read four bytes from DID_VID register */
	if (iic_tpm_read(TPM_DID_VID(0), (uint8_t *)&vendor, 4) < 0)
		goto out_err;

	if (vendor != CR50_DID_VID) {
		printk(BIOS_DEBUG, "Vendor ID 0x%08x not recognized\n", vendor);
		goto out_err;
	}

	printk(BIOS_DEBUG, "cr50 TPM %u:%02x (device-id 0x%X)\n",
	       tpm_dev->bus, tpm_dev->addr, vendor >> 16);

	chip->is_open = 1;
	return 0;

out_err:
	release_locality(chip, 0, 1);
	return -1;
}

void tpm_vendor_cleanup(struct tpm_chip *chip)
{
	release_locality(chip, chip->vendor.locality, 1);
}
