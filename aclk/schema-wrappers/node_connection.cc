// SPDX-License-Identifier: GPL-3.0-or-later

#include "../aclk-schemas/proto/nodeinstance/connection/v1/connection.pb.h"
#include "node_connection.h"

#include <stdlib.h>

char *generate_node_instance_connection(size_t *len, const node_instance_connection_t *data) {
    nodeinstance::v1::UpdateNodeInstanceConnection msg;

    msg.set_claim_id(data->claim_id);
    msg.set_node_id(data->node_id);

    msg.set_liveness(data->live);
    msg.set_queryable(data->queriable);

    msg.set_session_id(data->session_id);


    *len = msg.ByteSizeLong();
    char *bin = (char*)malloc(*len);
    if (bin)
        msg.SerializeToArray(bin, *len);

    return bin;
}