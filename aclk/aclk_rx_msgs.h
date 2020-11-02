

// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef ACLK_RX_MSGS_H
#define ACLK_RX_MSGS_H

#include "../daemon/common.h"
#include "libnetdata/libnetdata.h"

// TODO this should not be here
struct aclk_request {
    char *type_id;
    char *msg_id;
    char *callback_topic;
    char *payload;
    int version;
    int min_version;
    int max_version;
};

int aclk_handle_cloud_message(char *payload);
void aclk_set_rx_handlers(int version);

#endif /* ACLK_RX_MSGS_H */
