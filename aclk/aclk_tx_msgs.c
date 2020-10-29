// SPDX-License-Identifier: GPL-3.0-or-later

#include "aclk_tx_msgs.h"
#include "../daemon/common.h"

static struct json_object *create_hdr(const char *type, const char *msg_id, time_t ts_secs, usec_t ts_us, int version)
{
    uuid_t uuid;
    char uuid_str[36 + 1];
    json_object *tmp;
    json_object *obj = json_object_new_object();

    tmp = json_object_new_string(type);
    json_object_object_add(obj, "type", tmp);

    if (unlikely(!msg_id)) {
        uuid_generate(uuid);
        uuid_unparse(uuid, uuid_str);
        msg_id = uuid_str;
    }

    if (ts_secs == 0) {
        ts_us = now_realtime_usec();
        ts_secs = ts_us / USEC_PER_SEC;
        ts_us = ts_us % USEC_PER_SEC;
    }

    tmp = json_object_new_string(msg_id);
    json_object_object_add(obj, "msg-id", tmp);

    tmp = json_object_new_int64(ts_secs);
    json_object_object_add(obj, "timestamp", tmp);

// TODO handle this somehow on older json-c
//    tmp = json_object_new_uint64(ts_us);
// probably jso->_to_json_strinf -> custom function
//          jso->o.c_uint64 -> map this with pointer to signed int
// commit that implements json_object_new_uint64 is 3c3b592
// between 0.14 and 0.15
    tmp = json_object_new_int64(ts_us);
    json_object_object_add(obj, "timestamp-offset-usec", tmp);

    tmp = json_object_new_int64(0 /* TODO aclk_session_sec */);
    json_object_object_add(obj, "connect", tmp);

// TODO handle this somehow see above
//    tmp = json_object_new_uint64(0 /* TODO aclk_session_us */);
    tmp = json_object_new_int64(0 /* TODO aclk_session_us */);
    json_object_object_add(obj, "connect-offset-usec", tmp);

    tmp = json_object_new_int(version);
    json_object_object_add(obj, "version", tmp);

    return obj;
}

static char *create_uuid()
{
    uuid_t uuid;
    char *uuid_str = mallocz(36 + 1);

    uuid_generate(uuid);
    uuid_unparse(uuid, uuid_str);

    return uuid_str;
}

/*
 * This will send the /api/v1/info
 */
#define BUFFER_INITIAL_SIZE 4096
//TODO back to void
struct json_object *aclk_send_info_metadata(int metadata_submitted)
{
    BUFFER *local_buffer = buffer_create(BUFFER_INITIAL_SIZE);
    json_object *msg, *payload, *tmp;

    char *msg_id = create_uuid();
    buffer_flush(local_buffer);
    local_buffer->contenttype = CT_APPLICATION_JSON;

    // on_connect messages are sent on a health reload, if the on_connect message is real then we
    // use the session time as the fake timestamp to indicate that it starts the session. If it is
    // a fake on_connect message then use the real timestamp to indicate it is within the existing
    // session.
    if (metadata_submitted)
        msg = create_hdr("update", msg_id, 0, 0, /* TODO aclk_shared_state.version_neg*/ 2);
    else
        msg = create_hdr("connect", msg_id, 0/* TODO aclk_session_sec */, 0/* TODO aclk_session_us */, /* TODO aclk_shared_state.version_neg*/ 2);

    payload = json_object_new_object();
    json_object_object_add(msg, "payload", payload);

    web_client_api_request_v1_info_fill_buffer(localhost, local_buffer);
    tmp = json_tokener_parse(local_buffer->buffer);
    json_object_object_add(payload, "info", tmp);

    buffer_flush(local_buffer);

    charts2json(localhost, local_buffer, 1, 0);
    tmp = json_tokener_parse(local_buffer->buffer);
    json_object_object_add(payload, "charts", tmp);

// TODO
//    aclk_send_message(ACLK_METADATA_TOPIC, local_buffer->buffer, msg_id);
//    json_object_put(msg);

    freez(msg_id);
    buffer_free(local_buffer);
    return msg;
}
