// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NETDATA_SQLITE_ACLK_H
#define NETDATA_SQLITE_ACLK_H

#include "sqlite3.h"

enum aclk_database_opcode {
    /* can be used to return empty status or flush the command queue */
    ACLK_DATABASE_NOOP = 0,
    ACLK_DATABASE_CLEANUP,
    ACLK_DATABASE_TIMER,
    ACLK_DATABASE_ADD_CHART,
    ACLK_DATABASE_UPD_CHART,
    ACLK_DATABASE_UPD_ALERT,
    ACLK_DATABASE_SHUTDOWN,
    ACLK_DATABASE_MAX_OPCODE
};

struct aclk_database_cmd {
    enum aclk_database_opcode opcode;
    void *data;
    void *data_param;
    union {
//        struct rrdeng_read_page {
//            struct rrdeng_page_descr *page_cache_descr;
//        } read_page;
//        struct rrdeng_read_extent {
//            struct rrdeng_page_descr *page_cache_descr[MAX_PAGES_PER_EXTENT];
//            int page_count;
//        } read_extent;
        struct completion *completion;
    };
};

#define ACLK_DATABASE_CMD_Q_MAX_SIZE (2048)

struct aclk_database_cmdqueue {
    unsigned head, tail;
    struct aclk_database_cmd cmd_array[ACLK_DATABASE_CMD_Q_MAX_SIZE];
};

struct aclk_database_worker_config {
    uv_thread_t thread;
    char uuid_str[GUID_LEN + 1];
    uv_loop_t *loop;
    RRDHOST *host;
    uv_async_t async;
    /* FIFO command queue */
    uv_mutex_t cmd_mutex;
    uv_cond_t cmd_cond;
    volatile unsigned queue_size;
    struct aclk_database_cmdqueue cmd_queue;
    int error;
};

//extern void sqlite_worker(void* arg);
extern void aclk_database_enq_cmd(struct aclk_database_worker_config *wc, struct aclk_database_cmd *cmd);

extern void sql_queue_chart_to_aclk(RRDSET *st, int cmd);
extern sqlite3 *db_meta;
extern void sql_create_aclk_table(RRDHOST *host);
void aclk_add_chart_event(RRDSET *st, char *payload_type);

extern void aclk_set_architecture(int mode);
#endif //NETDATA_SQLITE_ACLK_H
