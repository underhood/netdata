// SPDX-License-Identifier: GPL-3.0-or-later

#include "aclk_query_queue.h"
#include "aclk_query.h"
#include "aclk_stats.h"

static netdata_mutex_t aclk_query_queue_mutex = NETDATA_MUTEX_INITIALIZER;
#define ACLK_QUEUE_LOCK netdata_mutex_lock(&aclk_query_queue_mutex)
#define ACLK_QUEUE_UNLOCK netdata_mutex_unlock(&aclk_query_queue_mutex)

static struct aclk_query_queue {
    aclk_query_t head;
    aclk_query_t tail;
} aclk_query_queue = {
    .head = NULL,
    .tail = NULL
};

static inline int _aclk_queue_query(aclk_query_t query)
{
    if (aclk_stats_enabled) {
        ACLK_STATS_LOCK;
        aclk_metrics_per_sample.queries_queued++;
        ACLK_STATS_UNLOCK;
    }
    query->created = now_realtime_usec();
    ACLK_QUEUE_LOCK;
    if (!aclk_query_queue.head) {
        aclk_query_queue.head = query;
        aclk_query_queue.tail = query;
        ACLK_QUEUE_UNLOCK;
        return 0;
    }
    // TODO deduplication
    aclk_query_queue.tail->next = query;
    ACLK_QUEUE_UNLOCK;
    return 0;

}

int aclk_queue_query(aclk_query_t query)
{
    int ret = _aclk_queue_query(query);
    if(!ret)
        QUERY_THREAD_WAKEUP;
    return ret;
}

aclk_query_t aclk_queue_pop(void)
{
    aclk_query_t ret;
    ACLK_QUEUE_LOCK;
    ret = aclk_query_queue.head;
    if (!ret) {
        ACLK_QUEUE_UNLOCK;
        return ret;
    }

    aclk_query_queue.head = ret->next;
    if (unlikely(!aclk_query_queue.head))
        aclk_query_queue.tail = aclk_query_queue.head;
    ACLK_QUEUE_UNLOCK;

    ret->next = NULL;
    return ret;
}

void aclk_queue_flush(void)
{
    aclk_query_t query = aclk_queue_pop();
    while (query) {
        aclk_query_free(query);
        query = aclk_queue_pop();
    };
}

aclk_query_t aclk_query_new(aclk_query_type_t type)
{
    aclk_query_t query = callocz(1, sizeof(struct aclk_query));
    query->type = type;
    return query;
}

void aclk_query_free(aclk_query_t query)
{
    if (query->type == HTTP_API_V2) {
        freez(query->data.http_api_v2.payload);
        if (query->data.http_api_v2.query != query->dedup_id)
            freez(query->data.http_api_v2.query);
    }

    freez(query->dedup_id);
    freez(query->callback_topic);
    freez(query->msg_id);
    freez(query);
}
