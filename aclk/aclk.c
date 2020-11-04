#include "aclk.h"

#include "aclk_stats.h"
#include "mqtt_wss_client.h"
#include "aclk_otp.h"
#include "aclk_tx_msgs.h"
#include "aclk_query.h"
#include "aclk_query_queue.h"
#include "aclk_util.h"
#include "aclk_rx_msgs.h"

#ifdef ACLK_LOG_CONVERSATION_DIR
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#define ACLK_STABLE_TIMEOUT 3 // Minimum delay to mark AGENT as stable

//TODO remove most (as in 99.999999999%) of this crap
int aclk_connected = 0;
int aclk_disable_runtime = 0;
int aclk_disable_single_updates = 0;
int aclk_kill_link = 0;

usec_t aclk_session_us = 0;         // Used by the mqtt layer
time_t aclk_session_sec = 0;        // Used by the mqtt layer

mqtt_wss_client mqttwss_client;

netdata_mutex_t aclk_shared_state_mutex = NETDATA_MUTEX_INITIALIZER;
#define ACLK_SHARED_STATE_LOCK netdata_mutex_lock(&aclk_shared_state_mutex)
#define ACLK_SHARED_STATE_UNLOCK netdata_mutex_unlock(&aclk_shared_state_mutex)

//TODO remove
void aclk_dummy() {}

struct aclk_shared_state aclk_shared_state = {
    .agent_state = AGENT_INITIALIZING,
    .last_popcorn_interrupt = 0,
    .version_neg = 0,
    .version_neg_wait_till = 0
};

void aclk_single_update_disable()
{
    aclk_disable_single_updates = 1;
}

void aclk_single_update_enable()
{
    aclk_disable_single_updates = 0;
}

const char *aclk_get_proxy(ACLK_PROXY_TYPE *type)
{
    *type = PROXY_DISABLED;
    return NULL;
}
//ENDTODO

static RSA *aclk_private_key = NULL;
static int load_private_key()
{
    if (aclk_private_key != NULL)
        RSA_free(aclk_private_key);
    aclk_private_key = NULL;
    char filename[FILENAME_MAX + 1];
    snprintfz(filename, FILENAME_MAX, "%s/cloud.d/private.pem", netdata_configured_varlib_dir);

    long bytes_read;
    char *private_key = read_by_filename(filename, &bytes_read);
    if (!private_key) {
        error("Claimed agent cannot establish ACLK - unable to load private key '%s' failed.", filename);
        return 1;
    }
    debug(D_ACLK, "Claimed agent loaded private key len=%ld bytes", bytes_read);

    BIO *key_bio = BIO_new_mem_buf(private_key, -1);
    if (key_bio==NULL) {
        error("Claimed agent cannot establish ACLK - failed to create BIO for key");
        goto biofailed;
    }

    aclk_private_key = PEM_read_bio_RSAPrivateKey(key_bio, NULL, NULL, NULL);
    BIO_free(key_bio);
    if (aclk_private_key!=NULL)
    {
        freez(private_key);
        return 0;
    }
    char err[512];
    ERR_error_string_n(ERR_get_error(), err, sizeof(err));
    error("Claimed agent cannot establish ACLK - cannot create private key: %s", err);

biofailed:
    freez(private_key);
    return 1;
}

static int wait_till_cloud_enabled()
{
    info("Waiting for Cloud to be enabled");
    while (!netdata_cloud_setting) {
        sleep_usec(USEC_PER_SEC * 1);
        if (netdata_exit)
            return 1;
    }
    return 0;
}

/**
 * Will block until agent is claimed. Returns only if agent claimed
 * or if agent needs to shutdown.
 * 
 * @return `0` if agent has been claimed, 
 * `1` if interrupted due to agent shutting down
 */
static int wait_till_agent_claimed(void)
{
    //TODO prevent malloc and freez
    char *agent_id = is_agent_claimed();
    while (likely(!agent_id)) {
        sleep_usec(USEC_PER_SEC * 1);
        if (netdata_exit)
            return 1;
        agent_id = is_agent_claimed();
    }
    freez(agent_id);
    return 0;
}

/**
 * Checks everything is ready for connection
 * agent claimed, cloud url set and private key available
 * 
 * @param aclk_hostname points to location where string pointer to hostname will be set
 * @param ackl_port port to int where port will be saved
 * 
 * @return If non 0 returned irrecoverable error happened and ACLK should be terminated
 */
static int wait_till_agent_claim_ready()
{
    int port;
    char *hostname = NULL;
    while (!netdata_exit) {
        if (wait_till_agent_claimed())
            return 1;

        // The NULL return means the value was never initialised, but this value has been initialized in post_conf_load.
        // We trap the impossible NULL here to keep the linter happy without using a fatal() in the code.
        char *cloud_base_url = appconfig_get(&cloud_config, CONFIG_SECTION_GLOBAL, "cloud base url", NULL);
        if (cloud_base_url == NULL) {
            error("Do not move the cloud base url out of post_conf_load!!");
            return 1;
        }

        // We just check configuration is valid here
        // TODO make it without malloc/free
        freez(hostname);
        if (aclk_decode_base_url(cloud_base_url, &hostname, &port)) {
            error("Agent is claimed but the configuration is invalid, please fix");
            sleep(5);
            continue;
        }

        if (!load_private_key() /* TODO && !_mqtt_lib_init() */) {
            sleep(5);
            break;
        }
    }

    freez(hostname);
    return 0;
}

void aclk_mqtt_wss_log_cb(mqtt_wss_log_type_t log_type, const char* str)
{
    //TODO filter based on log_type
    (void)log_type;
    error(str);
}

#define STRNCMP_PTR_SHIFT(ptr, str) \
    if(strncmp(ptr, str, strlen(str))) \
        error("Received message on unexpected topic %s", topic); \
    ptr+=strlen(str); \

//TODO prevent big buffer on stack
#define RX_MSGLEN_MAX 4096
static void msg_callback(const char *topic, const void *msg, size_t msglen, int qos)
{
    char cmsg[RX_MSGLEN_MAX];
    const char *ptr = topic;
    size_t len = (msglen < RX_MSGLEN_MAX - 1) ? msglen : (RX_MSGLEN_MAX - 1);

    if (msglen > RX_MSGLEN_MAX - 1)
        error("Incoming ACLK message was bigger than MAX of %d and got truncated.", RX_MSGLEN_MAX);

    memcpy(cmsg,
           msg,
           len);
    cmsg[len] = 0;

#ifdef ACLK_LOG_CONVERSATION_DIR
#define FN_MAX_LEN 512
    char filename[FN_MAX_LEN];
    int logfd;
    snprintf(filename, FN_MAX_LEN, ACLK_LOG_CONVERSATION_DIR "/%010d-rx.json", ACLK_GET_CONV_LOG_NEXT());
    logfd = open(filename, O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR );
    if(logfd < 0)
        error("Error opening ACLK Conversation logfile \"%s\" for RX message.", filename);
    write(logfd, msg, msglen);
    close(logfd);
#endif

    debug(D_ACLK, "Got Message From Broker Topic \"%s\" QOS %d MSG: \"%s\"", topic, qos, cmsg);

    // Check topic is what we expect
    STRNCMP_PTR_SHIFT(ptr, "/agent/")

    netdata_mutex_lock(&localhost->claimed_id_lock);
    STRNCMP_PTR_SHIFT(ptr, localhost->claimed_id);
    netdata_mutex_unlock(&localhost->claimed_id_lock);

    STRNCMP_PTR_SHIFT(ptr, "/inbound/cmd");
    // This should be the end of it
    if (*ptr)
        error("Received message on unexpected topic %s", topic);

    aclk_handle_cloud_message(cmsg);
}

static void puback_callback(uint16_t packet_id)
{
    error("Got PUBACK for %d", (int)packet_id);
}

static int read_query_thread_count()
{
    int threads = MIN(processors/2, 6);
    threads = MAX(threads, 2);
    threads = config_get_number(CONFIG_SECTION_CLOUD, "query thread count", threads);
    if(threads < 1) {
        error("You need at least one query thread. Overriding configured setting of \"%d\"", threads);
        threads = 1;
        config_set_number(CONFIG_SECTION_CLOUD, "query thread count", threads);
    }
    return threads;
}

/* Keeps connection alive and handles all network comms.
 * Returns on error or when netdata is shutting down.
 * @param client instance of mqtt_wss_client
 * @returns  0 - Netdata Exits
 *          >0 - Error happened. Reconnect and start over.
 */
static int handle_connection(mqtt_wss_client client)
{
    time_t last_periodic_query_wakeup = now_monotonic_sec();
    while (!netdata_exit) {
        // timeout 1000 to check at least once a second
        // for netdata_exit
        if (mqtt_wss_service(client, 1000)){
            error("Connection Error or Dropped");
            return 1;
        }

        // mqtt_wss_service will return faster than in one second
        // if there is enough work to do
        time_t now = now_monotonic_sec();
        if (last_periodic_query_wakeup < now) {
            // wake up at least one Query Thread at least
            // once per second
            last_periodic_query_wakeup = now;
            QUERY_THREAD_WAKEUP;
        }
    }
    return 0;
}

inline static int aclk_popcorn_check_bump()
{
    ACLK_SHARED_STATE_LOCK;
    if (unlikely(aclk_shared_state.agent_state == AGENT_INITIALIZING)) {
        aclk_shared_state.last_popcorn_interrupt = now_realtime_sec();
        ACLK_SHARED_STATE_UNLOCK;
        return 1;
    }
    ACLK_SHARED_STATE_UNLOCK;
    return 0;
}

/*
 * Subscribe to a topic in the cloud
 * The final subscription will be in the form
 * /agent/claim_id/<sub_topic>
 */
#define ACLK_MAX_TOPIC 255
static int aclk_subscribe(mqtt_wss_client client, char *sub_topic, int qos)
{
    char topic[ACLK_MAX_TOPIC + 1];
    const char *final_topic;

    final_topic = aclk_get_topic(sub_topic, "864a1ca0-b537-4bae-b1aa-059663c21812" /* TODO */, topic, ACLK_MAX_TOPIC);
    if (unlikely(!final_topic)) {
        errno = 0;
        error("Unable to build outgoing topic; truncated?");
        return 1;
    }

    return mqtt_wss_subscribe(client, final_topic, qos);
}

static inline void localhost_popcorn_finish_actions(mqtt_wss_client client, struct aclk_query_threads *query_threads)
{
    aclk_subscribe(client, ACLK_COMMAND_TOPIC, 1);
    if (unlikely(!query_threads->thread_list))
        aclk_query_threads_start(query_threads, client);

    aclk_query_t query = aclk_query_new(METADATA_INFO);
    query->data.metadata_info.host = localhost;
    query->data.metadata_info.initial_on_connect = 1;
    aclk_queue_query(query);
    query = aclk_query_new(METADATA_ALARMS);
    query->data.metadata_alarms.initial_on_connect = 1;
    aclk_queue_query(query);
}

static inline void mqtt_connected_actions(mqtt_wss_client client)
{
    // TODO global vars?
    usec_t now = now_realtime_usec();
    aclk_session_sec = now / USEC_PER_SEC;
    aclk_session_us = now % USEC_PER_SEC;

    aclk_hello_msg(client);
}

/* Waits until agent is ready or needs to exit
 * @param client instance of mqtt_wss_client
 * @param query_threads pointer to aclk_query_threads
 *        structure where to store data about started query threads
 * @return  0 - Popcorning Finished - Agent STABLE,
 *         !0 - netdata_exit
 */
static int wait_popcorning_finishes(mqtt_wss_client client, struct aclk_query_threads *query_threads)
{
    time_t elapsed;
    int need_wait;
    while (!netdata_exit) {
        ACLK_SHARED_STATE_LOCK;
        if (likely(aclk_shared_state.agent_state != AGENT_INITIALIZING)) {
            ACLK_SHARED_STATE_UNLOCK;
            return 0;
        }
        elapsed = now_realtime_sec() - aclk_shared_state.last_popcorn_interrupt;
        if (elapsed > ACLK_STABLE_TIMEOUT) {
            aclk_shared_state.agent_state = AGENT_STABLE;
            ACLK_SHARED_STATE_UNLOCK;
            error("ACLK localhost popocorn finished");
            localhost_popcorn_finish_actions(client, query_threads);
            return 0;
        }
        ACLK_SHARED_STATE_UNLOCK;
        need_wait = ACLK_STABLE_TIMEOUT - elapsed;
        error("ACLK localhost popocorn wait %d seconds longer", need_wait);
        sleep(need_wait);
    }
    return 1;
}

/* Attempts to make a connection to MQTT broker over WSS
 * @param client instance of mqtt_wss_client
 * @return  0 - Successfull Connection,
 *          <0 - Irrecoverable Error -> Kill ACLK,
 *          >0 - netdata_exit
 */
#define CLOUD_BASE_URL_READ_RETRY 30
static int aclk_attempt_to_connect(mqtt_wss_client client)
{
    char *aclk_hostname = NULL;
    int aclk_port;

    char *mqtt_otp_user = NULL;
    char *mqtt_otp_pass = NULL;

    while (!netdata_exit) {
        char *cloud_base_url = appconfig_get(&cloud_config, CONFIG_SECTION_GLOBAL, "cloud base url", NULL);
        if (cloud_base_url == NULL) {
            error("Do not move the cloud base url out of post_conf_load!!");
            return -1;
        }

        freez(aclk_hostname);
        if (aclk_decode_base_url(cloud_base_url, &aclk_hostname, &aclk_port)) {
            error("ACLK base URL configuration key could not be parsed. Will retry in %d seconds.", CLOUD_BASE_URL_READ_RETRY);
            sleep(CLOUD_BASE_URL_READ_RETRY);
            continue;
        }
#ifndef ACLK_DISABLE_CHALLENGE
        aclk_get_mqtt_otp(aclk_private_key, aclk_hostname, aclk_port, &mqtt_otp_user, &mqtt_otp_pass);
        struct mqtt_connect_params mqtt_conn_params = {
            .clientid = mqtt_otp_user,
            .username = mqtt_otp_user,
            .password = mqtt_otp_pass
        };
        if (!mqtt_wss_connect(client, aclk_hostname, aclk_port, &mqtt_conn_params)) {
#else
        if (!mqtt_wss_connect(client, aclk_hostname, aclk_port, "anon", "anon")) {
#endif
            freez(aclk_hostname);
            info("MQTTWSS connection succeeded");
            mqtt_connected_actions(client);
            return 0;
        }

        printf("Connect failed\n");
        // TODO exponential something thing or other thing instead of 1
        sleep(1);
        printf("Attempting Reconnect\n");
    }

    freez(aclk_hostname);
    return 1;
}

/**
 * Main agent cloud link thread
 *
 * This thread will simply call the main event loop that handles
 * pending requests - both inbound and outbound
 *
 * @param ptr is a pointer to the netdata_static_thread structure.
 *
 * @return It always returns NULL
 */
void *aclk_main(void *ptr)
{
    struct netdata_static_thread *static_thread = (struct netdata_static_thread *)ptr;

    struct aclk_stats_thread *stats_thread = NULL;

    struct aclk_query_threads query_threads;
    query_threads.thread_list = NULL;

    // This thread is unusual in that it cannot be cancelled by cancel_main_threads()
    // as it must notify the far end that it shutdown gracefully and avoid the LWT.
    netdata_thread_disable_cancelability();

#if defined( DISABLE_CLOUD ) || !defined( ENABLE_ACLK )
    info("Killing ACLK thread -> cloud functionality has been disabled");
    static_thread->enabled = NETDATA_MAIN_THREAD_EXITED;
    return NULL;
#endif
    aclk_popcorn_check_bump(); // start localhost popcorn timer
    query_threads.count = read_query_thread_count();

    if (wait_till_cloud_enabled())
        goto exit;

    if (wait_till_agent_claim_ready())
        goto exit;

    if (!(mqttwss_client = mqtt_wss_new("mqtt_wss", aclk_mqtt_wss_log_cb, msg_callback, puback_callback))) {
        error("Couldn't initialize MQTT_WSS network library");
        goto exit;
    }

    aclk_stats_enabled = config_get_boolean(CONFIG_SECTION_CLOUD, "statistics", CONFIG_BOOLEAN_YES);
    if (aclk_stats_enabled) {
        stats_thread = callocz(1, sizeof(struct aclk_stats_thread));
        stats_thread->thread = mallocz(sizeof(netdata_thread_t));
        stats_thread->query_thread_count = query_threads.count;
        netdata_thread_create(
            stats_thread->thread, ACLK_STATS_THREAD_NAME, NETDATA_THREAD_OPTION_JOINABLE, aclk_stats_main_thread,
            stats_thread);
    }

    // Keep reconnecting and talking until our time has come
    // and the Grim Reaper (netdata_exit) calls
    do {
        if (aclk_attempt_to_connect(mqttwss_client))
            goto exit_full;

        // TODO
        // warning this assumes the popcorning is relative short
        // if that changes call mqtt_wss_service from within
        // to keep OpenSSL, WSS and MQTT connection alive
        if (wait_popcorning_finishes(mqttwss_client, &query_threads))
            goto exit_full;

        handle_connection(mqttwss_client);
    } while (!netdata_exit);

exit_full:
// Tear Down
    QUERY_THREAD_WAKEUP_ALL;

    aclk_query_threads_cleanup(&query_threads);

    if (aclk_stats_enabled) {
        netdata_thread_join(*stats_thread->thread, NULL);
        aclk_stats_thread_cleanup();
        freez(stats_thread->thread);
        freez(stats_thread);
    }
exit:
    static_thread->enabled = NETDATA_MAIN_THREAD_EXITED;
    return NULL;
}
