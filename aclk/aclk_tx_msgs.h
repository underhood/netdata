// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ACLK_TX_MSGS_H
#define ACLK_TX_MSGS_H

#include <json-c/json.h>
#include "libnetdata/libnetdata.h"
#include "../daemon/common.h"
#include "aclk_util.h"

void aclk_send_info_metadata(transport_client client, int metadata_submitted, RRDHOST *host);
void aclk_send_alarm_metadata(transport_client client, int metadata_submitted);

void aclk_hello_msg(transport_client client);

void aclk_http_msg_v2(transport_client client, const char *topic, const char *msg_id, usec_t t_exec, usec_t created, int http_code, const char *payload, size_t payload_len);

void aclk_chart_msg(transport_client client, RRDHOST *host, const char *chart);

void aclk_alarm_state_msg(transport_client client, json_object *msg);

json_object *aclk_generate_disconnect(const char *message);
int aclk_send_app_layer_disconnect(transport_client client, const char *message);

#endif
