// SPDX-License-Identifier: GPL-3.0-or-later

#include "aclk_tx_msgs.h"
#include "../daemon/common.h"
#include "aclk_util.h"

#define ACLK_THREAD_NAME "ACLK_Query"
#define ACLK_CHART_TOPIC "outbound/meta"
#define ACLK_ALARMS_TOPIC "outbound/alarms"
#define ACLK_METADATA_TOPIC "outbound/meta"
#define ACLK_COMMAND_TOPIC "inbound/cmd"

#ifndef __GNUC__
#pragma region aclk_tx_msgs helper functions
#endif

#define TOPIC_MAX_LEN 512
static void aclk_send_message(mqtt_wss_client client, json_object *msg, const char *subtopic)
{
    char topic[TOPIC_MAX_LEN];
    const char *str;

    str = json_object_to_json_string_ext(msg, JSON_C_TO_STRING_PLAIN);
    mqtt_wss_publish(client, aclk_get_topic(subtopic, "864a1ca0-b537-4bae-b1aa-059663c21812" /* TODO */, topic, TOPIC_MAX_LEN), str, strlen(str),  MQTT_WSS_PUB_QOS1);
#ifdef ACLK_LOG_CONVERSATION_DIR
#define FN_MAX_LEN 1024
    char filename[FN_MAX_LEN];
    snprintf(filename, FN_MAX_LEN, ACLK_LOG_CONVERSATION_DIR "/%010d-tx.json", ACLK_GET_CONV_LOG_NEXT());
    json_object_to_file_ext(filename, msg, JSON_C_TO_STRING_PRETTY);
#endif
}

/*
 * Creates universal header common for all ACLK messages. User gets ownership of json object created.
 * Usually this is freed by send function after message has been sent.
 */
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

    tmp = json_object_new_int64(aclk_session_sec);
    json_object_object_add(obj, "connect", tmp);

// TODO handle this somehow see above
//    tmp = json_object_new_uint64(0 /* TODO aclk_session_us */);
    tmp = json_object_new_int64(aclk_session_us);
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

#ifndef __GNUC__
#pragma endregion
#endif

#ifndef __GNUC__
#pragma region aclk_tx_msgs message generators
#endif

/*
 * This will send the /api/v1/info
 */
#define BUFFER_INITIAL_SIZE (1024 * 16)
void aclk_send_info_metadata(mqtt_wss_client client, int metadata_submitted, RRDHOST *host)
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
        msg = create_hdr("connect", msg_id, aclk_session_sec, aclk_session_us, /* TODO aclk_shared_state.version_neg*/ 2);

    payload = json_object_new_object();
    json_object_object_add(msg, "payload", payload);

    web_client_api_request_v1_info_fill_buffer(host, local_buffer);
    tmp = json_tokener_parse(local_buffer->buffer);
    json_object_object_add(payload, "info", tmp);

    buffer_flush(local_buffer);

    charts2json(host, local_buffer, 1, 0);
    tmp = json_tokener_parse(local_buffer->buffer);
    json_object_object_add(payload, "charts", tmp);

    aclk_send_message(client, msg, ACLK_METADATA_TOPIC);

    json_object_put(msg);
    freez(msg_id);
    buffer_free(local_buffer);
}

// TODO should include header instead
void health_active_log_alarms_2json(RRDHOST *host, BUFFER *wb);

void aclk_send_alarm_metadata(mqtt_wss_client client, int metadata_submitted)
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
        msg = create_hdr("connect_alarms", msg_id, 0, 0, 2 /* TODO aclk_shared_state.version_neg*/);
    else
        msg = create_hdr("connect_alarms", msg_id, aclk_session_sec, aclk_session_us, 2 /* TODO aclk_shared_state.version_neg*/);

    payload = json_object_new_object();
    json_object_object_add(msg, "payload", payload);

    health_alarms2json(localhost, local_buffer, 1);
    tmp = json_tokener_parse(local_buffer->buffer);
    json_object_object_add(payload, "configured-alarms", tmp);

    buffer_flush(local_buffer);

    health_active_log_alarms_2json(localhost, local_buffer);
    tmp = json_tokener_parse(local_buffer->buffer);
    json_object_object_add(payload, "alarms-active", tmp);

    aclk_send_message(client, msg, ACLK_ALARMS_TOPIC);

    freez(msg_id);
    buffer_free(local_buffer);
}

#ifndef __GNUC__
#pragma endregion
#endif
