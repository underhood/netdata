#ifndef ACLK_PROXY_H
#define ACLK_PROXY_H

#include <config.h>

#ifdef ACLK_NG
#include "mqtt_wss_client.h"
#endif

#define ACLK_PROXY_PROTO_ADDR_SEPARATOR "://"

typedef enum aclk_proxy_type {
    PROXY_TYPE_UNKNOWN = 0,
    PROXY_TYPE_SOCKS5,
    PROXY_TYPE_HTTP,
    PROXY_DISABLED,
    PROXY_NOT_SET,
} ACLK_PROXY_TYPE;

const char *aclk_proxy_type_to_s(ACLK_PROXY_TYPE *type);
ACLK_PROXY_TYPE aclk_verify_proxy(const char *string);
const char *aclk_lws_wss_get_proxy_setting(ACLK_PROXY_TYPE *type);
void safe_log_proxy_censor(char *proxy);
const char *aclk_get_proxy(ACLK_PROXY_TYPE *type);

#ifdef ACLK_NG
void aclk_set_proxy(char **ohost, int *port, enum mqtt_wss_proxy_type *type);
#endif

#endif /* ACLK_PROXY_H */
