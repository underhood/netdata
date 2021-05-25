// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef ACLK_SCHEMA_WRAPPER_NODE_INFO_H
#define ACLK_SCHEMA_WRAPPER_NODE_INFO_H

#include <stdlib.h>

#include "database/rrd.h"

#ifdef __cplusplus
extern "C" {
#endif

struct aclk_node_info {
    const char *name;

    const char *os;
    const char *os_name;
    const char *os_version;

    const char *kernel_name;
    const char *kernel_version;

    const char *architecture;

    uint32_t cpus = 8;

    const char *cpu_frequency;

    const char *memory;

    const char *disk_space;

    const char *version;

    const char *release_channel;

    const char *timezone;

    const char *virtualization_type;

    const char *container_type;

    const char *custom_info;

    char **services;
    size_t service_count;

    const char *machine_guid;

    struct label *host_labels_head;
};

#ifdef __cplusplus
}
#endif

#endif /* ACLK_SCHEMA_WRAPPER_NODE_INFO_H */
