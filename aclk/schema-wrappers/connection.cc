// SPDX-License-Identifier: GPL-3.0-or-later

#include "../aclk-schemas/proto/agent/v1/connection.pb.h"
#include "connection.h"

#include <stdlib.h>

char *generate_update_agent_connection(size_t *len, const update_agent_connection_t *data)
{
    agent::v1::UpdateAgentConnection connupd;

    connupd.set_claim_id(data->claim_id);
    connupd.set_reachable(data->reachable);
    connupd.set_session_id(data->session_id);

    connupd.set_update_source((data->lwt) ? agent::v1::CONNECTION_UPDATE_SOURCE_LWT : agent::v1::CONNECTION_UPDATE_SOURCE_AGENT);

    *len = connupd.ByteSizeLong();
    char *msg = (char*)malloc(*len);
    if (msg)
        connupd.SerializeToArray(msg, *len);

    return msg;
}
