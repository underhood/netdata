
#include "aclk_rx_msgs.h"

#include "aclk_common.h"
#include "aclk_stats.h"
#include "aclk_query.h"

static inline int aclk_extract_v2_payload(char *payload, struct aclk_request *req)
{
    char* ptr = strstr(payload, ACLK_V2_PAYLOAD_SEPARATOR);
    if(!ptr)
        return 1;
    ptr += strlen(ACLK_V2_PAYLOAD_SEPARATOR);
    req->payload = strdupz(ptr);
    return 0;
}

static inline int aclk_v2_payload_preverify(const char *payload)
{
    // we do better parsing when we handle the command in query thread
    // this is only to check roughly if all looks OK
    if(strncmp(payload, ACLK_CLOUD_REQ_V2_PREFIX, strlen(ACLK_CLOUD_REQ_V2_PREFIX))) {
        error("Only accepting GET requests from CLOUD.");
        return 1;
    }

    if(!strstr(payload, " HTTP/1.1\x0D\x0A")) {
        error("Doesn't look like HTTP GET request.");
        return 1;
    }

    return 0;
}

#define HTTP_CHECK_AGENT_INITIALIZED() ACLK_SHARED_STATE_LOCK;\
    if (unlikely(aclk_shared_state.agent_state == AGENT_INITIALIZING)) {\
        debug(D_ACLK, "Ignoring \"http\" cloud request; agent not in stable state");\
        ACLK_SHARED_STATE_UNLOCK;\
        return 1;\
    }\
    ACLK_SHARED_STATE_UNLOCK;

/*
 * Parse the incoming payload and queue a command if valid
 */
static int aclk_handle_cloud_request_v1(struct aclk_request *cloud_to_agent, char *raw_payload)
{
    UNUSED(raw_payload);
    HTTP_CHECK_AGENT_INITIALIZED();

    if (unlikely(cloud_to_agent->version != 1)) {
        error("Received \"http\" message from Cloud with version %d, but ACLK version %d is used", cloud_to_agent->version, aclk_shared_state.version_neg);
        return 1;
    }

    if (unlikely(!cloud_to_agent->payload)) {
        error("payload missing");
        return 1;
    }
    
    if (unlikely(!cloud_to_agent->callback_topic)) {
        error("callback_topic missing");
        return 1;
    }
    
    if (unlikely(!cloud_to_agent->msg_id)) {
        error("msg_id missing");
        return 1;
    }

    if (unlikely(aclk_queue_query(cloud_to_agent->callback_topic, NULL, cloud_to_agent->msg_id, cloud_to_agent->payload, 0, 0, ACLK_CMD_CLOUD)))
        debug(D_ACLK, "ACLK failed to queue incoming \"http\" message");

    return 0;
}

static int aclk_handle_cloud_request_v2(struct aclk_request *cloud_to_agent, char *raw_payload)
{
    HTTP_CHECK_AGENT_INITIALIZED();

    if(cloud_to_agent->version < ACLK_V_COMPRESSION) {
        error("This handler cannot reply to request with version older than %d, received %d.", ACLK_V_COMPRESSION, cloud_to_agent->version);
        return 1;
    }

    if(unlikely(aclk_extract_v2_payload(raw_payload, cloud_to_agent))) {
        error("Error extracting payload expected after the JSON dictionary.");
        return 1;
    }

    if(unlikely(aclk_v2_payload_preverify(cloud_to_agent->payload)))
        return 1;

    if (unlikely(!cloud_to_agent->callback_topic)) {
        error("Missing callback_topic");
        return 1;
    }

    if (unlikely(!cloud_to_agent->msg_id)) {
        error("Missing msg_id");
        return 1;
    }

    if (unlikely(aclk_queue_query(cloud_to_agent->callback_topic, NULL, cloud_to_agent->msg_id, cloud_to_agent->payload, 0, 0, ACLK_CMD_CLOUD_2)))
        debug(D_ACLK, "ACLK failed to queue incoming \"http\" message");

    UNUSED(cloud_to_agent);
    return 0;
}

// This handles `version` message from cloud used to negotiate
// protocol version we will use
static int aclk_handle_version_response(struct aclk_request *cloud_to_agent, char *raw_payload)
{
    UNUSED(raw_payload);
    int version = -1;

    if(unlikely(cloud_to_agent->version != ACLK_VERSION_MIN)) {
        error("Unsuported version of \"version\" message from cloud. Expected %d, Got %d", ACLK_VERSION_MIN, cloud_to_agent->version);
        return 1;
    }
    if(unlikely(!cloud_to_agent->min_version)) {
        error("Min version missing or 0");
        return 1;
    }
    if(unlikely(!cloud_to_agent->max_version)) {
        error("Max version missing or 0");
        return 1;
    }
    if(unlikely(cloud_to_agent->max_version < cloud_to_agent->max_version)) {
        error("Max version (%d) must be >= than min version (%d)", cloud_to_agent->max_version, cloud_to_agent->min_version);
        return 1;
    }

    if(unlikely(cloud_to_agent->min_version > ACLK_VERSION_MAX)) {
        error("Agent too old for this cloud. Minimum version required by cloud %d. Maximum version supported by this agent %d.", cloud_to_agent->min_version, ACLK_VERSION_MAX);
        aclk_kill_link = 1;
        aclk_disable_runtime = 1;
        return 1;
    }
    if(unlikely(cloud_to_agent->max_version < ACLK_VERSION_MIN)) {
        error("Cloud version is too old for this agent. Maximum version supported by cloud %d. Minimum (oldest) version supported by this agent %d.", cloud_to_agent->max_version, ACLK_VERSION_MIN);
        aclk_kill_link = 1;
        return 1;
    }

    version = MIN(cloud_to_agent->max_version, ACLK_VERSION_MAX);

    ACLK_SHARED_STATE_LOCK;
    if (unlikely(now_monotonic_usec() > aclk_shared_state.version_neg_wait_till)) {
        error("The \"version\" message came too late ignoring.");
        goto err_cleanup;
    }
    if (unlikely(aclk_shared_state.version_neg)) {
        error("Version has already been set to %d", aclk_shared_state.version_neg);
        goto err_cleanup;
    }
    aclk_shared_state.version_neg = version;
    ACLK_SHARED_STATE_UNLOCK;

    info("Choosing version %d of ACLK", version);

    aclk_set_rx_handlers(version);

    return 0;

err_cleanup:
    ACLK_SHARED_STATE_UNLOCK;
    return 1;
}

typedef struct aclk_incoming_msg_type{
    char *name;
    int(*fnc)(struct aclk_request *, char *);
}aclk_incoming_msg_type;

aclk_incoming_msg_type aclk_incoming_msg_types_v1[] = {
    { .name = "http",    .fnc = aclk_handle_cloud_request_v1 },
    { .name = "version", .fnc = aclk_handle_version_response },
    { .name = NULL,      .fnc = NULL                         }
};

aclk_incoming_msg_type aclk_incoming_msg_types_compression[] = {
    { .name = "http",    .fnc = aclk_handle_cloud_request_v2 },
    { .name = "version", .fnc = aclk_handle_version_response },
    { .name = NULL,      .fnc = NULL                         }
};

struct aclk_incoming_msg_type *aclk_incoming_msg_types = aclk_incoming_msg_types_v1;

void aclk_set_rx_handlers(int version)
{
    if(version >= ACLK_V_COMPRESSION) {
        aclk_incoming_msg_types = aclk_incoming_msg_types_compression;
        return;
    }

    aclk_incoming_msg_types = aclk_incoming_msg_types_v1;
}

int aclk_handle_cloud_message(char *payload)
{
    struct aclk_request cloud_to_agent;
    memset(&cloud_to_agent, 0, sizeof(struct aclk_request));

    if (aclk_stats_enabled) {
        ACLK_STATS_LOCK;
        aclk_metrics_per_sample.cloud_req_recvd++;
        ACLK_STATS_UNLOCK;
    }

    if (unlikely(!payload)) {
        debug(D_ACLK, "ACLK incoming message is empty");
        goto err_cleanup;
    }

    debug(D_ACLK, "ACLK incoming message (%s)", payload);

    int rc = json_parse(payload, &cloud_to_agent, cloud_to_agent_parse);

    if (unlikely(rc != JSON_OK)) {
        error("Malformed json request (%s)", payload);
        goto err_cleanup;
    }

    if (!cloud_to_agent.type_id) {
        error("Cloud message is missing compulsory key \"type\"");
        goto err_cleanup;
    }

    for (int i = 0; aclk_incoming_msg_types[i].name; i++) {
        if (strcmp(cloud_to_agent.type_id, aclk_incoming_msg_types[i].name) == 0) {
            if (likely(!aclk_incoming_msg_types[i].fnc(&cloud_to_agent, payload))) {
                // NEVER CONTINUE THIS LOOP AFTER CALLING FUNCTION!!!
                // msg handlers (namely aclk_handle_version_responce)
                // can freely change what aclk_incoming_msg_types points to
                // so either exit or restart this for loop
                freez(cloud_to_agent.type_id);
                return 0;
            }
            goto err_cleanup;
        }
    }

    error("Unknown message type from Cloud \"%s\"", cloud_to_agent.type_id);

err_cleanup:
    if (cloud_to_agent.payload)
        freez(cloud_to_agent.payload);
    if (cloud_to_agent.type_id)
        freez(cloud_to_agent.type_id);
    if (cloud_to_agent.msg_id)
        freez(cloud_to_agent.msg_id);
    if (cloud_to_agent.callback_topic)
        freez(cloud_to_agent.callback_topic);

    if (aclk_stats_enabled) {
        ACLK_STATS_LOCK;
        aclk_metrics_per_sample.cloud_req_err++;
        ACLK_STATS_UNLOCK;
    }

    return 1;
}
