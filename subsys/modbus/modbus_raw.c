/*
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_REGISTER(modbus_raw, CONFIG_MODBUS_LOG_LEVEL);

#include <kernel.h>
#include <sys/byteorder.h>
#include <modbus_internal.h>

#define MODBUS_ADU_LENGTH_DEVIATION	2
#define MODBUS_RAW_MIN_MSG_SIZE		(MODBUS_RTU_MIN_MSG_SIZE - 2)
#define MODBUS_RAW_BUFFER_SIZE		(CONFIG_MODBUS_BUFFER_SIZE - 2)

int modbus_raw_rx_adu(struct modbus_context *ctx)
{
	if (ctx->rx_adu.length < MODBUS_RAW_MIN_MSG_SIZE ||
	    ctx->rx_adu.length > MODBUS_RAW_BUFFER_SIZE) {
		LOG_WRN("Frame length error");
		return -EMSGSIZE;
	}

	if (ctx->rx_adu.proto_id != MODBUS_ADU_PROTO_ID) {
		LOG_ERR("MODBUS protocol not supported");
		return -ENOTSUP;
	}

	return 0;
}

int modbus_raw_tx_adu(struct modbus_context *ctx)
{

	if (ctx->mode != MODBUS_MODE_RAW) {
		return -ENOTSUP;
	}

	ctx->raw_tx_cb(ctx, &ctx->tx_adu);

	return 0;
}

int modbus_raw_submit_rx(struct modbus_context *ctx, const struct modbus_adu *adu)
{

	if (ctx == NULL) {
		LOG_ERR("Interface not available");
		return -ENODEV;
	}

	if (ctx->mode != MODBUS_MODE_RAW) {
		LOG_ERR("Interface not in RAW mode");
		return -ENOTSUP;
	}

	ctx->rx_adu.trans_id = adu->trans_id;
	ctx->rx_adu.proto_id = adu->proto_id;
	ctx->rx_adu.length = adu->length;
	ctx->rx_adu.unit_id = adu->unit_id;
	ctx->rx_adu.fc = adu->fc;
	memcpy(ctx->rx_adu.data, adu->data,
	       MIN(adu->length, sizeof(ctx->rx_adu.data)));
	k_work_submit(&ctx->server_work);

	return 0;
}

void modbus_raw_put_header(const struct modbus_adu *adu, uint8_t *header)
{
	uint16_t length = MIN(adu->length, CONFIG_MODBUS_BUFFER_SIZE);

	sys_put_be16(adu->trans_id, &header[0]);
	sys_put_be16(adu->proto_id, &header[2]);
	sys_put_be16(length + MODBUS_ADU_LENGTH_DEVIATION, &header[4]);
	header[6] = adu->unit_id;
	header[7] = adu->fc;
}

void modbus_raw_get_header(struct modbus_adu *adu, const uint8_t *header)
{
	adu->trans_id = sys_get_be16(&header[0]);
	adu->proto_id = sys_get_be16(&header[2]);
	adu->length = MIN(sys_get_be16(&header[4]), CONFIG_MODBUS_BUFFER_SIZE);
	adu->unit_id = header[6];
	adu->fc = header[7];

	if (adu->length >= MODBUS_ADU_LENGTH_DEVIATION) {
		adu->length -= MODBUS_ADU_LENGTH_DEVIATION;
	}
}

static void modbus_set_exception(struct modbus_adu *adu,
				 const uint8_t excep_code)
{
	const uint8_t excep_bit = BIT(7);

	adu->fc |= excep_bit;
	adu->data[0] = excep_code;
	adu->length = 1;
}

void modbus_raw_set_server_failure(struct modbus_adu *adu)
{
	const uint8_t excep_bit = BIT(7);

	adu->fc |= excep_bit;
	adu->data[0] = MODBUS_EXC_SERVER_DEVICE_FAILURE;
	adu->length = 1;
}

int modbus_raw_backend_txn(struct modbus_context *ctx, struct modbus_adu *adu)
{
	int err;

	if (ctx == NULL) {
		LOG_ERR("Modbus context not available");
		modbus_set_exception(adu, MODBUS_EXC_GW_PATH_UNAVAILABLE);
		return -ENODEV;
	}

	/*
	 * This is currently only possible over serial line
	 * since no other medium is directly supported.
	 */
	if (ctx->client == false ||
	    (ctx->mode != MODBUS_MODE_RTU && ctx->mode != MODBUS_MODE_ASCII)) {
		LOG_ERR("Modbus context has wrong configuration");
		modbus_set_exception(adu, MODBUS_EXC_GW_PATH_UNAVAILABLE);
		return -ENOTSUP;
	}

	memcpy(&ctx->tx_adu, adu, sizeof(struct modbus_adu));
	err = modbus_tx_wait_rx_adu(ctx);

	if (err == 0) {
		memcpy(adu, &ctx->rx_adu, sizeof(struct modbus_adu));
	} else {
		modbus_set_exception(adu, MODBUS_EXC_GW_TARGET_FAILED_TO_RESP);
	}

	return err;
}

int modbus_raw_init(struct modbus_context *ctx,
		    struct modbus_iface_param param)
{
	if (ctx->mode != MODBUS_MODE_RAW) {
		return -ENOTSUP;
	}

	ctx->raw_tx_cb = param.raw_tx_cb;

	return 0;
}

void modbus_raw_disable(struct modbus_context *ctx)
{
}
