// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ACLK_UTIL_H
#define ACLK_UTIL_H

// Helper functions which should not have any inside ACLK dependency

const char *aclk_get_topic(const char *sub_topic, const char *agent_id, char *final_topic, int max_size);
int aclk_decode_base_url(char *url, char **aclk_hostname, int *aclk_port);

#endif /* ACLK_UTIL_H */
