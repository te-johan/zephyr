/*
 * USB audio class core header
 *
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief USB Audio Device Class public header
 *
 * Header follows Device Class Definition for Audio Class
 * Version 1.0 document (audio10.pdf).
 */

#ifndef ZEPHYR_INCLUDE_USB_CLASS_AUDIO_H_
#define ZEPHYR_INCLUDE_USB_CLASS_AUDIO_H_

#include <usb/usb_common.h>
#include <device.h>
#include <net/buf.h>
#include <sys/util.h>

/* Feature Unit Control Selectors */
/* audio10.pdf Table A-11 */
enum feature_unit_control_selectors {
	FU_CONTROL_UNDEFINED		= 0x00,
	MUTE_CONTROL			= 0x01,
	VOLUME_CONTROL			= 0x02,
	BASS_CONTROL			= 0x03,
	MID_CONTROL			= 0x04,
	TREBLE_CONTROL			= 0x05,
	GRAPHIC_EQUALIZER_CONTROL	= 0x06,
	AUTOMATIC_GAIN_CONTROL		= 0x07,
	DELAY_CONTROL			= 0x08,
	BASS_BOOST_CONTROL		= 0x09,
	LOUDNESS_CONTROL		= 0x0A
};

enum direction {
	IN = 0x00,
	OUT = 0x01
};

struct feature_unit_evt {
	struct device *dev;
	enum direction dir;
	enum feature_unit_control_selectors cs;
	u8_t channel;
	void *val;
};

typedef void (*usb_audio_data_request_cb_t)(const struct device *dev);

typedef void (*usb_audio_data_completion_cb_t)(const struct device *dev,
					       struct net_buf *buffer,
					       size_t size);

typedef void (*usb_audio_feature_updated_cb_t)(struct feature_unit_evt evt);

struct audio_ops {
	/* Callback called when data could be send */
	usb_audio_data_request_cb_t data_request_cb;

	/* Callback called on data written event */
	usb_audio_data_completion_cb_t data_written_cb;

	/* Callback called on data received event */
	usb_audio_data_completion_cb_t data_received_cb;

	/* Callback called on features manipulation by Host */
	usb_audio_feature_updated_cb_t feature_update_cb;
};

struct controls {
	bool  mute;
	u16_t volume;
	u8_t  tone_control[3];
	/** TODO: specify size. Leave for now as u8_t
	 * check Table 5-27 audio10.pdf
	 */
	u8_t  graphic_equalizer;
	bool  automatic_gain_control;
	u16_t delay;
	bool  bass_boost;
	bool  loudness;
} __packed;

/**
 * @brief Register the USB Audio device and make it useable.
 *	  This must be called in order to make the device work
 *	  and respond to all relevant requests.
 *
 * @param [in] dev	USB audio device which will send the data
 *			over its ISO IN endpoint
 * @param [in] ops	USB audio callback structure. Callback are used to
 *			inform the user about what is happening
 */
void usb_audio_register(struct device *dev,
			const struct audio_ops *ops);

/**
 * @brief Send data using USB AUDIO device
 *
 * @param [out] buffer Pointer to the allocated buffer pointer
 */
void usb_audio_alloc_buffer(struct net_buf **buffer);

/**
 * @brief Send data using USB AUDIO device
 *
 * @param [in] dev    USB audio device which will send the data
 *		      over its ISO IN endpoint
 * @param [in] buffer Pointer to the buffer that should be send
 * @param [in] len    Length of the data to be send
 *
 * @return 0 on success, negative error on fail
 */
int usb_audio_send(const struct device *dev, struct net_buf *buffer, int len);

#endif /* ZEPHYR_INCLUDE_USB_CLASS_AUDIO_H_ */
