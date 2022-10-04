/*
 * Copyright (c) 2020 PHYTEC Messtechnik GmbH
 * Copyright (c) 2021 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_REGISTER(modbus, CONFIG_MODBUS_LOG_LEVEL);

#include <kernel.h>
#include <string.h>
#include <sys/byteorder.h>
#include <modbus_internal.h>
#include <device.h>

#define DT_DRV_COMPAT zephyr_modbus_serial

static void modbus_rx_handler(struct k_work *item)
{
	struct modbus_context *ctx;

	ctx = CONTAINER_OF(item, struct modbus_context, server_work);
	if (ctx == NULL) {
		LOG_ERR("Failed to obtain context pointer?");
		return;
	}

	switch (ctx->mode) {
	case MODBUS_MODE_RTU:
	case MODBUS_MODE_ASCII:
		if (IS_ENABLED(CONFIG_MODBUS_SERIAL)) {
			modbus_serial_rx_disable(ctx);
			ctx->rx_adu_err = modbus_serial_rx_adu(ctx);
		}
		break;
	case MODBUS_MODE_RAW:
		if (IS_ENABLED(CONFIG_MODBUS_RAW_ADU)) {
			ctx->rx_adu_err = modbus_raw_rx_adu(ctx);
		}
		break;
	default:
		LOG_ERR("Unknown MODBUS mode");
		return;
	}

	if (ctx->client == true) {
		k_sem_give(&ctx->client_wait_sem);
	} else if (IS_ENABLED(CONFIG_MODBUS_SERVER)) {
		bool respond = modbus_server_handler(ctx);

		if (respond) {
			modbus_tx_adu(ctx);
		} else {
			LOG_DBG("Server has dropped frame");
		}

		switch (ctx->mode) {
		case MODBUS_MODE_RTU:
		case MODBUS_MODE_ASCII:
			if (IS_ENABLED(CONFIG_MODBUS_SERIAL) &&
			    respond == false) {
				modbus_serial_rx_enable(ctx);
			}
			break;
		default:
			break;
		}
	}
}

void modbus_tx_adu(struct modbus_context *ctx)
{
	switch (ctx->mode) {
	case MODBUS_MODE_RTU:
	case MODBUS_MODE_ASCII:
		if (IS_ENABLED(CONFIG_MODBUS_SERIAL) &&
		    modbus_serial_tx_adu(ctx)) {
			LOG_ERR("Unsupported MODBUS serial mode");
		}
		break;
	case MODBUS_MODE_RAW:
		if (IS_ENABLED(CONFIG_MODBUS_RAW_ADU) &&
		    modbus_raw_tx_adu(ctx)) {
			LOG_ERR("Unsupported MODBUS raw mode");
		}
		break;
	default:
		LOG_ERR("Unknown MODBUS mode");
	}
}

int modbus_tx_wait_rx_adu(struct modbus_context *ctx)
{
	modbus_tx_adu(ctx);

	if (k_sem_take(&ctx->client_wait_sem, K_USEC(ctx->rxwait_to)) != 0) {
		LOG_WRN("Client wait-for-RX timeout");
		return -ETIMEDOUT;
	}

	return ctx->rx_adu_err;
}

static int modbus_init_iface(struct modbus_context *ctx)
{
	if (atomic_test_and_set_bit(&ctx->state, MODBUS_STATE_CONFIGURED)) {
		LOG_ERR("Interface allready used");
		return -EINVAL;
	}

	k_mutex_init(&ctx->iface_lock);
	k_sem_init(&ctx->client_wait_sem, 0, 1);
	k_work_init(&ctx->server_work, modbus_rx_handler);
	return 0;
}

int modbus_init_server(struct modbus_context *ctx, struct modbus_iface_param param)
{
	int rc = 0;

	if (!IS_ENABLED(CONFIG_MODBUS_SERVER)) {
		LOG_ERR("Modbus server support is not enabled");
		rc = -ENOTSUP;
		goto init_server_error;
	}

	if (param.server.user_cb == NULL) {
		LOG_ERR("User callbacks should be available");
		rc = -EINVAL;
		goto init_server_error;
	}

	rc = modbus_init_iface(ctx);
	if (rc != 0) {
		LOG_ERR("Modbus server failed to set iface");
		rc = -EINVAL;
		goto init_server_error;
	}

	switch (param.mode) {
	case MODBUS_MODE_RTU:
	case MODBUS_MODE_ASCII:
		if (IS_ENABLED(CONFIG_MODBUS_SERIAL) &&
		    modbus_serial_init(ctx, param) != 0) {
			LOG_ERR("Failed to init MODBUS over serial line");
			rc = -EINVAL;
			goto init_server_error;
		}
		break;
	case MODBUS_MODE_RAW:
		if (IS_ENABLED(CONFIG_MODBUS_RAW_ADU) &&
		    modbus_raw_init(ctx, param) != 0) {
			LOG_ERR("Failed to init MODBUS raw ADU support");
			rc = -EINVAL;
			goto init_server_error;
		}
		break;
	default:
		LOG_ERR("Unknown MODBUS mode");
		rc = -ENOTSUP;
		goto init_server_error;
	}

	ctx->client = false;
	ctx->unit_id = param.server.unit_id;
	ctx->mbs_user_cb = param.server.user_cb;
	if (IS_ENABLED(CONFIG_MODBUS_FC08_DIAGNOSTIC)) {
		modbus_reset_stats(ctx);
	}

	LOG_DBG("Modbus interface %d initialized", ctx->unit_id);

	return 0;

init_server_error:
	if (ctx != NULL) {
		atomic_clear_bit(&ctx->state, MODBUS_STATE_CONFIGURED);
	}

	return rc;
}

int modbus_init_client(struct modbus_context *ctx, struct modbus_iface_param param)
{
	int rc = 0;

	if (!IS_ENABLED(CONFIG_MODBUS_CLIENT)) {
		LOG_ERR("Modbus client support is not enabled");
		rc = -ENOTSUP;
		goto init_client_error;
	}

	rc = modbus_init_iface(ctx);
	if (rc != 0) {
		rc = -EINVAL;
		goto init_client_error;
	}

	switch (param.mode) {
	case MODBUS_MODE_RTU:
	case MODBUS_MODE_ASCII:
		if (IS_ENABLED(CONFIG_MODBUS_SERIAL) &&
		    modbus_serial_init(ctx, param) != 0) {
			LOG_ERR("Failed to init MODBUS over serial line");
			rc = -EINVAL;
			goto init_client_error;
		}
		break;
	case MODBUS_MODE_RAW:
		if (IS_ENABLED(CONFIG_MODBUS_RAW_ADU) &&
		    modbus_raw_init(ctx, param) != 0) {
			LOG_ERR("Failed to init MODBUS raw ADU support");
			rc = -EINVAL;
			goto init_client_error;
		}
		break;
	default:
		LOG_ERR("Unknown MODBUS mode");
		rc = -ENOTSUP;
		goto init_client_error;
	}

	ctx->client = true;
	ctx->unit_id = 0;
	ctx->mbs_user_cb = NULL;
	ctx->rxwait_to = param.rx_timeout;

	return 0;

init_client_error:
	if (ctx != NULL) {
		atomic_clear_bit(&ctx->state, MODBUS_STATE_CONFIGURED);
	}

	return rc;
}

int modbus_disable(struct modbus_context *ctx)
{
	if (ctx == NULL) {
		LOG_ERR("Interface not initialized");
		return -EINVAL;
	}

	switch (ctx->mode) {
	case MODBUS_MODE_RTU:
	case MODBUS_MODE_ASCII:
		if (IS_ENABLED(CONFIG_MODBUS_SERIAL)) {
			modbus_serial_disable(ctx);
		}
		break;
	case MODBUS_MODE_RAW:
		break;
	default:
		LOG_ERR("Unknown MODBUS mode");
	}

	ctx->rxwait_to = 0;
	ctx->unit_id = 0;
	ctx->mode = MODBUS_MODE_RTU;
	ctx->mbs_user_cb = NULL;
	atomic_clear_bit(&ctx->state, MODBUS_STATE_CONFIGURED);

	LOG_INF("Modbus interface disabled");

	return 0;
}
