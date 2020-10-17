#include "aclk.h"

#include "aclk_stats.h"
#include "mqtt_wss_client.h"
#include "aclk_otp.h"
//#include "aclk_query.h"

//TODO remove most of this crap
int aclk_force_reconnect = 0;
int aclk_connecting = 0;
int aclk_connected = 0;
int aclk_subscribed = 0;
char *aclk_username = NULL, *aclk_password = NULL;
int aclk_disable_runtime = 0;
int aclk_disable_single_updates = 0;
int aclk_kill_link = 0;

mqtt_wss_client mqttwss_client;

netdata_mutex_t aclk_shared_state_mutex = NETDATA_MUTEX_INITIALIZER;

struct aclk_shared_state aclk_shared_state = {
    .metadata_submitted = ACLK_METADATA_REQUIRED,
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

static int aclk_decode_base_url(char *url, char **aclk_hostname, int *aclk_port)
{
    int pos = 0;
    if (!strncmp("https://", url, 8)) {
        pos = 8;
    } else if (!strncmp("http://", url, 7)) {
        error("Cannot connect ACLK over %s -> unencrypted link is not supported", url);
        return 1;
    }
    int host_end = pos;
    while (url[host_end] != 0 && url[host_end] != '/' && url[host_end] != ':')
        host_end++;
    if (url[host_end] == 0) {
        *aclk_hostname = strdupz(url + pos);
        *aclk_port = 443;
        info("Setting ACLK target host=%s port=%d from %s", *aclk_hostname, *aclk_port, url);
        return 0;
    }
    if (url[host_end] == ':') {
        *aclk_hostname = callocz(host_end - pos + 1, 1);
        strncpy(*aclk_hostname, url + pos, host_end - pos);
        int port_end = host_end + 1;
        while (url[port_end] >= '0' && url[port_end] <= '9')
            port_end++;
        if (port_end - host_end > 6) {
            error("Port specified in %s is invalid", url);
            return 0;
        }
        *aclk_port = atoi(&url[host_end+1]);
    }
    if (url[host_end] == '/') {
        *aclk_port = 443;
        *aclk_hostname = callocz(1, host_end - pos + 1);
        strncpy(*aclk_hostname, url+pos, host_end - pos);
    }
    info("Setting ACLK target host=%s port=%d from %s", *aclk_hostname, *aclk_port, url);
    return 0;
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
static int wait_till_agent_claim_ready(char **aclk_hostname, int *aclk_port)
{
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

        if (aclk_decode_base_url(cloud_base_url, aclk_hostname, aclk_port)) {
            error("Agent is claimed but the configuration is invalid, please fix");
            sleep(5);
            continue;
        }

        if (!load_private_key() /* TODO && !_mqtt_lib_init() */) {
            sleep(5);
            break;
        }
    }

    return 0;
}

void aclk_mqtt_wss_log_cb(mqtt_wss_log_type_t log_type, const char* str)
{
    (void)log_type;
    error(str);
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

    char *aclk_hostname = NULL; // Initializers are over-written but prevent gcc complaining about clobbering.
    int aclk_port;

    char *mqtt_otp_user = NULL;
    char *mqtt_otp_pass = NULL;

    // This thread is unusual in that it cannot be cancelled by cancel_main_threads()
    // as it must notify the far end that it shutdown gracefully and avoid the LWT.
    netdata_thread_disable_cancelability();

#if defined( DISABLE_CLOUD ) || !defined( ENABLE_ACLK )
    info("Killing ACLK thread -> cloud functionality has been disabled");
    static_thread->enabled = NETDATA_MAIN_THREAD_EXITED;
    return NULL;
#endif
    if (wait_till_cloud_enabled())
        goto exit;

    if (wait_till_agent_claim_ready(&aclk_hostname, &aclk_port))
        goto exit;

    if (!(mqttwss_client = mqtt_wss_new("mqtt_wss", aclk_mqtt_wss_log_cb))) {
        error("Couldn't initialize MQTT_WSS network library");
        goto exit;
    }

    aclk_stats_enabled = config_get_boolean(CONFIG_SECTION_CLOUD, "statistics", CONFIG_BOOLEAN_YES);
    if (aclk_stats_enabled) {
        stats_thread = callocz(1, sizeof(struct aclk_stats_thread));
        stats_thread->thread = mallocz(sizeof(netdata_thread_t));
        stats_thread->query_thread_count = 2;//TODO query_threads.count;
        netdata_thread_create(
            stats_thread->thread, ACLK_STATS_THREAD_NAME, NETDATA_THREAD_OPTION_JOINABLE, aclk_stats_main_thread,
            stats_thread);
    }

    while (1) {
        aclk_get_mqtt_otp(aclk_private_key, aclk_hostname, aclk_port, &mqtt_otp_user, &mqtt_otp_pass);
        if (!mqtt_wss_connect(mqttwss_client, aclk_hostname, aclk_port, mqtt_otp_user, mqtt_otp_pass))
            break;
        printf("Connect failed\n");
        sleep(1);
        printf("Attempting Reconnect\n");
    }

    while (!netdata_exit) {
        mqtt_wss_service(mqttwss_client, -1);
    }

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
