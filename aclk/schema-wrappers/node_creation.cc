// SPDX-License-Identifier: GPL-3.0-or-later

#include "../aclk-schemas/proto/nodeinstance/create/v1/creation.pb.h"
#include "node_creation.h"

#include <stdlib.h>

char *generate_node_instance_creation(size_t *len, const node_instance_creation_t *data)
{
    nodeinstance::create::v1::CreateNodeInstance msg;

    msg.set_claim_id(data->claim_id);
    msg.set_machine_guid(data->machine_guid);
    msg.set_hostname(data->hostname);
    msg.set_hops(data->hops);

    *len = msg.ByteSizeLong();
    char *bin = (char*)malloc(*len);
    if (bin)
        msg.SerializeToArray(bin, *len);

    return bin;
}