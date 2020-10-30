// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ACLK_TX_MSGS_H
#define ACLK_TX_MSGS_H

#include <json-c/json.h>
#include "libnetdata/libnetdata.h"
#include "mqtt_wss_client.h"

void aclk_send_info_metadata(mqtt_wss_client client, int metadata_submitted);

#endif
