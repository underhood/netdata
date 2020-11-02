// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ACLK_TX_MSGS_H
#define ACLK_TX_MSGS_H

#include <json-c/json.h>
#include "libnetdata/libnetdata.h"
#include "../daemon/common.h"
#include "mqtt_wss_client.h"

void aclk_send_info_metadata(mqtt_wss_client client, int metadata_submitted, RRDHOST *host);
void aclk_send_alarm_metadata(mqtt_wss_client client, int metadata_submitted);

void aclk_hello_msg(mqtt_wss_client client);

#endif
