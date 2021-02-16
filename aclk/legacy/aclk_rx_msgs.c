
#include "aclk_rx_msgs.h"

#include "aclk_common.h"
#include "aclk_stats.h"
#include "aclk_query.h"

#ifndef UUID_STR_LEN
#define UUID_STR_LEN 37
#endif

static inline int aclk_extract_v2_data(char *payload, char **data)
{
    char* ptr = strstr(payload, ACLK_V2_PAYLOAD_SEPARATOR);
    if(!ptr)
        return 1;
    ptr += strlen(ACLK_V2_PAYLOAD_SEPARATOR);
    *data = strdupz(ptr);
    return 0;
}

#define ACLK_GET_REQ "GET "
#define ACLK_CHILD_REQ "/host/"
#define ACLK_CLOUD_REQ_V2_PREFIX "/api/v1/"
#define STRNCMP_CONSTANT_PREFIX(str, const_pref) strncmp(str, const_pref, strlen(const_pref))
static inline int aclk_v2_payload_get_query(struct aclk_cloud_req_v2 *cloud_req, struct aclk_request *req)
{
    const char *start, *end, *ptr, *query_type;
    char uuid_str[UUID_STR_LEN];
    uuid_t uuid;

    errno = 0;

    if(STRNCMP_CONSTANT_PREFIX(cloud_req->data, ACLK_GET_REQ)) {
        error("Only accepting GET HTTP requests from CLOUD");
        return 1;
    }
    start = ptr = cloud_req->data + strlen(ACLK_GET_REQ);

    if(!STRNCMP_CONSTANT_PREFIX(ptr, ACLK_CHILD_REQ)) {
        ptr += strlen(ACLK_CHILD_REQ);
        if(strlen(ptr) < UUID_STR_LEN) {
            error("the child id in URL too short \"%s\"", start);
            return 1;
        }

        strncpyz(uuid_str, ptr, UUID_STR_LEN - 1);

        for(int i = 0; i < UUID_STR_LEN && uuid_str[i]; i++)
            uuid_str[i] = tolower(uuid_str[i]);

        if(ptr[0] && uuid_parse(uuid_str, uuid)) {
            error("Got Child query (/host/XXX/...) host id \"%s\" doesn't look like valid GUID", uuid_str);
            return 1;
        }
        ptr += UUID_STR_LEN - 1;

        cloud_req->host = rrdhost_find_by_guid(uuid_str, 0);
        if(!cloud_req->host) {
            error("Cannot find host with GUID \"%s\"", uuid_str);
            return 1;
        }
    }

    if(STRNCMP_CONSTANT_PREFIX(ptr, ACLK_CLOUD_REQ_V2_PREFIX)) {
        error("Only accepting requests that start with \"%s\" from CLOUD.", ACLK_CLOUD_REQ_V2_PREFIX);
        return 1;
    }
    ptr += strlen(ACLK_CLOUD_REQ_V2_PREFIX);
    query_type = ptr;

    if(!(end = strstr(ptr, " HTTP/1.1\x0D\x0A"))) {
        errno = 0;
        error("Doesn't look like HTTP GET request.");
        return 1;
    }

    if(!(ptr = strchr(ptr, '?')) || ptr > end) 
        ptr = end;
    cloud_req->query_endpoint = mallocz((ptr - query_type) + 1);
    strncpyz(cloud_req->query_endpoint, query_type, ptr - query_type);

    req->payload = mallocz((end - start) + 1);
    strncpyz(req->payload, start, end - start);

    return 0;
}

#define HTTP_CHECK_AGENT_INITIALIZED() rrdhost_aclk_state_lock(localhost);\
    if (unlikely(localhost->aclk_state.state == ACLK_HOST_INITIALIZING)) {\
        debug(D_ACLK, "Ignoring \"http\" cloud request; agent not in stable state");\
        rrdhost_aclk_state_unlock(localhost);\
        return 1;\
    }\
    rrdhost_aclk_state_unlock(localhost);

static int aclk_handle_cloud_request_v2(struct aclk_request *cloud_to_agent, char *raw_payload)
{
    HTTP_CHECK_AGENT_INITIALIZED();

    struct aclk_cloud_req_v2 *cloud_req;
    char *data;
    int stat_idx;

    errno = 0;
    if (cloud_to_agent->version < ACLK_V_COMPRESSION) {
        error(
            "This handler cannot reply to request with version older than %d, received %d.",
            ACLK_V_COMPRESSION,
            cloud_to_agent->version);
        return 1;
    }

    if (unlikely(aclk_extract_v2_data(raw_payload, &data))) {
        error("Error extracting payload expected after the JSON dictionary.");
        return 1;
    }

    cloud_req = mallocz(sizeof(struct aclk_cloud_req_v2));
    cloud_req->data = data;
    cloud_req->host = localhost;

    if (unlikely(aclk_v2_payload_get_query(cloud_req, cloud_to_agent))) {
        error("Could not extract payload from query");
        goto cleanup;
    }

    if (unlikely(!cloud_to_agent->callback_topic)) {
        error("Missing callback_topic");
        goto cleanup;
    }

    if (unlikely(!cloud_to_agent->msg_id)) {
        error("Missing msg_id");
        goto cleanup;
    }

    // we do this here due to cloud_req being taken over by query thread
    // which if crazy quick can free it after aclk_queue_query
    stat_idx = aclk_cloud_req_type_to_idx(cloud_req->query_endpoint);

    // aclk_queue_query takes ownership of data pointer
    if (unlikely(aclk_queue_query(
            cloud_to_agent->callback_topic, cloud_req, cloud_to_agent->msg_id, cloud_to_agent->payload, 0, 0,
            ACLK_CMD_CLOUD_QUERY_2))) {
        error("ACLK failed to queue incoming \"http\" v2 message");
        goto cleanup;
    }

    if (aclk_stats_enabled) {
        ACLK_STATS_LOCK;
        aclk_metrics_per_sample.cloud_req_v2++;
        aclk_metrics_per_sample.cloud_req_ok++;
        aclk_metrics_per_sample.cloud_req_by_type[stat_idx]++;
        ACLK_STATS_UNLOCK;
    }

    return 0;
cleanup:
    freez(cloud_req->query_endpoint);
    freez(cloud_req->data);
    freez(cloud_req);
    return 1;
}

typedef struct aclk_incoming_msg_type{
    char *name;
    int(*fnc)(struct aclk_request *, char *);
}aclk_incoming_msg_type;

aclk_incoming_msg_type aclk_incoming_msg_types_compression[] = {
    { .name = "http",    .fnc = aclk_handle_cloud_request_v2 },
    { .name = NULL,      .fnc = NULL                         }
};

struct aclk_incoming_msg_type *aclk_incoming_msg_types = aclk_incoming_msg_types_compression;

int aclk_handle_cloud_message(char *payload)
{
    struct aclk_request cloud_to_agent;
    memset(&cloud_to_agent, 0, sizeof(struct aclk_request));

    if (unlikely(!payload)) {
        errno = 0;
        error("ACLK incoming message is empty");
        goto err_cleanup_nojson;
    }

    debug(D_ACLK, "ACLK incoming message (%s)", payload);

    int rc = json_parse(payload, &cloud_to_agent, cloud_to_agent_parse);

    if (unlikely(rc != JSON_OK)) {
        errno = 0;
        error("Malformed json request (%s)", payload);
        goto err_cleanup;
    }

    if (!cloud_to_agent.type_id) {
        errno = 0;
        error("Cloud message is missing compulsory key \"type\"");
        goto err_cleanup;
    }

    for (int i = 0; aclk_incoming_msg_types[i].name; i++) {
        if (strcmp(cloud_to_agent.type_id, aclk_incoming_msg_types[i].name) == 0) {
            if (likely(!aclk_incoming_msg_types[i].fnc(&cloud_to_agent, payload))) {
                // in case of success handler is supposed to clean up after itself
                // or as in the case of aclk_handle_cloud_request take
                // ownership of the pointers (done to avoid copying)
                // see what `aclk_queue_query` parameter `internal` does

                // NEVER CONTINUE THIS LOOP AFTER CALLING FUNCTION!!!
                // msg handlers (namely aclk_handle_version_response)
                // can freely change what aclk_incoming_msg_types points to
                // so either exit or restart this for loop
                freez(cloud_to_agent.type_id);
                return 0;
            }
            goto err_cleanup;
        }
    }

    errno = 0;
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

err_cleanup_nojson:
    if (aclk_stats_enabled) {
        ACLK_STATS_LOCK;
        aclk_metrics_per_sample.cloud_req_err++;
        ACLK_STATS_UNLOCK;
    }

    return 1;
}
