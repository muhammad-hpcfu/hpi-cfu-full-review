/*
 * Copyright 2024 Owner Name <ananth.kunchaka@hp.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>

#include "fu-cfu-struct.h"
#include "fu-hpi-cfu-device.h"
#include "fu-hpi-cfu-struct.h"

/*******************************************/
/*            USB PROTOCOL DEFINES         */
/*******************************************/

#define GET_REPORT	   0x01
#define SET_REPORT	   0x09
#define FIRMWARE_REPORT_ID 0x20
#define OFFER_REPORT_ID	   0x25
#define END_POINT_ADDRESS  0x81

#define IN_REPORT_TYPE	    0x0100
#define OUT_REPORT_TYPE	    0x0200
#define FEATURE_REPORT_TYPE 0x0300

#define FU_HPI_CFU_PAYLOAD_LENGTH 52
#define FU_HPI_CFU_DEVICE_TIMEOUT 0 /* ms */

struct _FuHpiCfuDeviceClass {
	FuUsbDeviceClass parent_class;
};

typedef struct {
	guint8 iface_number;
	FuHpiCfuState state;
	guint8 force_version;
	guint8 force_reset;
	gint32 sequence_number;
	gint32 currentaddress;
	gint32 bytes_sent;
	gint32 retry_attempts;
	gint32 payload_file_size;
	gint32 bytes_remaining;
	gint32 last_packet_sent;
	gint32 bulk_acksize;
	gint32 curfilepos;
	gboolean firmware_status;
	gboolean exit_state_machine_framework;
} FuHpiCfuDevicePrivate;

typedef gint32 (*FuHpiCfuStateHandler)(FuHpiCfuDevice *self,
				       FuHpiCfuDevicePrivate *priv,
				       FuProgress *progress,
				       void *options,
				       GError **error);

typedef struct {
	FuFirmware *fw_offer;
	FuFirmware *fw_payload;
} FuHpiCfuHandlerOptions;

FuHpiCfuHandlerOptions handler_options;

typedef struct {
	FuHpiCfuState state_no;
	FuHpiCfuStateHandler handler;
	void *options;
} FuHpiCfuStateMachineFramework;

G_DEFINE_TYPE_WITH_PRIVATE(FuHpiCfuDevice, fu_hpi_cfu_device, FU_TYPE_HID_DEVICE)
#define GET_PRIVATE(o) (fu_hpi_cfu_device_get_instance_private(o))

static gboolean
fu_hpi_cfu_start_entire_transaction(FuHpiCfuDevice *self, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GByteArray) buf_out = g_byte_array_new();
	g_autoptr(GBytes) start_entire_trans_request = NULL;
	guint8 start_entire_transaction_buf[] = {0x25,
						 0x00,
						 0x00,
						 0xff,
						 0xa0,
						 0x00,
						 0x00,
						 0x00,
						 0x00,
						 0x00,
						 0x00,
						 0x00,
						 0x00,
						 0x00,
						 0x00,
						 0x00,
						 0x00};
	g_byte_array_append(buf_out, start_entire_transaction_buf, 16);
	start_entire_trans_request = g_bytes_new(buf_out->data, buf_out->len);
	fu_dump_bytes(G_LOG_DOMAIN,
		      "fu_hpi_cfu_start_entire_transaction sending:",
		      start_entire_trans_request);

	if (!fu_usb_device_control_transfer(FU_USB_DEVICE(self),
					    FU_USB_DIRECTION_HOST_TO_DEVICE,
					    FU_USB_REQUEST_TYPE_VENDOR,
					    FU_USB_RECIPIENT_DEVICE,
					    SET_REPORT,
					    OUT_REPORT_TYPE | OFFER_REPORT_ID,
					    0,
					    buf_out->data,
					    buf_out->len,
					    NULL,
					    FU_HPI_CFU_DEVICE_TIMEOUT,
					    NULL,
					    &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "fu_hpi_cfu_start_entire_transaction with error: %s",
			    error_local->message);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_hpi_cfu_start_entire_transaction_accepted(FuHpiCfuDevice *self,
					     FuHpiCfuDevicePrivate *priv,
					     GError **error)
{
	g_autoptr(GError) error_local = NULL;
	gsize actual_length = 0;
	guint8 buf[128] = {0};
	g_autoptr(GBytes) start_entire_trans_response = NULL;

	if (!fu_usb_device_interrupt_transfer(FU_USB_DEVICE(self),
					      END_POINT_ADDRESS,
					      buf,
					      sizeof(buf),
					      &actual_length,
					      FU_HPI_CFU_DEVICE_TIMEOUT,
					      NULL,
					      &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "start_entire_transaction_accepted with error: %s",
			    error_local->message);
		return FALSE;
	}

	g_debug("fu_hpi_cfu_start_entire_transaction_accepted: total bytes received: %x",
		(guint)actual_length);

	start_entire_trans_response = g_bytes_new(buf, actual_length);
	fu_dump_bytes(G_LOG_DOMAIN, "bytes received", start_entire_trans_response);

	if (buf[13] == 0x01)
		priv->state = FU_HPI_CFU_STATE_START_OFFER_LIST;
	else
		priv->state = FU_HPI_CFU_STATE_ERROR;

	return TRUE;
}

static gboolean
fu_hpi_cfu_send_start_offer_list(FuHpiCfuDevice *self, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GBytes) start_offer_list_request = NULL;

	guint8 start_offer_list_buf[] = {0x25,
					 0x01,
					 0x00,
					 0xff,
					 0xa0,
					 0x00,
					 0x00,
					 0x00,
					 0x00,
					 0x00,
					 0x00,
					 0x00,
					 0x00,
					 0x00,
					 0x00,
					 0x00,
					 0x00};
	start_offer_list_request = g_bytes_new(start_offer_list_buf, sizeof(start_offer_list_buf));
	fu_dump_bytes(G_LOG_DOMAIN,
		      "fu_hpi_cfu_send_start_offer_list: sending",
		      start_offer_list_request);

	if (!fu_usb_device_control_transfer(FU_USB_DEVICE(self),
					    FU_USB_DIRECTION_HOST_TO_DEVICE,
					    FU_USB_REQUEST_TYPE_VENDOR,
					    FU_USB_RECIPIENT_DEVICE,
					    SET_REPORT,
					    OUT_REPORT_TYPE | OFFER_REPORT_ID,
					    0,
					    start_offer_list_buf,
					    sizeof(start_offer_list_buf),
					    NULL,
					    FU_HPI_CFU_DEVICE_TIMEOUT,
					    NULL,
					    &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "send_start_offer_list with error: %s",
			    error_local->message);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_hpi_cfu_send_offer_list_accepted(FuHpiCfuDevice *self, gint *status, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	gsize actual_length = 0;
	guint8 buf[128] = {0};
	g_autoptr(GBytes) send_offer_list_response = NULL;
	*status = 0;

	if (!fu_usb_device_interrupt_transfer(FU_USB_DEVICE(self),
					      END_POINT_ADDRESS,
					      buf,
					      sizeof(buf),
					      &actual_length,
					      FU_HPI_CFU_DEVICE_TIMEOUT,
					      NULL,
					      &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "fu_hpi_cfu_send_offer_list_accepted with error: %s",
			    error_local->message);
		return FALSE;
	}

	g_debug("fu_hpi_cfu_send_offer_list_accepted: total bytes received: %x",
		(guint)actual_length);
	send_offer_list_response = g_bytes_new(buf, actual_length);
	fu_dump_bytes(G_LOG_DOMAIN,
		      "fu_hpi_cfu_send_offer_list_accepted: bytes received",
		      send_offer_list_response);

	/* success */
	if (buf[13] == 0x01) {
		g_debug("fu_hpi_cfu_device_send_offer_list_accepted success.");
	} else {
		if (buf[13] == 0x02) {
			g_warning(
			    "failed fu_hpi_cfu_device_send_offer_list_accepted with reason: %s",
			    fu_cfu_rr_code_to_string(buf[9]));

		} else {
			g_warning(
			    "failed fu_hpi_cfu_device_send_offer_list_accepted with reason: %s",
			    fu_cfu_rr_code_to_string(buf[9]));
		}
	}
	*status = buf[13];

	return TRUE;
}

static guint8
fu_hpi_cfu_set_flag(guint8 val, guint8 position)
{
	return val | (1 << (position - 1));
}

static gboolean
fu_hpi_cfu_send_offer_update_command(FuHpiCfuDevice *self, FuFirmware *fw_offer, GError **error)
{
	g_autoptr(GByteArray) st_req = fu_struct_hpi_cfu_offer_cmd_new();
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GBytes) blob_offer = NULL;
	g_autoptr(GBytes) offer_command_request = NULL;

	const guint8 *data = 0;
	gsize size = 0;
	guchar flag = 0x00;
	guint8 flag_value = 0;

	blob_offer = fu_firmware_get_bytes(fw_offer, NULL);
	data = g_bytes_get_data(blob_offer, &size);

	fu_struct_hpi_cfu_payload_cmd_set_report_id(st_req, OFFER_REPORT_ID);

	if (!fu_memcpy_safe(st_req->data + 1, 16, 0x0, data, 16, 0x0, 16, error)) {
		return FALSE;
	}

	flag_value = fu_hpi_cfu_set_flag(flag, 7);	 /* (Update now) */
	flag_value = fu_hpi_cfu_set_flag(flag_value, 8); /* (Force update version) */
	fu_struct_hpi_cfu_offer_cmd_set_flags(st_req, flag_value);

	offer_command_request = g_bytes_new(st_req->data, st_req->len);
	fu_dump_bytes(G_LOG_DOMAIN,
		      "fu_hpi_cfu_send_offer_update_command sending:",
		      offer_command_request);

	if (!fu_usb_device_control_transfer(FU_USB_DEVICE(self),
					    FU_USB_DIRECTION_HOST_TO_DEVICE,
					    FU_USB_REQUEST_TYPE_VENDOR,
					    FU_USB_RECIPIENT_DEVICE,
					    SET_REPORT,
					    OUT_REPORT_TYPE | FIRMWARE_REPORT_ID,
					    0,
					    st_req->data,
					    st_req->len,
					    NULL,
					    FU_HPI_CFU_DEVICE_TIMEOUT,
					    NULL,
					    &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "send_offer_update_command with error: %s",
			    error_local->message);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_hpi_cfu_firmware_update_offer_accepted(FuHpiCfuDevice *self,
					  FuHpiCfuDevicePrivate *priv,
					  gint *reply,
					  GError **error)
{
	g_autoptr(GError) error_local = NULL;
	gsize actual_length = 0;
	guint8 buf[128] = {0};
	g_autoptr(GBytes) offer_response = NULL;
	*reply = 0;
	if (!fu_usb_device_interrupt_transfer(FU_USB_DEVICE(self),
					      END_POINT_ADDRESS,
					      buf,
					      sizeof(buf),
					      &actual_length,
					      FU_HPI_CFU_DEVICE_TIMEOUT,
					      NULL,
					      &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "fu_hpi_cfu_firmware_update_offer_accepted with error: %s",
			    error_local->message);
		return FALSE;
	}

	g_debug("fu_hpi_cfu_firmware_update_offer_accepted: total bytes received: %x",
		(guint)actual_length);
	offer_response = g_bytes_new(buf, actual_length);
	fu_dump_bytes(G_LOG_DOMAIN,
		      "fu_hpi_cfu_firmware_update_offer_accepted: bytes received",
		      offer_response);

	if (buf[13] == 0x01) {
		g_debug("fu_hpi_cfu_firmware_update_offer_accepted: success.");
	} else {
		if (buf[13] == 0x02) {
			g_debug("fu_hpi_cfu_firmware_update_offer_accepted:reason: %s",
				fu_cfu_rr_code_to_string(buf[9]));
		} else {
			g_debug("fu_hpi_cfu_firmware_update_offer_accepted:reason: %s buf[13] is "
				"not a reject.",
				fu_cfu_rr_code_to_string(buf[9]));
		}
	}
	*reply = buf[13];

	return TRUE;
}

static gboolean
fu_hpi_cfu_read_content_ack(FuHpiCfuDevicePrivate *priv,
			    FuHpiCfuDevice *self,
			    gint *lastpacket,
			    gint *report_id,
			    gint *reason,
			    gint *status,
			    GError **error)
{
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GBytes) read_ack_response = NULL;
	gsize actual_length = 0;
	guint8 buf[128];
	*report_id = 0;
	*status = 0;

	g_debug("fu_hpi_cfu_read_content_ack at sequence_number:%d", priv->sequence_number);
	if (!fu_usb_device_interrupt_transfer(FU_USB_DEVICE(self),
					      END_POINT_ADDRESS,
					      buf,
					      sizeof(buf),
					      &actual_length,
					      FU_HPI_CFU_DEVICE_TIMEOUT,
					      NULL,
					      &error_local)) {
		return FALSE;
	}
	g_debug("fu_hpi_cfu_read_content_ack: bytes_received:%x", (guint)actual_length);
	read_ack_response = g_bytes_new(buf, actual_length);
	fu_dump_bytes(G_LOG_DOMAIN,
		      "fu_hpi_cfu_read_content_ack: bytes received",
		      read_ack_response);

	*report_id = buf[0];
	/* success */
	if (buf[0] == FIRMWARE_REPORT_ID) {
		g_debug("status:%s response:%s",
			fu_cfu_offer_status_to_string(buf[13]),
			fu_cfu_rr_code_to_string(buf[9]));

		if (buf[13] == 0x01) {
			if (priv->last_packet_sent == 1)
				*lastpacket = 1;

			g_debug("fu_hpi_cfu_read_content_ack: last_packet_sent:1");

			*status = buf[13];
		}
		*status = buf[13];
	} else {
		g_debug("read_content_ack: buffer[5]: %02x, response:%s",
			(guchar)buf[5],
			fu_cfu_content_status_to_string(buf[5]));

		if (buf[5] == 0x00) {
			g_debug("read_content_ack:1");

			if (priv->last_packet_sent == 1)
				*lastpacket = 1;

			g_debug("read_content_ack:last_packet_sent:%d", *lastpacket);
			*status = buf[5];
		} else {
			*status = buf[5];
		}
	}
	return TRUE;
}

static gint8
fu_hpi_cfu_firmware_update_offer_rejected(gint8 reply)
{
	gint8 ret = 0;

	if (reply == FU_HPI_CFU_STATE_UPDATE_OFFER_REJECTED) {
		g_debug("fu_hpi_cfu_firmware_update_offer_rejected: "
			"FU_HPI_CFU_STATE_UPDATE_OFFER_REJECTED");
		ret = 1;
	}

	return ret;
}

static gboolean
fu_hpi_cfu_send_end_offer_list(FuHpiCfuDevice *self, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GBytes) end_offer_request = NULL;

	guint8 end_offer_list_buf[] = {0x25,
				       0x02,
				       0x00,
				       0xff,
				       0xa0,
				       0x00,
				       0x00,
				       0x00,
				       0x00,
				       0x00,
				       0x00,
				       0x00,
				       0x00,
				       0x00,
				       0x00,
				       0x00,
				       0x00};
	g_debug("fu_hpi_cfu_send_end_offer_list sending:");
	end_offer_request = g_bytes_new(end_offer_list_buf, sizeof(end_offer_list_buf));
	fu_dump_bytes(G_LOG_DOMAIN, "fu_hpi_cfu_send_end_offer_list sending:", end_offer_request);

	if (!fu_usb_device_control_transfer(FU_USB_DEVICE(self),
					    FU_USB_DIRECTION_HOST_TO_DEVICE,
					    FU_USB_REQUEST_TYPE_VENDOR,
					    FU_USB_RECIPIENT_DEVICE,
					    SET_REPORT,
					    OUT_REPORT_TYPE | OFFER_REPORT_ID,
					    0,
					    end_offer_list_buf,
					    sizeof(end_offer_list_buf),
					    NULL,
					    FU_HPI_CFU_DEVICE_TIMEOUT,
					    NULL,
					    &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "send_end_offer_list with error: %s",
			    error_local->message);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_hpi_cfu_end_offer_list_accepted(FuHpiCfuDevice *self, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	gsize actual_length = 0;
	guint8 buf[128] = {0};
	g_autoptr(GBytes) end_offer_response = NULL;

	if (!fu_usb_device_interrupt_transfer(FU_USB_DEVICE(self),
					      END_POINT_ADDRESS,
					      buf,
					      sizeof(buf),
					      &actual_length,
					      FU_HPI_CFU_DEVICE_TIMEOUT,
					      NULL,
					      &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_NOT_SUPPORTED,
			    "end_offer_list_accepted: %s",
			    error_local->message);
		return FALSE;
	}

	g_debug("fu_hpi_cfu_end_offer_list_accepted: bytes_received:%x", (guint)actual_length);
	end_offer_response = g_bytes_new(buf, actual_length);
	fu_dump_bytes(G_LOG_DOMAIN,
		      "fu_hpi_cfu_end_offer_list_accepted: bytes received",
		      end_offer_response);

	g_debug("fu_hpi_cfu_end_offer_list_accepted: Identify Type buffer[4]:%02x", (guchar)buf[4]);
	g_debug("fu_hpi_cfu_end_offer_list_accepted: reject reason:buffer[9]:%02x, this value is "
		"meaningful when buffer[13]=2",
		(guchar)buf[9]);
	g_debug("fu_hpi_cfu_end_offer_list_accepted: reply status:buffer[13]:%02x, response:%s",
		(guchar)buf[13],
		fu_cfu_rr_code_to_string(buf[13]));

	/* success */
	if (buf[13] == 0x01) {
		g_debug("fu_hpi_cfu_end_offer_list_accepted: buffer[13]:1");
	} else {
		if (buf[13] == 0x02) {
			g_warning("fu_hpi_cfu_end_offer_list_accepted: not acceptance with "
				  "reason:%s",
				  fu_cfu_rr_code_to_string(buf[9]));
		} else {
			g_warning("fu_hpi_cfu_end_offer_list_accepted: not acceptance with "
				  "reason:%s but buf[13] is not REJECT",
				  fu_cfu_rr_code_to_string(buf[9]));
		}
	}

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_start_entire_transaction(FuHpiCfuDevice *self,
					    FuHpiCfuDevicePrivate *priv,
					    FuProgress *progress,
					    void *options,
					    GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (!fu_hpi_cfu_start_entire_transaction(self, error)) {
		priv->state = FU_HPI_CFU_STATE_ERROR;
		return FALSE;
	} else
		priv->state = FU_HPI_CFU_STATE_START_ENTIRE_TRANSACTION_ACCEPTED;

	/* sucess */
	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_start_entire_transaction_accepted(FuHpiCfuDevice *self,
						     FuHpiCfuDevicePrivate *priv,
						     FuProgress *progress,
						     void *options,
						     GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (!fu_hpi_cfu_start_entire_transaction_accepted(self, priv, error)) {
		priv->state = FU_HPI_CFU_STATE_ERROR;
		return FALSE;
	}

	priv->state = FU_HPI_CFU_STATE_START_OFFER_LIST;
	fu_progress_step_done(progress); /* start-entire  */

	/* sucess */
	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_send_start_offer_list(FuHpiCfuDevice *self,
					 FuHpiCfuDevicePrivate *priv,
					 FuProgress *progress,
					 void *options,
					 GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (!fu_hpi_cfu_send_start_offer_list(self, error)) {
		priv->state = FU_HPI_CFU_STATE_ERROR;
		return FALSE;
	} else
		priv->state = FU_HPI_CFU_STATE_START_OFFER_LIST_ACCEPTED;

	/* sucess */
	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_send_start_offer_list_accepted(FuHpiCfuDevice *self,
						  FuHpiCfuDevicePrivate *priv,
						  FuProgress *progress,
						  void *options,
						  GError **error)
{
	gint32 status = 0;

	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (!fu_hpi_cfu_send_offer_list_accepted(self, &status, error)) {
		priv->state = FU_HPI_CFU_STATE_UPDATE_STOP;
		return FALSE;
	}

	if (status >= 0)
		priv->state = FU_HPI_CFU_STATE_UPDATE_OFFER;
	else
		priv->state = FU_HPI_CFU_STATE_UPDATE_STOP;

	fu_progress_step_done(progress); /* offer-accept  */

	/* sucess */
	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_send_offer_update_command(FuHpiCfuDevice *self,
					     FuHpiCfuDevicePrivate *priv,
					     FuProgress *progress,
					     void *options,
					     GError **error)
{
	FuHpiCfuHandlerOptions *update_options;

	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	update_options = (FuHpiCfuHandlerOptions *)options;

	if (!fu_hpi_cfu_send_offer_update_command(self, update_options->fw_offer, error)) {
		priv->state = FU_HPI_CFU_STATE_ERROR;
		return FALSE;
	} else
		priv->state = FU_HPI_CFU_STATE_UPDATE_OFFER_ACCEPTED;

	/* sucess */
	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_send_offer_accepted(FuHpiCfuDevice *self,
				       FuHpiCfuDevicePrivate *priv,
				       FuProgress *progress,
				       void *options,
				       GError **error)
{
	gint32 reply = 0;

	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (!fu_hpi_cfu_firmware_update_offer_accepted(self, priv, &reply, error)) {
		priv->state = FU_HPI_CFU_STATE_ERROR;
		return FALSE;
	}
	if (reply == FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_ACCEPT) {
		g_debug("fu_hpi_cfu_firmware_update_offer_accepted: reply:%d, offer accepted",
			reply);
		priv->sequence_number = 0;
		priv->currentaddress = 0;
		priv->last_packet_sent = 0;
		priv->state = FU_HPI_CFU_STATE_UPDATE_CONTENT;
	} else {
		if (reply == FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_SKIP) {
			g_debug(
			    "fu_hpi_cfu_firmware_update_offer_accepted: reply:%d, OFFER_SKIPPED",
			    reply);
			priv->state = FU_HPI_CFU_STATE_UPDATE_MORE_OFFERS;
		} else if (reply == FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_REJECT) {
			g_debug(
			    "fu_hpi_cfu_firmware_update_offer_accepted: reply:%d, OFFER_REJECTED",
			    reply);
			priv->state = FU_HPI_CFU_STATE_UPDATE_MORE_OFFERS;
		} else if (reply == FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_BUSY) {
			g_debug("fu_hpi_cfu_firmware_update_offer_accepted: reply:%d, OFFER_BUSY",
				reply);
			priv->retry_attempts++;
			priv->state = FU_HPI_CFU_STATE_START_ENTIRE_TRANSACTION;

			if (priv->retry_attempts > 3) {
				priv->state = FU_HPI_CFU_STATE_NOTIFY_ON_READY;
				g_warning("fu_hpi_cfu_handler_send_offer_accepted after 3 retry "
					  "attempts.Restart "
					  "the device(Reason: Device busy)");
			} else
				priv->state = FU_HPI_CFU_STATE_START_ENTIRE_TRANSACTION;
		} else {
			priv->state = FU_HPI_CFU_STATE_UPDATE_MORE_OFFERS;
		}
	}

	fu_progress_step_done(progress); /* send-offer */

	/* sucess */
	return TRUE;
}

static gboolean
fu_hpi_cfu_send_payload(FuHpiCfuDevice *self,
			FuHpiCfuDevicePrivate *priv,
			GByteArray *cfu_buf,
			GError **error)
{
	g_autoptr(GBytes) fw_content_command = NULL;
	g_autoptr(GByteArray) st_req = fu_struct_hpi_cfu_payload_cmd_new();
	g_autoptr(GError) error_local = NULL;

	fu_struct_hpi_cfu_payload_cmd_set_report_id(st_req, FIRMWARE_REPORT_ID);

	priv->sequence_number++;

	if (priv->sequence_number == 1) {
		g_debug("first packet setting the flag FU_CFU_CONTENT_FLAG_FIRST_BLOCK");
		fu_struct_hpi_cfu_payload_cmd_set_flags(st_req, FU_CFU_CONTENT_FLAG_FIRST_BLOCK);
	}

	if (priv->last_packet_sent) {
		g_debug("last packet setting the flag FU_CFU_CONTENT_FLAG_LAST_BLOCK");
		fu_struct_hpi_cfu_payload_cmd_set_flags(st_req, FU_CFU_CONTENT_FLAG_LAST_BLOCK);
	}

	fu_struct_hpi_cfu_payload_cmd_set_length(st_req, cfu_buf->len);
	fu_struct_hpi_cfu_payload_cmd_set_seq_number(st_req, priv->sequence_number);
	fu_struct_hpi_cfu_payload_cmd_set_address(st_req, priv->currentaddress);

	if (!fu_struct_hpi_cfu_payload_cmd_set_data(st_req, cfu_buf->data, cfu_buf->len, error)) {
		return FALSE;
	}

	priv->currentaddress += cfu_buf->len;
	priv->bytes_sent += cfu_buf->len;
	priv->bytes_remaining = priv->payload_file_size - (priv->bytes_sent + 5);

	fw_content_command = g_bytes_new(st_req->data, st_req->len);
	fu_dump_bytes(G_LOG_DOMAIN, "bytes sending to device", fw_content_command);

	if (!fu_usb_device_control_transfer(FU_USB_DEVICE(self),
					    FU_USB_DIRECTION_HOST_TO_DEVICE,
					    FU_USB_REQUEST_TYPE_VENDOR,
					    FU_USB_RECIPIENT_DEVICE,
					    SET_REPORT,
					    OUT_REPORT_TYPE | FIRMWARE_REPORT_ID,
					    0,
					    st_req->data,
					    st_req->len,
					    NULL,
					    FU_HPI_CFU_DEVICE_TIMEOUT,
					    NULL,
					    &error_local)) {
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_hpi_cfu_untransmitted_data(GByteArray *payload_data,
			      GByteArray *untransmitted_data,
			      guint32 payload_header_length,
			      guint32 fill_from_position,
			      GError **error)
{
	guint32 remaining_byte_count = 0;
	remaining_byte_count = payload_header_length - fill_from_position;

	fu_byte_array_set_size(untransmitted_data, remaining_byte_count, 0x00);

	if (!fu_memcpy_safe(untransmitted_data->data,
			    remaining_byte_count,
			    0x0,
			    payload_data->data + fill_from_position,
			    remaining_byte_count,
			    0x0,
			    remaining_byte_count,
			    error))
		return FALSE;

	return TRUE;
}

static gboolean
fu_hpi_cfu_get_payload_header(GByteArray *payload_header,
			      GByteArray *payload_buf,
			      guint32 read_index,
			      GError **error)
{
	fu_byte_array_set_size(payload_header, 5, 0x00);

	if (!fu_memcpy_safe(payload_header->data,
			    payload_header->len,
			    0x0,
			    payload_buf->data + read_index,
			    5,
			    0x0,
			    5,
			    error))
		return FALSE;

	return TRUE;
}

static gboolean
fu_hpi_cfu_get_payload_data(GByteArray *payload_data,
			    GByteArray *payload_buf,
			    guint32 payload_header_length,
			    guint32 read_index,
			    GError **error)
{
	fu_byte_array_set_size(payload_data, payload_header_length, 0x00);

	if (!fu_memcpy_safe(payload_data->data,
			    payload_data->len,
			    0x0,
			    payload_buf->data + read_index + 5,
			    payload_header_length,
			    0x0,
			    payload_header_length,
			    error))
		return FALSE;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_check_update_content(FuHpiCfuDevice *self,
					FuHpiCfuDevicePrivate *priv,
					FuProgress *progress,
					void *options,
					GError **error)
{
	gint32 lastpacket = 0;
	gint32 wait_for_burst_ack = 0;
	gint32 status = 0;
	gint32 report_id = 0;
	gint32 reason = 0;

	if (priv->last_packet_sent) {
		g_debug("fu_hpi_cfu_handler_check_update_content: last_packet_sent");
		if (!fu_hpi_cfu_read_content_ack(priv,
						 self,
						 &lastpacket,
						 &report_id,
						 &reason,
						 &status,
						 error))
			return FALSE;
	} else {
		switch (priv->bulk_acksize) {
		case 1:
			if ((priv->sequence_number) % 16 == 0) {
				if (!fu_hpi_cfu_read_content_ack(priv,
								 self,
								 &lastpacket,
								 &report_id,
								 &reason,
								 &status,
								 error))
					return FALSE;
			} else {
				priv->state = FU_HPI_CFU_STATE_UPDATE_CONTENT;
				wait_for_burst_ack = 1;
			}
			break;

		case 2:
			if ((priv->sequence_number) % 32 == 0) {
				if (!fu_hpi_cfu_read_content_ack(priv,
								 self,
								 &lastpacket,
								 &report_id,
								 &reason,
								 &status,
								 error))
					return FALSE;
			} else {
				priv->state = FU_HPI_CFU_STATE_UPDATE_CONTENT;
				wait_for_burst_ack = 1;
			}
			break;

		case 3:
			if ((priv->sequence_number) % 64 == 0) {
				if (!fu_hpi_cfu_read_content_ack(priv,
								 self,
								 &lastpacket,
								 &report_id,
								 &reason,
								 &status,
								 error))
					return FALSE;
			} else {
				priv->state = FU_HPI_CFU_STATE_UPDATE_CONTENT;
				wait_for_burst_ack = 1;
			}
			break;

		default:
			if (!fu_hpi_cfu_read_content_ack(priv,
							 self,
							 &lastpacket,
							 &report_id,
							 &reason,
							 &status,
							 error))
				return FALSE;
		}
	}

	if (wait_for_burst_ack) {
		return TRUE;
	}

	if (priv->last_packet_sent) {
		priv->state = FU_HPI_CFU_STATE_UPDATE_SUCCESS;
	} else
		priv->state = FU_HPI_CFU_STATE_UPDATE_CONTENT;

	if (status < 0) {
		g_debug("fu_hpi_cfu_handler_check_update_content: FU_HPI_CFU_STATE_ERROR");
		priv->state = FU_HPI_CFU_STATE_ERROR;
	} else {
		if (report_id == 0x25) {
			g_debug("fu_hpi_cfu_handler_check_update_content: report_id:%d",
				report_id == FIRMWARE_REPORT_ID);
			switch (status) {
			case FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_SKIP:
				g_debug("fu_hpi_cfu_handler_check_update_content: OFFER_SKIPPED");
				priv->state = FU_HPI_CFU_STATE_UPDATE_MORE_OFFERS;
				break;

			case FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_ACCEPT:
				g_debug("fu_hpi_cfu_handler_check_update_content: OFFER_ACCEPTED");
				if (lastpacket) {
					g_debug("fu_hpi_cfu_handler_check_update_content: "
						"OFFER_ACCEPTED last_packet_sent");
					priv->state = FU_HPI_CFU_STATE_UPDATE_SUCCESS;
				} else
					priv->state = FU_HPI_CFU_STATE_UPDATE_CONTENT;
				break;

			case FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_REJECT:
				g_warning("fu_hpi_cfu_handler_check_update_content: "
					  "FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_REJECTED");

				priv->state = FU_HPI_CFU_STATE_UPDATE_MORE_OFFERS;
				break;

			case FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_BUSY:
				g_warning("fu_hpi_cfu_handler_check_update_content: "
					  "FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_BUSY");
				priv->state = FU_HPI_CFU_STATE_NOTIFY_ON_READY;
				break;

			case FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_COMMAND_READY:
				g_debug("fu_hpi_cfu_handler_check_update_content: "
					"FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_COMMAND_READY");
				priv->state = FU_HPI_CFU_STATE_UPDATE_MORE_OFFERS;
				break;
			case FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_CMD_NOT_SUPPORTED:
				g_warning("fu_hpi_cfu_handler_check_update_content: "
					  "FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_CMD_NOT_SUPPORTED");
				priv->state = FU_HPI_CFU_STATE_UPDATE_MORE_OFFERS;
				break;
			default:
				g_warning("fu_hpi_cfu_handler_check_update_content: "
					  "FU_HPI_CFU_STATE_ERROR");
				priv->state = FU_HPI_CFU_STATE_ERROR;
				break;
			}
		} else if (report_id == 0x22) {
			g_debug("fu_hpi_cfu_handler_check_update_content: report_id:0x22");

			switch (status) {
			case FU_HPI_FIRMWARE_UPDATE_STATUS_ERROR_PREPARE:
			case FU_HPI_FIRMWARE_UPDATE_STATUS_ERROR_WRITE:
			case FU_HPI_FIRMWARE_UPDATE_STATUS_ERROR_COMPLETE:
			case FU_HPI_FIRMWARE_UPDATE_STATUS_ERROR_VERIFY:
			case FU_HPI_FIRMWARE_UPDATE_STATUS_ERROR_CRC:
			case FU_HPI_FIRMWARE_UPDATE_STATUS_ERROR_SIGNATURE:
			case FU_HPI_FIRMWARE_UPDATE_STATUS_ERROR_VERSION:
			case FU_HPI_FIRMWARE_UPDATE_STATUS_SWAP_PENDING:
			case FU_HPI_FIRMWARE_UPDATE_STATUS_ERROR_INVALID_ADDR:
			case FU_HPI_FIRMWARE_UPDATE_STATUS_ERROR_NO_OFFER:
			case FU_HPI_FIRMWARE_UPDATE_STATUS_ERROR_INVALID:
				priv->state = FU_HPI_CFU_STATE_ERROR;
				g_warning(
				    "fu_hpi_cfu_handler_check_update_content: FAILED, reason:%s",
				    fu_cfu_content_status_to_string(status));
				break;

			case FU_HPI_FIRMWARE_UPDATE_STATUS_SUCCESS:
				g_debug("fu_hpi_cfu_handler_check_update_content: SUCCESS");
				if (lastpacket) {
					priv->state = FU_HPI_CFU_STATE_UPDATE_SUCCESS;
				} else
					priv->state = FU_HPI_CFU_STATE_UPDATE_CONTENT;
				break;

			default:
				g_warning("fu_hpi_cfu_handler_check_update_content: status none.");
				break;
			}
		}
	}

	/* sucess */
	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_send_payload(FuHpiCfuDevice *self,
				FuHpiCfuDevicePrivate *priv,
				FuProgress *progress,
				void *options,
				GError **error)
{
	g_autoptr(GPtrArray) chunks = NULL;
	FuHpiCfuHandlerOptions *update_options = NULL;

	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	update_options = (FuHpiCfuHandlerOptions *)options;

	chunks = fu_firmware_get_chunks(update_options->fw_payload, error);
	if (chunks == NULL) {
		g_set_error(error, FWUPD_ERROR, FWUPD_ERROR_INVALID_DATA, "payload chunks is NULL");
		return FALSE;
	}

	for (guint32 i = 0; i < chunks->len; i++) {
		FuChunk *chk = g_ptr_array_index(chunks, i);
		g_autoptr(GByteArray) payload_buf = g_byte_array_new();
		g_autoptr(GByteArray) untransmitted_data = g_byte_array_new();

		g_byte_array_append(payload_buf, fu_chunk_get_data(chk), fu_chunk_get_data_sz(chk));

		for (guint32 read_index = 0; read_index < payload_buf->len;) {
			g_autoptr(GByteArray) payload_header = g_byte_array_new();
			g_autoptr(GByteArray) payload_data = g_byte_array_new();
			g_autoptr(GByteArray) cfu_data = g_byte_array_new();
			guint32 payload_header_length = 0;
			guint32 remaining_byte_count = 0;
			guint32 fill_from_position = 0;

			/* payload header */
			if (!fu_hpi_cfu_get_payload_header(payload_header,
							   payload_buf,
							   read_index,
							   error)) {
				g_set_error(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INVALID_DATA,
					    "failed to get payload header");
				return FALSE;
			}

			payload_header_length = (gint8)payload_header->data[4];

			/* payload data */
			if (!fu_hpi_cfu_get_payload_data(payload_data,
							 payload_buf,
							 payload_header_length,
							 read_index,
							 error)) {
				g_set_error(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INVALID_DATA,
					    "failed to get payload data");
				return FALSE;
			}

			read_index = read_index + payload_header_length + 5;
			priv->last_packet_sent = (read_index >= payload_buf->len) ? 1 : 0;

			/* send payload with existing untransmitted_data */
			if (untransmitted_data->data != NULL) {
				if (untransmitted_data->len >= FU_HPI_CFU_PAYLOAD_LENGTH) {
					/* cfu_data to be sent to device */
					g_byte_array_append(cfu_data,
							    untransmitted_data->data,
							    FU_HPI_CFU_PAYLOAD_LENGTH);
					if (!fu_hpi_cfu_send_payload(self, priv, cfu_data, error)) {
						g_set_error(error,
							    FWUPD_ERROR,
							    FWUPD_ERROR_INVALID_DATA,
							    "fu_hpi_cfu_handler_send_payload at "
							    "sequence number:%d",
							    priv->sequence_number);
						return FALSE;
					}

					remaining_byte_count =
					    untransmitted_data->len - FU_HPI_CFU_PAYLOAD_LENGTH;
					fill_from_position =
					    untransmitted_data->len - remaining_byte_count;

					if (remaining_byte_count) {
						g_autoptr(GByteArray) untransmitted_remain =
						    g_byte_array_new();

						/* store the untransmitted_data remaining data */
						if (!fu_hpi_cfu_untransmitted_data(
							untransmitted_data,
							untransmitted_remain,
							untransmitted_data->len,
							fill_from_position,
							error)) {
							g_set_error(
							    error,
							    FWUPD_ERROR,
							    FWUPD_ERROR_INVALID_DATA,
							    "fu_hpi_cfu_handler_send_payload to "
							    "set untransmitted_data");
							return FALSE;
						}
					}
				} else {
					/* append untransmitted_data first */
					g_byte_array_append(cfu_data,
							    untransmitted_data->data,
							    untransmitted_data->len);

					fill_from_position =
					    FU_HPI_CFU_PAYLOAD_LENGTH - untransmitted_data->len;
					remaining_byte_count =
					    payload_header_length - fill_from_position;

					/* Now, append actual payload_data */
					g_byte_array_append(cfu_data,
							    payload_data->data,
							    fill_from_position);

					if (!fu_hpi_cfu_send_payload(self, priv, cfu_data, error)) {
						g_set_error(error,
							    FWUPD_ERROR,
							    FWUPD_ERROR_INVALID_DATA,
							    "fu_hpi_cfu_handler_send_payload for "
							    "sequence number:%d",
							    priv->sequence_number);
						return FALSE;
					}

					if (remaining_byte_count >= FU_HPI_CFU_PAYLOAD_LENGTH) {
						g_autoptr(GByteArray) cfu_data_remain =
						    g_byte_array_new();

						/* append remaining payload_data first */
						g_byte_array_append(cfu_data_remain,
								    payload_data->data +
									fill_from_position,
								    FU_HPI_CFU_PAYLOAD_LENGTH);

						if (!fu_hpi_cfu_send_payload(self,
									     priv,
									     cfu_data_remain,
									     error)) {
							g_set_error(
							    error,
							    FWUPD_ERROR,
							    FWUPD_ERROR_INVALID_DATA,
							    "fu_hpi_cfu_handler_send_payload "
							    "for sequence number:%d",
							    priv->sequence_number);
							return FALSE;
						}

						remaining_byte_count = remaining_byte_count -
								       FU_HPI_CFU_PAYLOAD_LENGTH;
						fill_from_position =
						    payload_header_length - remaining_byte_count;

						/* store the untransmitted_data */
						if (!fu_hpi_cfu_untransmitted_data(
							payload_data,
							untransmitted_data,
							payload_header_length,
							fill_from_position,
							error)) {
							g_set_error(
							    error,
							    FWUPD_ERROR,
							    FWUPD_ERROR_INVALID_DATA,
							    "fu_hpi_cfu_handler_send_payload to "
							    "set untransmitted_data");
							return FALSE;
						}
					} else {
						/* store the untransmitted_data */
						if (!fu_hpi_cfu_untransmitted_data(
							payload_data,
							untransmitted_data,
							payload_header_length,
							fill_from_position,
							error)) {
							g_set_error(
							    error,
							    FWUPD_ERROR,
							    FWUPD_ERROR_INVALID_DATA,
							    "fu_hpi_cfu_handler_send_payload to "
							    "set untransmitted_data");
							return FALSE;
						}
					}
				}
			} else {
				if (payload_header_length > FU_HPI_CFU_PAYLOAD_LENGTH) {
					/* cfu_data to be sent to device */
					g_byte_array_append(cfu_data,
							    payload_data->data,
							    FU_HPI_CFU_PAYLOAD_LENGTH);

					if (!fu_hpi_cfu_send_payload(self, priv, cfu_data, error)) {
						g_set_error(error,
							    FWUPD_ERROR,
							    FWUPD_ERROR_INVALID_DATA,
							    "fu_hpi_cfu_handler_send_payload for "
							    "sequence number:%d",
							    priv->sequence_number);
						return FALSE;
					}

					remaining_byte_count =
					    payload_header_length - FU_HPI_CFU_PAYLOAD_LENGTH;
					fill_from_position =
					    payload_header_length - remaining_byte_count;

					/* store the remaining bytes to untransmitted_data */
					if (!fu_hpi_cfu_untransmitted_data(payload_data,
									   untransmitted_data,
									   payload_header_length,
									   fill_from_position,
									   error)) {
						g_set_error(error,
							    FWUPD_ERROR,
							    FWUPD_ERROR_INVALID_DATA,
							    "fu_hpi_cfu_handler_send_payload to "
							    "set untransmitted_data");
						return FALSE;
					}
				} else {
					/* cfu_data to be sent to device */
					g_byte_array_append(cfu_data,
							    payload_data->data,
							    payload_data->len);

					if (!fu_hpi_cfu_send_payload(self, priv, cfu_data, error)) {
						g_set_error(error,
							    FWUPD_ERROR,
							    FWUPD_ERROR_INVALID_DATA,
							    "fu_hpi_cfu_handler_send_payload for "
							    "sequence number:%d",
							    priv->sequence_number);
						return FALSE;
					}
				}
			}

			if (priv->last_packet_sent) {
				if (untransmitted_data->data != NULL) {
					g_autoptr(GByteArray) cfu_last_packet = g_byte_array_new();

					/* clear and assign the latest untransmitted_data */
					fu_byte_array_set_size(cfu_last_packet,
							       untransmitted_data->len,
							       0x00);
					if (!fu_memcpy_safe(cfu_last_packet->data,
							    untransmitted_data->len,
							    0x0,
							    untransmitted_data->data,
							    untransmitted_data->len,
							    0x0,
							    untransmitted_data->len,
							    error))
						return FALSE;

					g_debug("sending payload last packet");
					if (!fu_hpi_cfu_send_payload(self,
								     priv,
								     cfu_last_packet,
								     error)) {
						g_set_error(error,
							    FWUPD_ERROR,
							    FWUPD_ERROR_INVALID_DATA,
							    "fu_hpi_cfu_handler_send_payload for "
							    "sequence number:%d",
							    priv->sequence_number);
						return FALSE;
					}
				}
			}

			if (!fu_hpi_cfu_handler_check_update_content(self,
								     priv,
								     progress,
								     options,
								     error)) {
				g_set_error(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INVALID_DATA,
					    "failed fu_hpi_cfu_handler_check_update_content for "
					    "sequence_number:%d",
					    priv->sequence_number);
				return FALSE;
			}

			if (priv->state != FU_HPI_CFU_STATE_UPDATE_CONTENT)
				break;
		}
	}

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_update_success(FuHpiCfuDevice *self,
				  FuHpiCfuDevicePrivate *priv,
				  FuProgress *progress,
				  void *options,
				  GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (priv->last_packet_sent) {
		priv->firmware_status = TRUE;
		priv->state = FU_HPI_CFU_STATE_END_OFFER_LIST;
	} else
		priv->state = FU_HPI_CFU_STATE_UPDATE_MORE_OFFERS;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_update_offer_rejected(FuHpiCfuDevice *self,
					 FuHpiCfuDevicePrivate *priv,
					 FuProgress *progress,
					 void *options,
					 GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (priv->last_packet_sent)
		priv->state = FU_HPI_CFU_STATE_END_OFFER_LIST;
	else
		priv->state = FU_HPI_CFU_STATE_UPDATE_OFFER;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_update_more_offers(FuHpiCfuDevice *self,
				      FuHpiCfuDevicePrivate *priv,
				      FuProgress *progress,
				      void *options,
				      GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (priv->last_packet_sent)
		priv->state = FU_HPI_CFU_STATE_END_OFFER_LIST;
	else
		priv->state = FU_HPI_CFU_STATE_UPDATE_OFFER;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_end_offer_list(FuHpiCfuDevice *self,
				  FuHpiCfuDevicePrivate *priv,
				  FuProgress *progress,
				  void *options,
				  GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (!fu_hpi_cfu_send_end_offer_list(self, error)) {
		priv->state = FU_HPI_CFU_STATE_ERROR;
		return FALSE;
	}
	priv->state = FU_HPI_CFU_STATE_END_OFFER_LIST_ACCEPTED;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_end_offer_list_accepted(FuHpiCfuDevice *self,
					   FuHpiCfuDevicePrivate *priv,
					   FuProgress *progress,
					   void *options,
					   GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (!fu_hpi_cfu_end_offer_list_accepted(self, error)) {
		return FALSE;
	}

	priv->state = FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_BY_SENDING_OFFER_LIST_AGAIN;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_update_stop(FuHpiCfuDevice *self,
			       FuHpiCfuDevicePrivate *priv,
			       FuProgress *progress,
			       void *options,
			       GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	priv->exit_state_machine_framework = TRUE;

	fu_progress_step_done(progress); /* send-payload */

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_error(FuHpiCfuDevice *self,
			 FuHpiCfuDevicePrivate *priv,
			 FuProgress *progress,
			 void *options,
			 GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	priv->state = FU_HPI_CFU_STATE_UPDATE_STOP;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_notify_on_ready(FuHpiCfuDevice *self,
				   FuHpiCfuDevicePrivate *priv,
				   FuProgress *progress,
				   void *options,
				   GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	priv->state = FU_HPI_CFU_STATE_WAIT_FOR_READY_NOTIFICATION;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_wait_for_ready_notification(FuHpiCfuDevice *self,
					       FuHpiCfuDevicePrivate *priv,
					       FuProgress *progress,
					       void *options,
					       GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	priv->state = FU_HPI_CFU_STATE_UPDATE_STOP;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_swap_pending_send_offer_list_again(FuHpiCfuDevice *self,
						      FuHpiCfuDevicePrivate *priv,
						      FuProgress *progress,
						      void *options,
						      GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (!fu_hpi_cfu_send_start_offer_list(self, error)) {
		priv->state = FU_HPI_CFU_STATE_UPDATE_VERIFY_ERROR;
		return FALSE;
	}

	priv->state = FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_OFFER_LIST_ACCEPTED;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_swap_pending_offer_list_accepted(FuHpiCfuDevice *self,
						    FuHpiCfuDevicePrivate *priv,
						    FuProgress *progress,
						    void *options,
						    GError **error)
{
	gint32 status = 0;

	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (!fu_hpi_cfu_send_offer_list_accepted(self, &status, error)) {
		priv->state = FU_HPI_CFU_STATE_ERROR;
		return FALSE;
	}

	if (status >= 0)
		priv->state = FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_SEND_OFFER_AGAIN;
	else
		priv->state = FU_HPI_CFU_STATE_UPDATE_VERIFY_ERROR;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_swap_pending_send_offer_again(FuHpiCfuDevice *self,
						 FuHpiCfuDevicePrivate *priv,
						 FuProgress *progress,
						 void *options,
						 GError **error)
{
	FuHpiCfuHandlerOptions *update_options = NULL;

	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	update_options = (FuHpiCfuHandlerOptions *)options;

	if (!fu_hpi_cfu_send_offer_update_command(self, update_options->fw_offer, error)) {
		priv->state = FU_HPI_CFU_STATE_ERROR;
		return FALSE;
	} else
		priv->state = FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_OFFER_ACCEPTED;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_swap_pending_send_offer_list_accepted(FuHpiCfuDevice *self,
							 FuHpiCfuDevicePrivate *priv,
							 FuProgress *progress,
							 void *options,
							 GError **error)
{
	gint32 reason = 0;
	gint32 reply = 0;

	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	/* reply status must be SWAP_PENDING */
	if (!fu_hpi_cfu_firmware_update_offer_accepted(self, priv, &reply, error)) {
		return FALSE;
	}

	g_debug("fu_hpi_cfu_handler_swap_pending_send_offer_list_accepted: reply:%d", reply);

	if (reply == FU_HPI_CFU_FIRMWARE_UPDATE_OFFER_ACCEPT) {
		g_debug("fu_hpi_cfu_handler_swap_pending_send_offer_list_accepted: "
			"expceted a reject with SWAP PENDING");
		priv->state = FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_SEND_UPDATE_END_OFFER_LIST;
	} else {
		if (fu_hpi_cfu_firmware_update_offer_rejected(reply)) {
			g_debug("fu_hpi_cfu_handler_swap_pending_send_offer_list_accepted: "
				"reply: %d,OFFER_REJECTED: Reason:'%s'",
				reply,
				fu_cfu_rr_code_to_string(reason));

			switch (reason) {
			case FU_HPI_CFU_FIRMWARE_OFFER_REJECT_OLD_FW:
				g_debug("fu_hpi_cfu_handler_swap_pending_send_offer_list_accepted: "
					"FU_HPI_CFU_FIRMWARE_OFFER_REJECT_OLD_FW: expcetd a reject "
					"with SWAP "
					"PENDING");
				break;

			case FU_HPI_CFU_FIRMWARE_OFFER_REJECT_INV_COMPONENT:
				g_debug("fu_hpi_cfu_handler_swap_pending_send_offer_list_accepted: "
					"FU_HPI_CFU_FIRMWARE_OFFER_REJECT_INV_COMPONENT: expcetd a "
					"reject with SWAP "
					"PENDING");
				break;

			case FU_HPI_CFU_FIRMWARE_OFFER_REJECT_SWAP_PENDING:
				g_debug("FU_HPI_CFU_FIRMWARE_OFFER_REJECT_SWAP_PENDING: FIRMWARE "
					"UPDATE "
					"COMPLETED.");
				break;

			case FU_HPI_CFU_FIRMWARE_OFFER_REJECT_MISMATCH:
				g_debug("fu_hpi_cfu_handler_swap_pending_send_offer_list_accepted: "
					"FU_HPI_CFU_FIRMWARE_OFFER_REJECT_MISMATCH: expceted a "
					"reject with "
					"SWAP PENDING");
				break;

			case FU_HPI_CFU_FIRMWARE_OFFER_REJECT_BANK:
				g_debug("FU_HPI_CFU_FIRMWARE_OFFER_REJECT_BANK: expceted a reject "
					"with SWAP "
					"PENDING");
				break;

			case FU_HPI_CFU_FIRMWARE_OFFER_REJECT_PLATFORM:
				g_debug("FU_HPI_CFU_FIRMWARE_OFFER_REJECT_PLATFORM: expceted a "
					"reject with "
					"SWAP PENDING");
				break;
			case FU_HPI_CFU_FIRMWARE_OFFER_REJECT_MILESTONE:
				g_debug("FU_HPI_CFU_FIRMWARE_OFFER_REJECT_MILESTONE: expceted a "
					"reject with "
					"SWAP PENDING");
				break;

			case FU_HPI_CFU_FIRMWARE_OFFER_REJECT_INV_PCOL_REV:
				g_debug("FU_HPI_CFU_FIRMWARE_OFFER_REJECT_INV_PCOL_REV: expceted a "
					"reject with "
					"SWAP PENDING");
				priv->state =
				    FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_SEND_UPDATE_END_OFFER_LIST;
				break;

			case FU_HPI_CFU_FIRMWARE_OFFER_REJECT_VARIANT:
				g_debug("FU_HPI_CFU_FIRMWARE_OFFER_REJECT_VARIANT: expceted a "
					"reject with SWAP "
					"PENDING");
				break;

			default:
				g_debug("fu_hpi_cfu_handler_swap_pending_send_offer_list_accepted "
					"expceted a reject with SWAP PENDING");
				break;
			} /* swicth */
		}	  /* rejected */
		priv->state = FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_SEND_UPDATE_END_OFFER_LIST;
	}
	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_send_end_offer_list(FuHpiCfuDevice *self,
				       FuHpiCfuDevicePrivate *priv,
				       FuProgress *progress,
				       void *options,
				       GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (!fu_hpi_cfu_send_end_offer_list(self, error)) {
		return FALSE;
	}

	priv->state = FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_UPDATE_END_OFFER_LIST_ACCEPTED;

	fu_progress_step_done(progress); /* send-payload */

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_send_end_offer_list_accepted(FuHpiCfuDevice *self,
						FuHpiCfuDevicePrivate *priv,
						FuProgress *progress,
						void *options,
						GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	if (!fu_hpi_cfu_end_offer_list_accepted(self, error)) {
		return FALSE;
	}

	priv->state = FU_HPI_CFU_STATE_UPDATE_STOP;

	return TRUE;
}

static gboolean
fu_hpi_cfu_handler_verify_error(FuHpiCfuDevice *self,
				FuHpiCfuDevicePrivate *priv,
				FuProgress *progress,
				void *options,
				GError **error)
{
	g_debug("hpi-cfu-state: %s", fu_hpi_cfu_state_to_string(priv->state));

	priv->state = FU_HPI_CFU_STATE_UPDATE_STOP;

	return TRUE;
}

FuHpiCfuStateMachineFramework hpi_cfu_states[] = {
    {FU_HPI_CFU_STATE_START_ENTIRE_TRANSACTION, fu_hpi_cfu_handler_start_entire_transaction, NULL},
    {FU_HPI_CFU_STATE_START_ENTIRE_TRANSACTION_ACCEPTED,
     fu_hpi_cfu_handler_start_entire_transaction_accepted,
     NULL},
    {FU_HPI_CFU_STATE_START_OFFER_LIST, fu_hpi_cfu_handler_send_start_offer_list, NULL},
    {FU_HPI_CFU_STATE_START_OFFER_LIST_ACCEPTED,
     fu_hpi_cfu_handler_send_start_offer_list_accepted,
     NULL},
    {FU_HPI_CFU_STATE_UPDATE_OFFER, fu_hpi_cfu_handler_send_offer_update_command, &handler_options},
    {FU_HPI_CFU_STATE_UPDATE_OFFER_ACCEPTED, fu_hpi_cfu_handler_send_offer_accepted, NULL},
    {FU_HPI_CFU_STATE_UPDATE_CONTENT, fu_hpi_cfu_handler_send_payload, &handler_options},
    {FU_HPI_CFU_STATE_UPDATE_SUCCESS, fu_hpi_cfu_handler_update_success, NULL},
    {FU_HPI_CFU_STATE_UPDATE_OFFER_REJECTED, fu_hpi_cfu_handler_update_offer_rejected, NULL},
    {FU_HPI_CFU_STATE_UPDATE_MORE_OFFERS, fu_hpi_cfu_handler_update_more_offers, NULL},
    {FU_HPI_CFU_STATE_END_OFFER_LIST, fu_hpi_cfu_handler_end_offer_list, NULL},
    {FU_HPI_CFU_STATE_END_OFFER_LIST_ACCEPTED, fu_hpi_cfu_handler_end_offer_list_accepted, NULL},
    {FU_HPI_CFU_STATE_UPDATE_STOP, fu_hpi_cfu_handler_update_stop, NULL},
    {FU_HPI_CFU_STATE_ERROR, fu_hpi_cfu_handler_error, NULL},
    {FU_HPI_CFU_STATE_CHECK_UPDATE_CONTENT, fu_hpi_cfu_handler_check_update_content, NULL},
    {FU_HPI_CFU_STATE_NOTIFY_ON_READY, fu_hpi_cfu_handler_notify_on_ready, NULL},
    {FU_HPI_CFU_STATE_WAIT_FOR_READY_NOTIFICATION,
     fu_hpi_cfu_handler_wait_for_ready_notification,
     NULL},
    {FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_BY_SENDING_OFFER_LIST_AGAIN,
     fu_hpi_cfu_handler_swap_pending_send_offer_list_again,
     NULL},
    {FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_OFFER_LIST_ACCEPTED,
     fu_hpi_cfu_handler_swap_pending_offer_list_accepted,
     NULL},
    {FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_SEND_OFFER_AGAIN,
     fu_hpi_cfu_handler_swap_pending_send_offer_again,
     &handler_options},
    {FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_OFFER_ACCEPTED,
     fu_hpi_cfu_handler_swap_pending_send_offer_list_accepted,
     NULL},
    {FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_SEND_UPDATE_END_OFFER_LIST,
     fu_hpi_cfu_handler_send_end_offer_list,
     NULL},
    {FU_HPI_CFU_STATE_VERIFY_CHECK_SWAP_PENDING_UPDATE_END_OFFER_LIST_ACCEPTED,
     fu_hpi_cfu_handler_send_end_offer_list_accepted,
     NULL},
    {FU_HPI_CFU_STATE_UPDATE_VERIFY_ERROR, fu_hpi_cfu_handler_verify_error, NULL},
};

static gboolean
fu_hpi_cfu_device_setup(FuDevice *device, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	g_autofree gchar *version = NULL;
	guint32 version_raw;

	/* Header is 4 bytes. */
	gint32 versionTableOffset = 4;

	/* Component ID is 6th byte. */
	gint32 componentIDOffset = 5;

	/* Each component takes up 8 bytes. */
	gint32 componentDataSize = 8;

	/* componentIndex - refers, if having multiple offers.
	If offers count is 3, componentIndex starts from 0. */
	gint32 componentIndex =
	    0; /* Hardcoded to zero, since multiple offers logic is in progress. */

	gsize actual_length = 0;
	guint8 buf[60];
	g_autoptr(GBytes) device_version_response = NULL;

	FuHpiCfuDevice *self = FU_HPI_CFU_DEVICE(device);
	FuHpiCfuDevicePrivate *priv = GET_PRIVATE(self);

	g_return_val_if_fail(FU_HPI_CFU_DEVICE(self), FALSE);
	g_return_val_if_fail(error == NULL || *error == NULL, FALSE);

	/* FuHidDevice->setup */
	if (!FU_DEVICE_CLASS(fu_hpi_cfu_device_parent_class)->setup(device, error))
		return FALSE;

	if (!fu_usb_device_control_transfer(FU_USB_DEVICE(self),
					    FU_USB_DIRECTION_DEVICE_TO_HOST,
					    FU_USB_REQUEST_TYPE_VENDOR,
					    FU_USB_RECIPIENT_DEVICE,
					    GET_REPORT,
					    FEATURE_REPORT_TYPE | FIRMWARE_REPORT_ID,
					    priv->iface_number,
					    buf,
					    sizeof(buf),
					    &actual_length,
					    FU_HPI_CFU_DEVICE_TIMEOUT,
					    NULL,
					    &error_local)) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INVALID_DATA,
			    "failed to do device setup: %s",
			    error_local->message);
		return FALSE;
	}

	if (!fu_memread_uint32_safe(buf + 5, 4, 0x0, &version_raw, G_LITTLE_ENDIAN, error))
		return FALSE;

	device_version_response = g_bytes_new(buf, actual_length);
	fu_dump_bytes(G_LOG_DOMAIN,
		      "fu_hpi_cfu_device_setup: bytes received",
		      device_version_response);

	fu_device_set_version(device,
			      g_strdup_printf("%02x.%02x.%02x.%02x",
					      (version_raw >> 24) & 0xff,
					      (version_raw >> 16) & 0xff,
					      (version_raw >> 8) & 0xff,
					      version_raw & 0xff));

	/* Get Bulk optimization value */
	if (!fu_memcpy_safe((guint8 *)&priv->bulk_acksize,
			    sizeof(priv->bulk_acksize),
			    0x0,
			    (guint8 *)&buf[versionTableOffset + componentIndex * componentDataSize +
					   componentIDOffset],
			    1,
			    0x0,
			    1,
			    error))
		return FALSE;

	g_debug("fu_hpi_cfu_device_setup: bulk_acksize: %d", priv->bulk_acksize);

	/* success */
	return TRUE;
}

static void
fu_hpi_cfu_set_progress(FuDevice *self, FuProgress *progress)
{
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_set_percentage(progress, 0);
	fu_progress_add_flag(progress, FU_PROGRESS_FLAG_GUESSED);
	fu_progress_add_step(progress, FWUPD_STATUS_DECOMPRESSING, 4, "detach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 5, "write");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 86, "attach");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 5, "reload");
}

static gboolean
fu_hpi_cfu_device_write_firmware(FuDevice *device,
				 FuFirmware *firmware,
				 FuProgress *progress,
				 FwupdInstallFlags flags,
				 GError **error)
{
	FuHpiCfuDevice *self = FU_HPI_CFU_DEVICE(device);
	FuHpiCfuDevicePrivate *priv = GET_PRIVATE(self);

	g_autoptr(FuFirmware) fw_offer = NULL;
	g_autoptr(FuFirmware) fw_payload = NULL;
	g_autoptr(GBytes) blob_offer = NULL;
	g_autoptr(GBytes) blob_payload = NULL;
	gsize payload_file_size = 0;

	/* progress */
	fu_progress_set_id(progress, G_STRLOC);
	fu_progress_add_step(progress, FWUPD_STATUS_DECOMPRESSING, 0, "start-entire");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 0, "start-offer");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_BUSY, 0, "send-offer");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_WRITE, 92, "send-payload");
	fu_progress_add_step(progress, FWUPD_STATUS_DEVICE_RESTART, 8, "restart");

	/* get both images */
	fw_offer = fu_archive_firmware_get_image_fnmatch(FU_ARCHIVE_FIRMWARE(firmware),
							 "*.offer.bin",
							 error);
	if (fw_offer == NULL)
		return FALSE;

	fw_payload = fu_archive_firmware_get_image_fnmatch(FU_ARCHIVE_FIRMWARE(firmware),
							   "*.payload.bin",
							   error);
	if (fw_payload == NULL)
		return FALSE;

	priv->state = FU_HPI_CFU_STATE_START_ENTIRE_TRANSACTION;
	blob_payload = fu_firmware_get_bytes(fw_payload, NULL);
	g_bytes_get_data(blob_payload, &payload_file_size);
	priv->curfilepos = 0;
	priv->payload_file_size = payload_file_size;
	handler_options.fw_offer = fw_offer;
	handler_options.fw_payload = fw_payload;

	/* cfu state machine framework */
	while (!priv->exit_state_machine_framework) {
		if (!hpi_cfu_states[priv->state].handler(self,
							 priv,
							 progress,
							 hpi_cfu_states[priv->state].options,
							 error)) {
			g_prefix_error(error, "failed at state: ");
			return FALSE;
		}
	}

	if (priv->firmware_status) {
		/* the device automatically reboots */
		fu_device_add_flag(device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	}

	return TRUE;
}

static void
fu_hpi_cfu_device_init(FuHpiCfuDevice *self)
{
	FuHpiCfuDevicePrivate *priv = GET_PRIVATE(self);

	priv->iface_number = 0x00;
	priv->state = FU_HPI_CFU_STATE_START_ENTIRE_TRANSACTION;

	fu_device_add_protocol(FU_DEVICE(self), "com.microsoft.cfu");
	fu_device_set_version_format(FU_DEVICE(self), FWUPD_VERSION_FORMAT_QUAD);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_REQUIRE_AC);
	fu_device_add_flag(FU_DEVICE(self), FWUPD_DEVICE_FLAG_UNSIGNED_PAYLOAD);
	fu_device_set_firmware_gtype(FU_DEVICE(self), FU_TYPE_ARCHIVE_FIRMWARE);
	fu_device_add_private_flag(FU_DEVICE(self), FU_DEVICE_PRIVATE_FLAG_ADD_INSTANCE_ID_REV);

	/* The HP Dock device reboot takes down the entire hub for ~12 minutes */
	fu_device_set_remove_delay(FU_DEVICE(self), 720 * 1000);
}

static void
fu_hpi_cfu_device_class_init(FuHpiCfuDeviceClass *klass)
{
	FuDeviceClass *device_class = FU_DEVICE_CLASS(klass);

	device_class->write_firmware = fu_hpi_cfu_device_write_firmware;
	device_class->setup = fu_hpi_cfu_device_setup;
	device_class->set_progress = fu_hpi_cfu_set_progress;
}
