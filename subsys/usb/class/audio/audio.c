/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Audio device class driver
 *
 * Driver for USB Audio device class driver
 */

#include <kernel.h>
#include <usb/usb_common.h>
#include <usb/usb_device.h>
#include <usb_descriptor.h>
#include <usb/usbstruct.h>
#include <usb/class/usb_audio.h>
#include "usb_audio_internal.h"

#include <sys/byteorder.h>
#include <net/buf.h>

/* TODO: Kconfig options for below defines */
#define AUDIO_EP_SIZE	192

#define LOG_LEVEL CONFIG_USB_AUDIO_LOG_LEVEL
#include <logging/log.h>
LOG_MODULE_REGISTER(usb_audio);

static void audio_buffer_destroyed(struct net_buf *buf);

NET_BUF_POOL_FIXED_DEFINE(audio_data_pool, 5,
			  AUDIO_EP_SIZE,
			  audio_buffer_destroyed);

/* Device data structure */
struct usb_audio_dev_data_t {
	const struct audio_ops *ops;

	struct controls *controls[2];

	const struct cs_ac_interface_descriptor_header *header_descr;

	struct usb_dev_data common;

	bool rx_enable;
	bool tx_enable;
};

static sys_slist_t usb_audio_data_devlist;

/**
 * @brief Fill the USB Audio descriptor
 *
 * This macro fills USB descriptor for specific type of device
 * (Heahphones or Microphone) depending on dev param.
 *
 * @note Feature unit has variable length and only 1st field of
 *	 .bmaControls is filled. Later its fixed in usb_fix_descriptor()
 * @note Audio control and Audio streaming interfaces are numerated starting
 *	 from 0 and are later fixed in usb_fix_descriptor()
 *
 * @param [in] dev	Device type. Must be HP/MIC
 * @param [in] i	Instance of device of current type (dev)
 * @param [in] id	Param for counting logic entities
 * @param [in] link	ID of IN/OUT terminal to which General Descriptor
 *			is linked.
 * @param [in] it_type	Input terminal type
 * @param [in] ot_type	Output terminal type
 */
#define DEFINE_AUDIO_DESCRIPTOR(dev, i, id, link, it_type, ot_type)	\
USBD_CLASS_DESCR_DEFINE(primary, audio)					\
struct dev##_descriptor_##i dev##_desc_##i = {				\
	.std_ac_interface = INIT_STD_IF(AUDIOCONTROL, 0, 0, 0),		\
	.ac_interface_header = INIT_CS_AC_IF_HEADER(dev, i, 1),		\
	.input_terminal = INIT_IN_TERMINAL(dev, i, id, it_type),	\
	.feature_unit = INIT_FEATURE_UNIT(dev, i, id + 1, id),		\
	.output_terminal = INIT_OUT_TERMINAL(id + 2, id + 1, ot_type),	\
	.as_interface_alt_0 = INIT_STD_IF(AUDIOSTREAMING, 1, 0, 0),	\
	.as_interface_alt_1 = INIT_STD_IF(AUDIOSTREAMING, 1, 1, 1),	\
	.as_cs_interface = INIT_AS_GENERAL(link),			\
	.format = INIT_AS_FORMAT_I(dev, i),				\
	.std_ep_desc = INIT_STD_AS_AD_EP(dev, i, AUDIO_EP_SIZE),	\
	.cs_ep_desc = INIT_CS_AS_AD_EP,					\
}

/**
 * @brief Fill the USB Audio descriptor
 *
 * This macro fills USB descriptor for specific type of device.
 * Macro is used when the device uses 2 audiostreaming interfaces,
 * eg. Headset
 *
 * @note Feature units have variable length and only 1st field of
 *	 .bmaControls is filled. Its fixed in usb_fix_descriptor()
 * @note Audio control and Audio streaming interfaces are numerated starting
 *	 from 0 and are later fixed in usb_fix_descriptor()
 *
 * @param [in] dev	Device type.
 * @param [in] i	Instance of device of current type (dev)
 * @param [in] id	Param for counting logic entities
 */
#define DEFINE_AUDIO_DESCRIPTOR_BIDIR(dev, i, id)			  \
USBD_CLASS_DESCR_DEFINE(primary, audio)					  \
struct dev##_descriptor_##i dev##_desc_##i = {				  \
	.std_ac_interface = INIT_STD_IF(AUDIOCONTROL, 0, 0, 0),		  \
	.ac_interface_header = INIT_CS_AC_IF_HEADER_BIDIR(dev, i, 2),	  \
	.input_terminal_0 = INIT_IN_TERMINAL(dev, i, id, IO_HEADSET),	  \
	.feature_unit_0 = INIT_FEATURE_UNIT_BIDIR(dev, i, id+1, id, 0),	  \
	.output_terminal_0 = INIT_OUT_TERMINAL(id+2, id+1, USB_STREAMING),\
	.input_terminal_1 = INIT_IN_TERMINAL(dev, i, id+3, USB_STREAMING),\
	.feature_unit_1 = INIT_FEATURE_UNIT_BIDIR(dev, i, id+4, id+3, 1), \
	.output_terminal_1 = INIT_OUT_TERMINAL(id+5, id+4, IO_HEADSET),	  \
	.as_interface_alt_0_0 = INIT_STD_IF(AUDIOSTREAMING, 1, 0, 0),	  \
	.as_interface_alt_0_1 = INIT_STD_IF(AUDIOSTREAMING, 1, 1, 1),	  \
		.as_cs_interface_0 = INIT_AS_GENERAL(id+2),		  \
		.format_0 = INIT_AS_FORMAT_I_BIDIR(dev, i, MIC),	  \
		.std_ep_desc_0 = INIT_STD_AS_AD_EP_BIDIR(dev, i, MIC,	  \
							AUDIO_EP_SIZE),	  \
		.cs_ep_desc_0 = INIT_CS_AS_AD_EP,			  \
	.as_interface_alt_1_0 = INIT_STD_IF(AUDIOSTREAMING, 2, 0, 0),	  \
	.as_interface_alt_1_1 = INIT_STD_IF(AUDIOSTREAMING, 2, 1, 1),	  \
		.as_cs_interface_1 = INIT_AS_GENERAL(id+3),		  \
		.format_1 = INIT_AS_FORMAT_I_BIDIR(dev, i, HP),		  \
		.std_ep_desc_1 = INIT_STD_AS_AD_EP_BIDIR(dev, i, HP,	  \
							AUDIO_EP_SIZE),	  \
		.cs_ep_desc_1 = INIT_CS_AS_AD_EP,			  \
}

#define DEFINE_AUDIO_EP(dev, i, cb)					\
	static struct usb_ep_cfg_data dev##_usb_audio_ep_data_##i[] = {	\
		INIT_EP_DATA(cb, GET_EP_ADDR(dev, i)),			\
	}

#define DEFINE_AUDIO_EP_BIDIR(dev, i)					\
	static struct usb_ep_cfg_data dev##_usb_audio_ep_data_##i[] = {	\
		INIT_EP_DATA(usb_transfer_ep_callback,			\
					GET_EP_ADDR_BIDIR(dev, i, MIC)),\
		INIT_EP_DATA(audio_receive_cb,				\
					GET_EP_ADDR_BIDIR(dev, i, HP)),	\
	}

#define DEFINE_AUDIO_CFG_DATA(dev, i)					 \
	USBD_CFG_DATA_DEFINE(primary, audio)				 \
	struct usb_cfg_data dev##_audio_config_##i = {			 \
		.usb_device_description	= NULL,				 \
		.interface_config = audio_interface_config,		 \
		.interface_descriptor = &dev##_desc_##i,		 \
		.cb_usb_status = audio_cb_usb_status,			 \
		.interface = {						 \
			.class_handler = audio_class_handle_req,	 \
			.custom_handler = NULL,				 \
			.vendor_handler = NULL,				 \
		},							 \
		.num_endpoints = ARRAY_SIZE(dev##_usb_audio_ep_data_##i),\
		.endpoint = dev##_usb_audio_ep_data_##i,		 \
	}

#define DEFINE_AUDIO_DEV_DATA(dev, i)					\
	struct controls dev##_ctrls_##i[CH_CNT(dev, i) + 1];		\
	static struct usb_audio_dev_data_t dev##_audio_dev_data_##i =	\
		{ .controls = {dev##_ctrls_##i, NULL}, }

#define DEFINE_AUDIO_DEV_DATA_BIDIR(dev, i)				\
	struct controls dev##_ctrls0_##i[CH_CNT(dev, i) + 1];		\
	struct controls dev##_ctrls1_##i[CH_CNT(dev, i) + 1];		\
	static struct usb_audio_dev_data_t dev##_audio_dev_data_##i =	\
		{ .controls = {dev##_ctrls0_##i, dev##_ctrls1_##i}, }

/**
 * Helper function for getting channel number directly from the
 * feature unit descriptor.
 */
static u8_t get_num_of_channels(struct feature_unit_descriptor *fu)
{
	return (fu->bLength - FU_FIXED_ELEMS_SIZE)/sizeof(u16_t);
}

/**
 * Helper function for getting supported controls directly from
 * the feature unit descriptor.
 */
static u16_t get_controls(struct feature_unit_descriptor *fu)
{
	return *(u16_t *)((u8_t *)fu + BMA_CONTROLS_OFFSET);
}

/**
 * Helper function for getting the device streaming direction
 */
enum direction get_fu_dir(struct feature_unit_descriptor *fu)
{
	struct output_terminal_descriptor *ot  =
	(struct output_terminal_descriptor *)((u8_t *)fu + fu->bLength);
	enum direction dir;

	if (ot->wTerminalType == USB_STREAMING) {
		dir = IN;
	} else {
		dir = OUT;
	}

	return dir;
}

/**
 * Helper function for fixing controls in feature units descriptors.
 */
static void fix_fu_descriptors(struct usb_if_descriptor *iface)
{
	struct cs_ac_interface_descriptor_header *header;
	struct feature_unit_descriptor *fu;

	header = (struct cs_ac_interface_descriptor_header *)
			((u8_t *)iface + USB_PASSIVE_IF_DESC_SIZE);

	fu = (struct feature_unit_descriptor *)((u8_t *)header +
						header->bLength +
						INPUT_TERMINAL_DESC_SIZE);

	/* start from 1 as elem 0 is filled when descriptor is declared */
	for (int i = 1; i < get_num_of_channels(fu); i++) {
		*(fu->bmaControls + i) = fu->bmaControls[0];
	}

	if (header->bInCollection == 2) {
		fu = (struct feature_unit_descriptor *)((u8_t *)fu +
			fu->bLength +
			INPUT_TERMINAL_DESC_SIZE +
			OUTPUT_TERMINAL_DESC_SIZE);
		for (int i = 1; i < get_num_of_channels(fu); i++) {
			*(fu->bmaControls + i) = fu->bmaControls[0];
		}
	}
}

/**
 * Helper function for getting pointer to feature unit descriptor.
 * This is needed in order to address audio specific requests to proper
 * controls struct.
 */
int get_feature_unit(struct usb_audio_dev_data_t *audio_dev_data,
		     struct feature_unit_descriptor **fu, u8_t fu_id)
{
	*fu = (struct feature_unit_descriptor *)
		((u8_t *)audio_dev_data->header_descr +
		audio_dev_data->header_descr->bLength +
		INPUT_TERMINAL_DESC_SIZE);

	if ((*fu)->bUnitID == fu_id) {
		return 0;
	}
	/* skip to the next Feature Unit */
	*fu = (struct feature_unit_descriptor *)
		(u8_t *)((u8_t *)*fu + (*fu)->bLength +
			INPUT_TERMINAL_DESC_SIZE +
			OUTPUT_TERMINAL_DESC_SIZE);

	return 1;
}

void audio_dc_interface(struct usb_audio_dev_data_t *audio_dev_data,
			struct usb_if_descriptor *set_iface)
{
	const struct cs_ac_interface_descriptor_header *header =
					audio_dev_data->header_descr;
	struct usb_if_descriptor *iface = set_iface;
	struct usb_ep_descriptor *ep_desc;
	bool this = false;

	/** Because this function is invoked for each registered device
	 * There is a need to distinguish which device has been changed
	 * an interface. To acheive that header descriptor is checked.
	 */
	for (int i = 0; i < header->bInCollection; i++) {
		if (set_iface->bInterfaceNumber == header->baInterfaceNr[i]) {
			this = true;
			break;
		}
	}

	if (this) {
		if (!iface->bAlternateSetting) {
			iface = (struct usb_if_descriptor *)((u8_t *)iface +
					USB_PASSIVE_IF_DESC_SIZE);
		}
		ep_desc = (struct usb_ep_descriptor *)
				((u8_t *)iface +
				USB_PASSIVE_IF_DESC_SIZE +
				USB_AC_CS_IF_DESC_SIZE +
				USB_FORMAT_TYPE_I_DESC_SIZE);
		if (ep_desc->bEndpointAddress & 0x80) {
			audio_dev_data->tx_enable =
				set_iface->bAlternateSetting;
		} else {
			audio_dev_data->rx_enable =
				set_iface->bAlternateSetting;
		}
	}
}
/**
 * @brief This is a helper function user to inform the user about
 * possibility to write the data to the device.
 */
void audio_dc_sof(struct usb_cfg_data *cfg,
		  struct usb_audio_dev_data_t *dev_data)
{
	u8_t ep_addr;

	for (int i = 0; i < cfg->num_endpoints; i++) {
		ep_addr = cfg->endpoint[0].ep_addr;
		if ((ep_addr & 0x80) && (dev_data->tx_enable)) {
			if (dev_data->ops && dev_data->ops->data_request_cb) {
				dev_data->ops->data_request_cb(
					dev_data->common.dev);
			}
		}
	}
}

static void audio_interface_config(struct usb_desc_header *head,
				   u8_t bInterfaceNumber)
{
	struct usb_if_descriptor *iface = (struct usb_if_descriptor *)head;
	struct cs_ac_interface_descriptor_header *header;

	fix_fu_descriptors(iface);

	/* Audio Control Interface */
	iface->bInterfaceNumber = bInterfaceNumber;
	header = (struct cs_ac_interface_descriptor_header *)
		 ((u8_t *)iface + iface->bLength);
	header->baInterfaceNr[0] = bInterfaceNumber + 1;

	/* Audio Streaming Interface Passive */
	iface = (struct usb_if_descriptor *)
			  ((u8_t *)header + header->wTotalLength);
	iface->bInterfaceNumber = bInterfaceNumber + 1;

	/* Audio Streaming Interface Active */
	iface = (struct usb_if_descriptor *)
			  ((u8_t *)iface + iface->bLength);
	iface->bInterfaceNumber = bInterfaceNumber + 1;

	if (header->bInCollection == 2) {
		header->baInterfaceNr[1] = bInterfaceNumber + 2;
		/* Audio Streaming Interface Passive */
		iface = (struct usb_if_descriptor *)
			((u8_t *)iface + USB_ACTIVE_IF_DESC_SIZE);
		iface->bInterfaceNumber = bInterfaceNumber + 2;

		/* Audio Streaming Interface Active */
		iface = (struct usb_if_descriptor *)
			((u8_t *)iface + USB_PASSIVE_IF_DESC_SIZE);
		iface->bInterfaceNumber = bInterfaceNumber + 2;
	}
}

void audio_cb_usb_status(struct usb_cfg_data *cfg,
			 enum usb_dc_status_code cb_status,
			 const u8_t *param)
{
	struct usb_if_descriptor *set_iface =
			(struct usb_if_descriptor *)param;
	struct usb_audio_dev_data_t *audio_dev_data;
	struct usb_dev_data *dev_data;

	dev_data = usb_get_dev_data_by_cfg(&usb_audio_data_devlist, cfg);

	if (dev_data == NULL) {
		LOG_ERR("Device data not found for cfg %p", cfg);
		return;
	}

	audio_dev_data = CONTAINER_OF(dev_data, struct usb_audio_dev_data_t,
				      common);

	switch (cb_status) {
	case USB_DC_INTERFACE:
		audio_dc_interface(audio_dev_data, set_iface);
		break;
	case USB_DC_SOF:
		audio_dc_sof(cfg, audio_dev_data);
		break;
	default:
		break;
	}
}

/**
 * @brief Helper funciton for checking if request addresses valid entity
 *
 * This function searches through descriptor if request is addressing valid
 * entity. If any entity with given ent.id is found then ent.subtype is set
 * and true is returned. False is returned if there is no entity with given id.
 *
 * @param [in]     dev_data USB device data.
 * @param [in,out] ent      USB Audio entity addressed by the request.
 *			    .id      [in]  id of searched entity
 *			    .subtype [out] subtype of entity (if found)
 *
 * @return true if successfully found, false otherwise.
 */
static bool is_entity_valid(struct usb_dev_data *dev_data,
			    struct usb_audio_entity *ent)
{
	const struct usb_cfg_data *cfg = dev_data->dev->config->config_info;
	const u8_t *p = cfg->interface_descriptor;
	struct usb_audio_entity_desc_header *head;

	head = (struct usb_audio_entity_desc_header *)p;

	while (head->bLength != 0) {
		if (head->bDescriptorType == USB_CS_INTERFACE_DESC &&
		    head->bDescriptorSubtype != HEADER &&
		    head->bEntityID == ent->id) {
			ent->subtype = (enum audio_cs_ac_int_desc_subtypes)
					head->bDescriptorSubtype;
			return true;
		}
		p += head->bLength;
		head = (struct usb_audio_entity_desc_header *)p;
	}
	return false;
}

/**
 * @brief Handler for feature unit requests.
 *
 * This function handles feature unit specific requests.
 * If request is properly served 0 is returned. Negative errno
 * is returned in case of an error. This leads to setting stall on IN EP0.
 *
 * @param [in]  dev_data USB audio device data.
 * @param [in]  pSetup   Information about the executed request.
 * @param [in]  len      Size of the buffer.
 * @param [out] data     Buffer containing the request result.
 *
 * @return 0 if succesfulf, negative errno otherwise.
 */
static int handle_feature_unit_req(struct usb_audio_dev_data_t *dev_data,
				   struct usb_setup_packet *pSetup,
				   s32_t *len,
				   u8_t **data)
{
	enum feature_unit_control_selectors control_selector;
	struct feature_unit_descriptor *fu = NULL;
	struct feature_unit_evt evt = { .dev = dev_data->common.dev };
	u8_t ch_num, ch_start, ch_end;
	static u8_t tmp_data_ptr[3];
	u8_t data_offset = 0;
	u8_t fu_id = ((pSetup->wIndex) >> 8) & 0xFF;
	u8_t device = get_feature_unit(dev_data, &fu, fu_id);
	int ret = -EINVAL;

	evt.dir = get_fu_dir(fu);
	ch_num = (pSetup->wValue) & 0xFF;
	control_selector = ((pSetup->wValue) >> 8) & 0xFF;
	ch_start = ch_num == 0xFF ? 0 : ch_num;
	ch_end = ch_num == 0xFF ? get_num_of_channels(fu) : ch_num + 1;

	LOG_DBG("CS: %d, CN: %d, len: %d", control_selector, ch_num, *len);

	/* Error checking */
	if (!(BIT(control_selector) & (get_controls(fu) << 1UL))) {
		ret = -EINVAL;
		goto out;
	} else if (ch_num >= get_num_of_channels(fu) &&
		   ch_num != 0xFF) {
		ret = -EINVAL;
		goto out;
	}

	for (int ch = ch_start; ch < ch_end; ch++) {
		switch (control_selector) {
		case MUTE_CONTROL:
			switch (pSetup->bRequest) {
			case SET_CUR:
				memcpy(&dev_data->controls[device][ch].mute,
					(*data + data_offset),
					sizeof(bool));
				/* Inform the user APP by callback */
				if (dev_data->ops->feature_update_cb &&
				    dev_data->ops) {
					evt.cs = control_selector;
					evt.channel = ch;
					evt.val = &dev_data->controls[device][ch].mute;
					dev_data->ops->feature_update_cb(evt);
				}
				ret = 0;
				break;
			case GET_CUR:
				memcpy(&tmp_data_ptr[data_offset],
					(u8_t *)&dev_data->controls[device][ch].mute,
					sizeof(bool));
				ret = 0;
				break;
			default:
				ret = -EINVAL;
			}
			data_offset++;
			break;
		case VOLUME_CONTROL:
		case BASS_CONTROL:
		case MID_CONTROL:
		case TREBLE_CONTROL:
		case GRAPHIC_EQUALIZER_CONTROL:
		case AUTOMATIC_GAIN_CONTROL:
		case DELAY_CONTROL:
		case BASS_BOOST_CONTROL:
		case LOUDNESS_CONTROL:
			break;
		default:
			break;
		}
	}

	/* Process IN request */
	if (REQTYPE_GET_DIR(pSetup->bmRequestType) == REQTYPE_DIR_TO_HOST) {
		*data = tmp_data_ptr;
		*len = data_offset;
	}
out:
	return ret;
}

/**
 * @brief Handler called for class specific interface request.
 *
 * This function handles all class specific interface requests to a usb audio
 * device. If request is properly server then 0 is returned. Returning negative
 * value will lead to set stall on IN EP0.
 *
 * @param pSetup    Information about the executed request.
 * @param len       Size of the buffer.
 * @param data      Buffer containing the request result.
 *
 * @return  0 on success, negative errno code on fail.
 */
static int handle_interface_req(struct usb_setup_packet *pSetup,
				s32_t *len,
				u8_t **data)
{
	struct usb_audio_dev_data_t *audio_dev_data;
	struct usb_dev_data *dev_data;
	struct usb_audio_entity e;

	/* parse wIndex for interface request */
	u8_t interface = (pSetup->wIndex) & 0xFF;
	u8_t entity_id = ((pSetup->wIndex) >> 8) & 0xFF;

	dev_data = usb_get_dev_data_by_iface(&usb_audio_data_devlist,
					     interface);

	if (dev_data == NULL) {
		LOG_ERR("Device data not found for interface %u", interface);
		return -ENODEV;
	}

	audio_dev_data = CONTAINER_OF(dev_data, struct usb_audio_dev_data_t,
				      common);

	e.id = entity_id;
	if (!is_entity_valid(dev_data, &e)) {
		LOG_ERR("Could not find requested entity");
		return -ENODEV;
	}

	switch (e.subtype) {
	case FEATURE_UNIT:
		return handle_feature_unit_req(audio_dev_data,
					       pSetup, len, data);
	case INPUT_TERMINAL:
	case OUTPUT_TERMINAL:
	case MIXER_UNIT:
	case SELECTOR_UNIT:
	case PROCESSING_UNIT:
	case EXTENSION_UNIT:
	default:
		LOG_INF("Currently not supported");
		return -ENODEV;
	}

	return 0;
}

/**
 * @brief Handler called for class specific endpoint request.
 *
 * This function handles all class specific endpoint requests to a usb audio
 * device. If request is properly server then 0 is returned. Returning negative
 * value will lead to set stall on IN EP0.
 *
 * @param pSetup    Information about the executed request.
 * @param len       Size of the buffer.
 * @param data      Buffer containing the request result.
 *
 * @return  0 on success, negative errno code on fail.
 */
static int handle_endpoint_req(struct usb_setup_packet *pSetup,
				      s32_t *len,
				      u8_t **data)
{
	return -1;
}

/**
 * @brief Handler called for Class requests not handled by the USB stack.
 *
 * @param pSetup    Information about the request to execute.
 * @param len       Size of the buffer.
 * @param data      Buffer containing the request result.
 *
 * @return  0 on success, negative errno code on fail.
 */
int audio_class_handle_req(struct usb_setup_packet *pSetup,
			   s32_t *len, u8_t **data)
{
	LOG_INF("bmRequestType 0x%02x, bRequest 0x%02x, wValue 0x%04x,"
		"wIndex 0x%04x, wLength 0x%04x",
		pSetup->bmRequestType, pSetup->bRequest, pSetup->wValue,
		pSetup->wIndex, pSetup->wLength);

	switch (REQTYPE_GET_RECIP(pSetup->bmRequestType)) {
	case REQTYPE_RECIP_INTERFACE:
		if (handle_interface_req(pSetup, len, data) < 0) {
			return -1;
		}
		break;
	case REQTYPE_RECIP_ENDPOINT:
		if (handle_endpoint_req(pSetup, len, data) < 0) {
			return -1;
		}
		break;
	default:
		LOG_ERR("Request receipent invalid");
		return -1;
	}

	return 0;
}

static const struct usb_audio_device_api {
	void (*init)(void);
} usb_audio_api;

static int usb_audio_device_init(struct device *dev)
{
	LOG_DBG("Init Audio Device: dev %p (%s)", dev, dev->config->name);

	return 0;
}

static void audio_write_cb(u8_t ep, int size, void *priv)
{
	struct usb_dev_data *dev_data;
	struct usb_audio_dev_data_t *audio_dev_data;
	struct net_buf *buffer = priv;

	dev_data = usb_get_dev_data_by_ep(&usb_audio_data_devlist, ep);
	audio_dev_data = dev_data->dev->driver_data;

	LOG_DBG("Written %d bytes on ep 0x%02x, *audio_dev_data %p",
		size, ep, audio_dev_data);

	/* Release net_buf back to the pool */
	net_buf_unref(buffer);

	/* Call user callback if user registered one */
	if (audio_dev_data->ops && audio_dev_data->ops->data_written_cb) {
		audio_dev_data->ops->data_written_cb(dev_data->dev, NULL, size);
	}
}

void usb_audio_alloc_buffer(struct net_buf **buffer)
{
	*buffer = net_buf_alloc(&audio_data_pool, K_NO_WAIT);
}

int usb_audio_send(const struct device *dev, struct net_buf *buffer, int len)
{
	struct usb_audio_dev_data_t *audio_dev_data = dev->driver_data;
	struct usb_cfg_data *cfg = (void *)dev->config->config_info;
	/* EP ISO IN is always placed first in the endpoint table */
	u8_t ep = cfg->endpoint[0].ep_addr;

	if (!(ep & 0x80)) {
		LOG_ERR("Wrong device");
		return -EINVAL;
	}

	if (!audio_dev_data->tx_enable) {
		LOG_DBG("sending dropped -> Host chose passive interface");
		return -EAGAIN;
	}

	if (len > buffer->size) {
		LOG_ERR("Cannot send %d bytes, to much data", len);
		return -EINVAL;
	}

	/** buffer passed to *priv because completion callback
	 * needs to release it to the pool
	 */
	usb_transfer(ep, buffer->data, len, USB_TRANS_WRITE | USB_TRANS_NO_ZLP,
		     audio_write_cb, buffer);
	return 0;
}

void audio_receive_cb(u8_t ep, enum usb_dc_ep_cb_status_code status)
{
	struct usb_audio_dev_data_t *dev_data;
	struct usb_dev_data *common;
	struct net_buf *buffer;
	int ret_bytes;
	int ret;

	__ASSERT(status == USB_DC_EP_DATA_OUT, "Invalid ep status");

	common = usb_get_dev_data_by_ep(&usb_audio_data_devlist, ep);
	if (common == NULL) {
		return;
	}

	dev_data = CONTAINER_OF(common, struct usb_audio_dev_data_t, common);

	/** Check is as_active interface is active
	 * If no -> no sense to read data and return from callback
	 */
	if (!dev_data->rx_enable) {
		return;
	}

	usb_audio_alloc_buffer(&buffer);
	if (!buffer) {
		LOG_ERR("Failed to allocate data buffer");
		return;
	}

	ret = usb_read(ep, buffer->data, AUDIO_EP_SIZE, &ret_bytes);

	if (ret) {
		LOG_ERR("ret=%d ", ret);
		net_buf_unref(buffer);
		return;
	}

	if ((!ret_bytes)) {
		LOG_DBG("No data");
		net_buf_unref(buffer);
		return;
	}
	if (dev_data->ops && dev_data->ops->data_received_cb) {
		dev_data->ops->data_received_cb(common->dev, buffer, ret_bytes);
	}
}

void usb_audio_register(struct device *dev,
			const struct audio_ops *ops)
{
	struct usb_audio_dev_data_t *dev_data = dev->driver_data;
	const struct usb_cfg_data *cfg = dev->config->config_info;
	const struct std_if_descriptor *iface_descr = cfg->interface_descriptor;
	const struct cs_ac_interface_descriptor_header *header =
		(struct cs_ac_interface_descriptor_header *)
		((u8_t *)iface_descr + USB_PASSIVE_IF_DESC_SIZE);

	dev_data->ops = ops;
	dev_data->common.dev = dev;
	dev_data->rx_enable = false;
	dev_data->tx_enable = false;
	dev_data->header_descr = header;

	sys_slist_append(&usb_audio_data_devlist, &dev_data->common.node);

	LOG_DBG("Device dev %p dev_data %p cfg %p added to devlist %p",
		dev, dev_data, dev->config->config_info,
		&usb_audio_data_devlist);
}

static void audio_buffer_destroyed(struct net_buf *buf)
{
	net_buf_destroy(buf);
}

#define DEFINE_AUDIO_DEVICE(dev, i)					 \
	DEVICE_AND_API_INIT(dev##_usb_audio_device_##i,			 \
			    CONFIG_USB_AUDIO_DEVICE_NAME "_" #dev "_" #i,\
			    &usb_audio_device_init,			 \
			    &dev##_audio_dev_data_##i,			 \
			    &dev##_audio_config_##i, APPLICATION,	 \
			    CONFIG_KERNEL_INIT_PRIORITY_DEVICE,		 \
			    &usb_audio_api)

#define DECLARE_AUDIO_HP_DESCR_AUTO(i, _) \
	DECLARE_DESCRIPTOR(HP, i, 1)

#define DEFINE_AUDIO_HP_DESCR_AUTO(i, _) \
	DEFINE_AUDIO_DESCRIPTOR(HP, i, HP_ID(i), HP_LINK(i), USB_STREAMING, OUT_HEADPHONES)

#define DEFINE_AUDIO_HP_EP_AUTO(i, _) \
	DEFINE_AUDIO_EP(HP, i, audio_receive_cb)

#define DEFINE_AUDIO_HP_CFG_DATA_AUTO(i, _) \
	DEFINE_AUDIO_CFG_DATA(HP, i)

#define DEFINE_AUDIO_HP_DEV_DATA_AUTO(i, _) \
	DEFINE_AUDIO_DEV_DATA(HP, i)

#define DEFINE_AUDIO_HP_DEVICE_AUTO(i, _) \
	DEFINE_AUDIO_DEVICE(HP, i)

#define DECLARE_AUDIO_MIC_DESCR_AUTO(i, _) \
	DECLARE_DESCRIPTOR(MIC, i, 1)

#define DEFINE_AUDIO_MIC_DESCR_AUTO(i, _) \
	DEFINE_AUDIO_DESCRIPTOR(MIC, i, MIC_ID(i), MIC_LINK(i), IN_MICROPHONE, USB_STREAMING)

#define DEFINE_AUDIO_MIC_EP_AUTO(i, _) \
	DEFINE_AUDIO_EP(MIC, i, usb_transfer_ep_callback)

#define DEFINE_AUDIO_MIC_CFG_DATA_AUTO(i, _) \
	DEFINE_AUDIO_CFG_DATA(MIC, i)

#define DEFINE_AUDIO_MIC_DEV_DATA_AUTO(i, _) \
	DEFINE_AUDIO_DEV_DATA(MIC, i)

#define DEFINE_AUDIO_MIC_DEVICE_AUTO(i, _) \
	DEFINE_AUDIO_DEVICE(MIC, i)

#define DECLARE_AUDIO_HS_DESCR_AUTO(i, _) \
	DECLARE_DESCRIPTOR_BIDIR(HS, i, 2)

#define DEFINE_AUDIO_HS_DESCR_AUTO(i, _) \
	DEFINE_AUDIO_DESCRIPTOR_BIDIR(HS, i, HS_ID(i))

#define DEFINE_AUDIO_HS_EP_AUTO(i, _) \
	DEFINE_AUDIO_EP_BIDIR(HS, i)

#define DEFINE_AUDIO_HS_CFG_DATA_AUTO(i, _) \
	DEFINE_AUDIO_CFG_DATA(HS, i)

#define DEFINE_AUDIO_HS_DEV_DATA_AUTO(i, _) \
	DEFINE_AUDIO_DEV_DATA_BIDIR(HS, i)

#define DEFINE_AUDIO_HS_DEVICE_AUTO(i, _) \
	DEFINE_AUDIO_DEVICE(HS, i)

UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADPHONES_DEVICE_COUNT,
		DECLARE_AUDIO_HP_DESCR_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADPHONES_DEVICE_COUNT,
		DEFINE_AUDIO_HP_DESCR_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADPHONES_DEVICE_COUNT,
		DEFINE_AUDIO_HP_EP_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADPHONES_DEVICE_COUNT,
		DEFINE_AUDIO_HP_CFG_DATA_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADPHONES_DEVICE_COUNT,
		DEFINE_AUDIO_HP_DEV_DATA_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADPHONES_DEVICE_COUNT,
		DEFINE_AUDIO_HP_DEVICE_AUTO, _);

UTIL_LISTIFY(CONFIG_USB_AUDIO_MICROPHONE_DEVICE_COUNT,
		DECLARE_AUDIO_MIC_DESCR_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_MICROPHONE_DEVICE_COUNT,
		DEFINE_AUDIO_MIC_DESCR_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_MICROPHONE_DEVICE_COUNT,
		DEFINE_AUDIO_MIC_EP_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_MICROPHONE_DEVICE_COUNT,
		DEFINE_AUDIO_MIC_CFG_DATA_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_MICROPHONE_DEVICE_COUNT,
		DEFINE_AUDIO_MIC_DEV_DATA_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_MICROPHONE_DEVICE_COUNT,
		DEFINE_AUDIO_MIC_DEVICE_AUTO, _);

UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADSET_DEVICE_COUNT,
		DECLARE_AUDIO_HS_DESCR_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADSET_DEVICE_COUNT,
		DEFINE_AUDIO_HS_DESCR_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADSET_DEVICE_COUNT,
		DEFINE_AUDIO_HS_EP_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADSET_DEVICE_COUNT,
		DEFINE_AUDIO_HS_CFG_DATA_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADSET_DEVICE_COUNT,
		DEFINE_AUDIO_HS_DEV_DATA_AUTO, _);
UTIL_LISTIFY(CONFIG_USB_AUDIO_HEADSET_DEVICE_COUNT,
		DEFINE_AUDIO_HS_DEVICE_AUTO, _);
