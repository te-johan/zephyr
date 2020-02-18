/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Sample app for Audio class
 */

#include <zephyr.h>
#include <logging/log.h>
#include <usb/usb_device.h>
#include <usb/class/usb_audio.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* This must be global to be able to send data in receive callback
 * from headphones.
 */
struct device *mic_dev;

static void data_received(const struct device *dev,
			  struct net_buf *buffer,
			  size_t size)
{
	int ret;

	if (!buffer || !size) {
		/* This should never happen */
		return;
	}

	LOG_DBG("Received %d data, unref %p", size, buffer);

	ret = usb_audio_send(mic_dev, buffer, size);
	if (ret) {
		net_buf_unref(buffer);
	}
}

static void feature_update(struct feature_unit_evt evt)
{
	LOG_DBG("Control selector %d for channel %d updated",
		evt.cs, evt.channel);
	switch (evt.cs) {
	case MUTE_CONTROL:
	default:
		break;
	}
}

static const struct audio_ops ops = {
	.data_received_cb = data_received,
	.feature_update_cb = feature_update,
};

void main(void)
{
	struct device *hp_dev = device_get_binding("AUDIO_HP_0");
	int ret;

	mic_dev = device_get_binding("AUDIO_MIC_0");

	if (!hp_dev) {
		LOG_ERR("Can not get USB Headphones Device");
		return;
	}

	if (!mic_dev) {
		LOG_ERR("Can not get USB Microphone Device");
		return;
	}

	usb_audio_register(hp_dev, &ops);

	usb_audio_register(mic_dev, &ops);

	ret = usb_enable(NULL);
	if (ret != 0) {
		LOG_ERR("Failed to enable USB");
		return;
	}

	LOG_INF("Entered %s", __func__);
}
