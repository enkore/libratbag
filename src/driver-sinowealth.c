/*
 * Copyright © 2020 Marian Beermann
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "libratbag-private.h"
#include "libratbag-hidraw.h"

#define SINOWEALTH_REPORT_ID_CONFIG 0x4
#define SINOWEALTH_REPORT_ID_CMD 0x5
#define SINOWEALTH_CMD_FIRMWARE_VERSION 0x1
#define SINOWEALTH_CMD_GET_CONFIG 0x11
#define SINOWEALTH_CONFIG_SIZE 520

#define SINOWEALTH_XY_INDEPENDENT 0x80

/* The PC software only goes down to 400, but PMW3360 doesn't care */
#define SINOWEALTH_DPI_MIN 100
#define SINOWEALTH_DPI_MAX 12000
#define SINOWEALTH_DPI_STEP 100

/* other models might have up to eight */
#define SINOWEALTH_NUM_DPIS 6

typedef struct __attribute__((packed)) {
	uint8_t r, g, b;
} RGB8;

enum rgb_effect {
	RGB_OFF = 0,
	RGB_GLORIOUS = 0x1,   /* unicorn mode */
	RGB_SINGLE = 0x2,     /* single constant color */
	RGB_BREATHING = 0x5,  /* RGB breathing */
	RGB_BREATHING7 = 0x3, /* breathing with seven colors */
	RGB_BREATHING1 = 0xa, /* single color breathing */
	RGB_TAIL = 0x4,       /* idk what this is supposed to be */
	RGB_RAVE = 0x7,       /* ig */
	RGB_WAVE = 0x9
};

struct sinowealth_config_report {
	uint8_t report_id; /* SINOWEALTH_REPORT_ID_CONFIG */
	uint8_t command_id;
	uint8_t unk1;
	uint8_t config_write;
	/* always 0 when config is read from device,
	 * has to be 0x7b when writing config to device
	 */
	uint8_t unk2[6];
	uint8_t config;
	/* 0x80 - SINOWEALTH_XY_INDEPENDENT */
	uint8_t dpi_count:4;
	uint8_t active_dpi:4;
	uint8_t dpi_enabled;
	/* bit set: disabled, unset: enabled
	 * this structure has support for eight DPI slots,
	 * but the glorious software only exposes six
	 */
	uint8_t dpi[16];
	/* DPI/CPI is encoded in the way the PMW3360 sensor accepts it
	 * value = (DPI - 100) / 100
	 * If XY are identical, dpi[0-6] contain the sensitivities,
	 * while in XY independent mode each entry takes two chars for X and Y.
	 */
	RGB8 dpi_color[8];

	uint8_t rgb_effect;
	/* see enum rgb_effect */

	char glorious_mode;
	/* 0x40 - brightness (constant)
	 * 0x1/2/3 - speed
	 */
	char glorious_direction;

	RGB8 single_color;

	char breathing_mode;
	/* 0x40 - brightness (constant)
	 * 0x1/2/3 - speed
	 */
	char breathing_colorcount;
	/* 7, constant */
	RGB8 breathing_colors[7];

	char tail_mode;
	/* 0x10/20/30/40 - brightness
	 * 0x1/2/3 - speed
	 */

	char rave_mode;
	/* 0x10/20/30/40 - brightness
	 * 0x1/2/3 - speed
	 */
	RGB8 rave_colors[2];

	char wave_mode;
	/* 0x10/20/30/40 - brightness
	 * 0x1/2/3 - speed
	 */

	char breathing1_mode;
	/* 0x1/2/3 - speed */
	RGB8 breathing1_color;

	char unk4;
	char lift_off_distance;
	/* 0x1 - 2 mm
	 * 0x2 - 3 mm
	 */
} __attribute__((packed));

struct sinowealth_data {
	/* this is kinda unnecessary at this time, but all the other drivers do it too ;) */
	struct sinowealth_config_report config;
};


static int
sinowealth_raw_to_dpi(int raw)
{
	return (raw + 1) * 100;
}

static int
sinowealth_dpi_to_raw(int dpi)
{
	return dpi / 100 - 1;
}

static struct ratbag_color
sinowealth_raw_to_color(RGB8 raw)
{
	return (struct ratbag_color) {.red = raw.r, .green = raw.g, .blue = raw.b};
}

static RGB8
sinowealth_color_to_raw(struct ratbag_color color)
{
	return (RGB8) {.r = color.red, .g = color.green, .b = color.blue};
}

static int
sinowealth_read_profile(struct ratbag_profile *profile)
{
	struct ratbag_device *device = profile->device;
	struct sinowealth_data *drv_data = device->drv_data;
	struct sinowealth_config_report *config = &drv_data->config;
	struct ratbag_resolution *resolution;
	struct ratbag_led *led;
	int num_dpis = (SINOWEALTH_DPI_MAX - SINOWEALTH_DPI_MIN) / SINOWEALTH_DPI_STEP + 1 + 1;
	unsigned int dpis[num_dpis];
	unsigned int hz = 1000; /* TODO */
	int rc;
	uint8_t *data;

	uint8_t cmd[6] = {SINOWEALTH_REPORT_ID_CMD, SINOWEALTH_CMD_GET_CONFIG};
	rc = ratbag_hidraw_set_feature_report(device, SINOWEALTH_REPORT_ID_CMD, cmd, sizeof(cmd));
	if(rc != sizeof(cmd)) {
		log_error(device->ratbag, "Error while sending read config command: %d\n", rc);
		return -1;
	}

	data = zalloc(SINOWEALTH_CONFIG_SIZE);
	rc = ratbag_hidraw_get_feature_report(device, SINOWEALTH_REPORT_ID_CONFIG,
					      data, SINOWEALTH_CONFIG_SIZE);
	/* The GET_FEATURE report length has to be 520, but the actual data returned is less */
	if (rc < sizeof(*config)) {
		log_error(device->ratbag, "Could not read device configuration: %d\n", rc);
		return -1;
	}

	memcpy(config, data, sizeof(*config));
	free(data);

	/* TODO */
	ratbag_profile_set_report_rate_list(profile, &hz, 1);
	ratbag_profile_set_report_rate(profile, hz);

	/* Generate DPI list */
	dpis[0] = 0; /* 0 DPI = disabled */
	for(int i = 1; i < num_dpis; i++) {
		dpis[i] = SINOWEALTH_DPI_MIN + i * SINOWEALTH_DPI_STEP;
	}

	ratbag_profile_for_each_resolution(profile, resolution) {
		if(config->config & SINOWEALTH_XY_INDEPENDENT) {
			resolution->dpi_x = sinowealth_raw_to_dpi(config->dpi[resolution->index * 2]);
			resolution->dpi_y = sinowealth_raw_to_dpi(config->dpi[resolution->index * 2 + 1]);
		} else {
			resolution->dpi_x = sinowealth_raw_to_dpi(config->dpi[resolution->index]);
			resolution->dpi_y = resolution->dpi_x;
		}
		if (config->dpi_enabled & (1<<resolution->index)) {
			/* DPI step is disabled, fake it by setting DPI to 0 */
			resolution->dpi_x = 0;
			resolution->dpi_y = 0;
		}
		resolution->is_active = resolution->index == config->active_dpi - 1;
		resolution->is_default = resolution->is_active;
		ratbag_resolution_set_dpi_list(resolution, dpis, num_dpis);
		ratbag_resolution_set_cap(resolution, RATBAG_RESOLUTION_CAP_SEPARATE_XY_RESOLUTION);
	}

	/* Body lighting */
	led = ratbag_profile_get_led(profile, 0);
	led->type = RATBAG_LED_TYPE_SIDE;
	led->colordepth = RATBAG_LED_COLORDEPTH_RGB_888;
	ratbag_led_set_mode_capability(led, RATBAG_LED_OFF);
	ratbag_led_set_mode_capability(led, RATBAG_LED_ON);
	ratbag_led_set_mode_capability(led, RATBAG_LED_CYCLE);
	ratbag_led_set_mode_capability(led, RATBAG_LED_BREATHING);

	switch (config->rgb_effect) {
	case RGB_OFF:
		led->mode = RATBAG_LED_OFF;
		break;
	case RGB_SINGLE:
		led->mode = RATBAG_LED_ON;
		led->color = sinowealth_raw_to_color(config->single_color);
		break;
	case RGB_GLORIOUS:
	case RGB_BREATHING:
	case RGB_BREATHING7:
	case RGB_TAIL:
	case RGB_RAVE:
	case RGB_WAVE:
		led->mode = RATBAG_LED_CYCLE;
		break;
	case RGB_BREATHING1:
		led->mode = RATBAG_LED_BREATHING;
		led->color = sinowealth_raw_to_color(config->breathing1_color);
		break;
	}
	ratbag_led_unref(led);

	/* DPI indicator LED */
	for (int i = 1; i < SINOWEALTH_NUM_DPIS + 1; i++) {
		led = ratbag_profile_get_led(profile, i);
		led->type = RATBAG_LED_TYPE_DPI;
		led->colordepth = RATBAG_LED_COLORDEPTH_RGB_888;
		led->mode = RATBAG_LED_ON;
		led->color = sinowealth_raw_to_color(config->dpi_color[i - 1]);
		ratbag_led_set_mode_capability(led, RATBAG_LED_ON);
		ratbag_led_unref(led);
	}

	profile->is_active = true;

	return 0;
}

static int
sinowealth_test_hidraw(struct ratbag_device *device)
{
	/* Only the keyboard interface has this report */
	return ratbag_hidraw_has_report(device, SINOWEALTH_REPORT_ID_CONFIG);
}

static int
sinowealth_probe(struct ratbag_device *device)
{
	int rc;
	struct sinowealth_data *drv_data = 0;
	struct ratbag_profile *profile = 0;

	rc = ratbag_find_hidraw(device, sinowealth_test_hidraw);
	if (rc)
		goto err;

	drv_data = zalloc(sizeof(*drv_data));
	ratbag_set_drv_data(device, drv_data);

	/* TODO: Button remapping */
	ratbag_device_init_profiles(device, 1, SINOWEALTH_NUM_DPIS, 0, SINOWEALTH_NUM_DPIS + 1);

	profile = ratbag_device_get_profile(device, 0);
	rc = sinowealth_read_profile(profile);
	if (rc) {
		rc = -ENODEV;
		goto err;
	}

	return 0;

err:
	free(drv_data);
	ratbag_set_drv_data(device, 0);
	return rc;
}

static int
sinowealth_commit(struct ratbag_device *device)
{
	struct ratbag_profile *profile = ratbag_device_get_profile(device, 0);
	struct sinowealth_data *drv_data = device->drv_data;
	struct sinowealth_config_report *config = &drv_data->config;
	struct ratbag_resolution *resolution;
	struct ratbag_led *led;
	uint8_t *data;
	int rc;

	config->config &= ~SINOWEALTH_XY_INDEPENDENT;
	ratbag_profile_for_each_resolution(profile, resolution) {
		if (resolution->dpi_x != resolution->dpi_y && resolution->dpi_x && resolution->dpi_y) {
			config->config &= SINOWEALTH_XY_INDEPENDENT;
			break;
		}
	}

	config->dpi_enabled = 0xFF;
	ratbag_profile_for_each_resolution(profile, resolution) {
		if (config->config & SINOWEALTH_XY_INDEPENDENT) {
			config->dpi[resolution->index * 2] = sinowealth_dpi_to_raw(resolution->dpi_x);
			config->dpi[resolution->index * 2 + 1] = sinowealth_dpi_to_raw(resolution->dpi_y);
		} else {
			config->dpi[resolution->index] = sinowealth_dpi_to_raw(resolution->dpi_x);
		}
		if (resolution->dpi_x && resolution->dpi_y) {
			/* enable DPI step (dpi_enabled is inverted) */
			config->dpi_enabled &= ~(1<<resolution->index);
		}
	}

	/* Body lighting */
	led = ratbag_profile_get_led(profile, 0);
	switch(led->mode) {
	case RATBAG_LED_OFF:
		config->rgb_effect = RGB_OFF;
		break;
	case RATBAG_LED_ON:
		config->rgb_effect = RGB_SINGLE;
		config->single_color = sinowealth_color_to_raw(led->color);
		break;
	case RATBAG_LED_CYCLE:
		config->rgb_effect = RGB_GLORIOUS;
		break;
	case RATBAG_LED_BREATHING:
		config->rgb_effect = RGB_BREATHING1;
		config->breathing1_color = sinowealth_color_to_raw(led->color);
		break;
	}
	ratbag_led_unref(led);

	/* DPI indicator LED */
	for (int i = 1; i < SINOWEALTH_NUM_DPIS + 1; i++) {
		led = ratbag_profile_get_led(profile, i);
		config->dpi_color[i - 1] = sinowealth_color_to_raw(led->color);
		ratbag_led_unref(led);
	}

	config->config_write = 0x7b; /* magic */

	data = zalloc(SINOWEALTH_CONFIG_SIZE);
	memcpy(data, config, sizeof(*config));

	rc = ratbag_hidraw_set_feature_report(device, SINOWEALTH_REPORT_ID_CONFIG, data, SINOWEALTH_CONFIG_SIZE);
	free(data);
	if(rc != SINOWEALTH_CONFIG_SIZE) {
		log_error(device->ratbag, "Error while writing config: %d\n", rc);
		ratbag_profile_unref(profile);
		return -1;
	}

	ratbag_profile_unref(profile);
	return 0;
}

static void
sinowealth_remove(struct ratbag_device *device)
{
	ratbag_close_hidraw(device);
	free(ratbag_get_drv_data(device));
}

struct ratbag_driver sinowealth_driver = {
	.name = "Sinowealth Gaming Mouse",
	.id = "sinowealth",
	.probe = sinowealth_probe,
	.remove = sinowealth_remove,
	.commit = sinowealth_commit
};
