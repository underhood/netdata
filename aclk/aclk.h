// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ACLK_H
#define ACLK_H

#include "../daemon/common.h"

// minimum and maximum supported version of ACLK
// in this version of agent
#define ACLK_VERSION_MIN 2
#define ACLK_VERSION_MAX 2

// Version negotiation messages have they own versioning
// this is also used for LWT message as we set that up
// before version negotiation
#define ACLK_VERSION_NEG_VERSION 1

// Maximum time to wait for version negotiation before aborting
// and defaulting to oldest supported version
#define VERSION_NEG_TIMEOUT 3

#if ACLK_VERSION_MIN > ACLK_VERSION_MAX
#error "ACLK_VERSION_MAX must be >= than ACLK_VERSION_MIN"
#endif

// Define ACLK Feature Version Boundaries Here
#define ACLK_V_COMPRESSION 2

// TODO get rid of this shit
extern int aclk_disable_runtime;
extern int aclk_disable_single_updates;
extern int aclk_kill_link;
extern int aclk_connected;

extern usec_t aclk_session_us;         // Used by the mqtt layer
extern time_t aclk_session_sec;        // Used by the mqtt layer

void *aclk_main(void *ptr);
void aclk_single_update_disable();
void aclk_single_update_enable();

#define NETDATA_ACLK_HOOK                                                                                              \
    { .name = "ACLK_Main",                                                                                             \
      .config_section = NULL,                                                                                          \
      .config_name = NULL,                                                                                             \
      .enabled = 1,                                                                                                    \
      .thread = NULL,                                                                                                  \
      .init_routine = NULL,                                                                                            \
      .start_routine = aclk_main },

extern netdata_mutex_t aclk_shared_state_mutex;
#define ACLK_SHARED_STATE_LOCK netdata_mutex_lock(&aclk_shared_state_mutex)
#define ACLK_SHARED_STATE_UNLOCK netdata_mutex_unlock(&aclk_shared_state_mutex)

typedef enum aclk_cmd {
    ACLK_CMD_CLOUD,
    ACLK_CMD_ONCONNECT,
    ACLK_CMD_INFO,
    ACLK_CMD_CHART,
    ACLK_CMD_CHARTDEL,
    ACLK_CMD_ALARM,
    ACLK_CMD_CLOUD_QUERY_2
} ACLK_CMD;

typedef enum aclk_agent_state {
    AGENT_INITIALIZING,
    AGENT_STABLE
} ACLK_AGENT_STATE;
extern struct aclk_shared_state {
    ACLK_AGENT_STATE agent_state;
    time_t last_popcorn_interrupt;

    // read only while ACLK connected
    // protect by lock otherwise
    int version_neg;
    usec_t version_neg_wait_till;
} aclk_shared_state;

typedef enum aclk_proxy_type {
    PROXY_TYPE_UNKNOWN = 0,
    PROXY_TYPE_SOCKS5,
    PROXY_TYPE_HTTP,
    PROXY_DISABLED,
    PROXY_NOT_SET,
} ACLK_PROXY_TYPE;

// TODO
const char *aclk_get_proxy(ACLK_PROXY_TYPE *type);

void aclk_dummy();

#define aclk_wss_set_proxy(...) aclk_dummy()
#define aclk_alarm_reload(...) aclk_dummy()
#define aclk_update_alarm(...) aclk_dummy()
#define aclk_update_chart(...) aclk_dummy()
#define aclk_del_collector(...) aclk_dummy()
#define aclk_add_collector(...) aclk_dummy()

#define ACLK_CHART_TOPIC "outbound/meta"
#define ACLK_ALARMS_TOPIC "outbound/alarms"
#define ACLK_METADATA_TOPIC "outbound/meta"
#define ACLK_COMMAND_TOPIC "inbound/cmd"
#define ACLK_TOPIC_STRUCTURE "/agent/%s"

#endif /* ACLK_H */
