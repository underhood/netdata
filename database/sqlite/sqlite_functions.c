// SPDX-License-Identifier: GPL-3.0-or-later

#include "sqlite_functions.h"

const char *database_config[] = {
    "PRAGMA auto_vacuum=incremental; PRAGMA synchronous=1 ; PRAGMA journal_mode=WAL; PRAGMA temp_store=MEMORY;",
    "PRAGMA journal_size_limit=16777216;",
    "CREATE TABLE IF NOT EXISTS host(host_id blob PRIMARY KEY, hostname text, "
    "registry_hostname text, update_every int, os text, timezone text, tags text);",
    "CREATE TABLE IF NOT EXISTS chart(chart_id blob PRIMARY KEY, host_id blob, type text, id text, name text, "
    "family text, context text, title text, unit text, plugin text, module text, priority int, update_every int, "
    "chart_type int, memory_mode int, history_entries);",
    "CREATE TABLE IF NOT EXISTS dimension(dim_id blob PRIMARY KEY, chart_id blob, id text, name text, "
    "multiplier int, divisor int , algorithm int, options text);",
    "DROP TABLE IF EXISTS chart_active;",
    "DROP TABLE IF EXISTS dimension_active;",
    "CREATE TABLE IF NOT EXISTS chart_active(chart_id blob PRIMARY KEY, date_created int);",
    "CREATE TABLE IF NOT EXISTS dimension_active(dim_id blob primary key, date_created int);",
    "CREATE TABLE IF NOT EXISTS metadata_migration(filename text, file_size, date_created int);",
    "CREATE INDEX IF NOT EXISTS ind_d1 on dimension (chart_id, id, name);",
    "CREATE INDEX IF NOT EXISTS ind_c1 on chart (host_id, id, type, name);",
    "CREATE TABLE IF NOT EXISTS chart_label(chart_id blob, source_type int, label_key text, "
    "label_value text, date_created int, PRIMARY KEY (chart_id, label_key));",
    "CREATE TABLE IF NOT EXISTS node_instance (host_id blob PRIMARY KEY, claim_id, node_id, date_created);",

    "CREATE TABLE IF NOT EXISTS chart_hash(hash_id blob PRIMARY KEY,type text, id text, name text, "
    "family text, context text, title text, unit text, plugin text, module text, priority integer, last_used);",

    "delete from chart_active;",
    "delete from dimension_active;",
    "delete from chart where chart_id not in (select chart_id from dimension);",
    "delete from host where host_id not in (select host_id from chart);",
    "delete from chart_label where chart_id not in (select chart_id from chart);",
    NULL
};

sqlite3 *db_meta = NULL;

static uv_mutex_t sqlite_transaction_lock;

int execute_insert(sqlite3_stmt *res)
{
    int rc;

    while ((rc = sqlite3_step(res)) != SQLITE_DONE && unlikely(netdata_exit)) {
        if (likely(rc == SQLITE_BUSY || rc == SQLITE_LOCKED))
            usleep(SQLITE_INSERT_DELAY * USEC_PER_MS);
        else {
            error_report("SQLite error %d", rc);
            break;
        }
    }

    return rc;
}

#define MAX_OPEN_STATEMENTS (512)

static void add_stmt_to_list(sqlite3_stmt *res)
{
    static int idx = 0;
    static sqlite3_stmt *statements[MAX_OPEN_STATEMENTS];

    if (unlikely(!res)) {
        while (idx > 0)
            sqlite3_finalize(statements[--idx]);
        return;
    }

    if (unlikely(idx == MAX_OPEN_STATEMENTS))
        return;
    statements[idx++] = res;
}

static int prepare_statement(sqlite3 *database, char *query, sqlite3_stmt **statement) {
    int rc = sqlite3_prepare_v2(database, query, -1, statement, 0);
    if (likely(rc == SQLITE_OK))
        add_stmt_to_list(*statement);
    return rc;
}

/*
 * Store a chart or dimension UUID in  chart_active or dimension_active
 * The statement that will be prepared determines that
 */

static int store_active_uuid_object(sqlite3_stmt **res, char *statement, uuid_t *uuid)
{
    int rc;

    // Check if we should need to prepare the statement
    if (!*res) {
        rc = sqlite3_prepare_v2(db_meta, statement, -1, res, 0);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to store active object, rc = %d", rc);
            return rc;
        }
    }

    rc = sqlite3_bind_blob(*res, 1, uuid, sizeof(*uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to bind input parameter to store active object, rc = %d", rc);
    else
        rc = execute_insert(*res);
    return rc;
}

/*
 * Marks a chart with UUID as active
 * Input: UUID
 */
void store_active_chart(uuid_t *chart_uuid)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    if (unlikely(!chart_uuid))
        return;

    rc = store_active_uuid_object(&res, SQL_STORE_ACTIVE_CHART, chart_uuid);
    if (rc != SQLITE_DONE)
        error_report("Failed to store active chart, rc = %d", rc);

    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize statement in store active chart, rc = %d", rc);
    return;
}

/*
 * Marks a dimension with UUID as active
 * Input: UUID
 */
void store_active_dimension(uuid_t *dimension_uuid)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    if (unlikely(!dimension_uuid))
        return;

    rc = store_active_uuid_object(&res, SQL_STORE_ACTIVE_DIMENSION, dimension_uuid);
    if (rc != SQLITE_DONE)
        error_report("Failed to store active dimension, rc = %d", rc);

    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize statement in store active dimension, rc = %d", rc);
    return;
}

/*
 * Initialize the SQLite database
 * Return 0 on success
 */
int sql_init_database(void)
{
    char *err_msg = NULL;
    char sqlite_database[FILENAME_MAX + 1];
    int rc;

    fatal_assert(0 == uv_mutex_init(&sqlite_transaction_lock));

    snprintfz(sqlite_database, FILENAME_MAX, "%s/netdata-meta.db", netdata_configured_cache_dir);
    rc = sqlite3_open(sqlite_database, &db_meta);
    if (rc != SQLITE_OK) {
        error_report("Failed to initialize database at %s, due to \"%s\"", sqlite_database, sqlite3_errstr(rc));
        sqlite3_close(db_meta);
        db_meta = NULL;
        return 1;
    }

    info("SQLite database %s initialization", sqlite_database);

    for (int i = 0; database_config[i]; i++) {
        debug(D_METADATALOG, "Executing %s", database_config[i]);
        rc = sqlite3_exec(db_meta, database_config[i], 0, 0, &err_msg);
        if (rc != SQLITE_OK) {
            error_report("SQLite error during database setup, rc = %d (%s)", rc, err_msg);
            error_report("SQLite failed statement %s", database_config[i]);
            sqlite3_free(err_msg);
            return 1;
        }
    }
    info("SQLite database initialization completed");
//    uv_thread_t   sqlite_event_loop_t;
//    struct sqlite_worker_config worker_config;
//    memset(&worker_config, 0, sizeof(worker_config));
//    fatal_assert(0 == uv_thread_create(&sqlite_event_loop_t, sqlite_worker, &worker_config));
    return 0;
}

/*
 * Close the sqlite database
 */

void sql_close_database(void)
{
    int rc;
    if (unlikely(!db_meta))
        return;

    info("Closing SQLite database");

    add_stmt_to_list(NULL);

    rc = sqlite3_close_v2(db_meta);
    if (unlikely(rc != SQLITE_OK))
        error_report("Error %d while closing the SQLite database, %s", rc, sqlite3_errstr(rc));
    return;
}

#define FIND_UUID_TYPE  "select 1 from host where host_id = @uuid union select 2 from chart where chart_id = @uuid union select 3 from dimension where dim_id = @uuid;"

int find_uuid_type(uuid_t *uuid)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc;
    int uuid_type = 3;

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, FIND_UUID_TYPE, &res);
        if (rc != SQLITE_OK) {
            error_report("Failed to bind prepare statement to find UUID type in the database");
            return 0;
        }
    }

    rc = sqlite3_bind_blob(res, 1, uuid, sizeof(*uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_step(res);
    if (likely(rc == SQLITE_ROW))
        uuid_type = sqlite3_column_int(res, 0);

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement during find uuid type, rc = %d", rc);

    return uuid_type;

bind_fail:
    return 0;
}

uuid_t *find_dimension_uuid(RRDSET *st, RRDDIM *rd)
{
    static __thread sqlite3_stmt *res = NULL;
    uuid_t *uuid = NULL;
    int rc;

    if (unlikely(!db_meta) && default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
        return NULL;

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, SQL_FIND_DIMENSION_UUID, &res);
        if (rc != SQLITE_OK) {
            error_report("Failed to bind prepare statement to lookup dimension UUID in the database");
            return NULL;
        }
    }

    rc = sqlite3_bind_blob(res, 1, st->chart_uuid, sizeof(*st->chart_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 2, rd->id, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 3, rd->name, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_step(res);
    if (likely(rc == SQLITE_ROW)) {
        uuid = mallocz(sizeof(uuid_t));
        uuid_copy(*uuid, sqlite3_column_blob(res, 0));
    }

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement find dimension uuid, rc = %d", rc);

#ifdef NETDATA_INTERNAL_CHECKS
    char  uuid_str[GUID_LEN + 1];
    if (likely(uuid)) {
        uuid_unparse_lower(*uuid, uuid_str);
        debug(D_METADATALOG, "Found UUID %s for dimension %s", uuid_str, rd->name);
    }
    else
        debug(D_METADATALOG, "UUID not found for dimension %s", rd->name);
#endif
    return uuid;

bind_fail:
    error_report("Failed to bind input parameter to perform dimension UUID database lookup, rc = %d", rc);
    return NULL;
}

uuid_t *create_dimension_uuid(RRDSET *st, RRDDIM *rd)
{
    uuid_t *uuid = NULL;
    int rc;

    uuid = mallocz(sizeof(uuid_t));
    uuid_generate(*uuid);

#ifdef NETDATA_INTERNAL_CHECKS
    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(*uuid, uuid_str);
    debug(D_METADATALOG,"Generating uuid [%s] for dimension %s under chart %s", uuid_str, rd->name, st->id);
#endif

    rc = sql_store_dimension(uuid, st->chart_uuid, rd->id, rd->name, rd->multiplier, rd->divisor, rd->algorithm);
    if (unlikely(rc))
       error_report("Failed to store dimension metadata in the database");

    return uuid;
}

#define DELETE_DIMENSION_UUID   "delete from dimension where dim_id = @uuid;"

void delete_dimension_uuid(uuid_t *dimension_uuid)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc;

#ifdef NETDATA_INTERNAL_CHECKS
    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(*dimension_uuid, uuid_str);
    debug(D_METADATALOG,"Deleting dimension uuid %s", uuid_str);
#endif

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, DELETE_DIMENSION_UUID, &res);
        if (rc != SQLITE_OK) {
            error_report("Failed to prepare statement to delete a dimension uuid");
            return;
        }
    }

    rc = sqlite3_bind_blob(res, 1, dimension_uuid,  sizeof(*dimension_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_step(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to delete dimension uuid, rc = %d", rc);

bind_fail:
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement when deleting dimension UUID, rc = %d", rc);
    return;
}

/*
 * Do a database lookup to find the UUID of a chart
 *
 */
uuid_t *find_chart_uuid(RRDHOST *host, const char *type, const char *id, const char *name)
{
    static __thread sqlite3_stmt *res = NULL;
    uuid_t *uuid = NULL;
    int rc;

    if (unlikely(!db_meta) && default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
        return NULL;

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, SQL_FIND_CHART_UUID, &res);
        if (rc != SQLITE_OK) {
            error_report("Failed to prepare statement to lookup chart UUID in the database");
            return NULL;
        }
    }

    rc = sqlite3_bind_blob(res, 1, &host->host_uuid, sizeof(host->host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
            goto bind_fail;

    rc = sqlite3_bind_text(res, 2, type, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 3, id, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 4, name ? name : id, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_step(res);
    if (likely(rc == SQLITE_ROW)) {
        uuid = mallocz(sizeof(uuid_t));
        uuid_copy(*uuid, sqlite3_column_blob(res, 0));
    }

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement when searching for a chart UUID, rc = %d", rc);

#ifdef NETDATA_INTERNAL_CHECKS
    char  uuid_str[GUID_LEN + 1];
    if (likely(uuid)) {
        uuid_unparse_lower(*uuid, uuid_str);
        debug(D_METADATALOG, "Found UUID %s for chart %s.%s", uuid_str, type, name ? name : id);
    }
    else
        debug(D_METADATALOG, "UUID not found for chart %s.%s", type, name ? name : id);
#endif
    return uuid;

bind_fail:
    error_report("Failed to bind input parameter to perform chart UUID database lookup, rc = %d", rc);
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement when searching for a chart UUID, rc = %d", rc);
    return NULL;
}

int update_chart_metadata(uuid_t *chart_uuid, RRDSET *st, const char *id, const char *name)
{
    int rc;

    if (unlikely(!db_meta) && default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
        return 0;

    rc = sql_store_chart(
        chart_uuid, &st->rrdhost->host_uuid, st->type, id, name, st->family, st->context, st->title, st->units, st->plugin_name,
        st->module_name, st->priority, st->update_every, st->chart_type, st->rrd_memory_mode, st->entries);

    return rc;
}

uuid_t *create_chart_uuid(RRDSET *st, const char *id, const char *name)
{
    uuid_t *uuid = NULL;
    int rc;

    uuid = mallocz(sizeof(uuid_t));
    uuid_generate(*uuid);

#ifdef NETDATA_INTERNAL_CHECKS
    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(*uuid, uuid_str);
    debug(D_METADATALOG,"Generating uuid [%s] for chart %s under host %s", uuid_str, st->id, st->rrdhost->hostname);
#endif

    rc = update_chart_metadata(uuid, st, id, name);

    if (unlikely(rc))
        error_report("Failed to store chart metadata in the database");

    return uuid;
}

// Functions to create host, chart, dimension in the database

int sql_store_host(
    uuid_t *host_uuid, const char *hostname, const char *registry_hostname, int update_every, const char *os,
    const char *tzone, const char *tags)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
            return 0;
        error_report("Database has not been initialized");
        return 1;
    }

    if (unlikely((!res))) {
        rc = prepare_statement(db_meta, SQL_STORE_HOST, &res);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to store host, rc = %d", rc);
            return 1;
        }
    }

    rc = sqlite3_bind_blob(res, 1, host_uuid, sizeof(*host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 2, hostname, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 3, registry_hostname, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_int(res, 4, update_every);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 5, os, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 6, tzone, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 7, tags, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    int store_rc = sqlite3_step(res);
    if (unlikely(store_rc != SQLITE_DONE))
        error_report("Failed to store host %s, rc = %d", hostname, rc);

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement to store host %s, rc = %d", hostname, rc);

    return !(store_rc == SQLITE_DONE);
bind_fail:
    error_report("Failed to bind parameter to store host %s, rc = %d", hostname, rc);
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement to store host %s, rc = %d", hostname, rc);
    return 1;
}

/*
 * Store a chart in the database
 */

int sql_store_chart(
    uuid_t *chart_uuid, uuid_t *host_uuid, const char *type, const char *id, const char *name, const char *family,
    const char *context, const char *title, const char *units, const char *plugin, const char *module, long priority,
    int update_every, int chart_type, int memory_mode, long history_entries)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc, param = 0;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
            return 0;
        error_report("Database has not been initialized");
        return 1;
    }

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, SQL_STORE_CHART, &res);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to store chart, rc = %d", rc);
            return 1;
        }
    }

    param++;
    rc = sqlite3_bind_blob(res, 1, chart_uuid, sizeof(*chart_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_blob(res, 2, host_uuid, sizeof(*host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 3, type, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 4, id, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    if (name && *name)
        rc = sqlite3_bind_text(res, 5, name, -1, SQLITE_STATIC);
    else
        rc = sqlite3_bind_null(res, 5);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 6, family, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 7, context, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 8, title, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 9, units, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 10, plugin, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 11, module, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_int(res, 12, priority);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_int(res, 13, update_every);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_int(res, 14, chart_type);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_int(res, 15, memory_mode);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_int(res, 16, history_entries);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store chart, rc = %d", rc);

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in chart store function, rc = %d", rc);

    return 0;

bind_fail:
    error_report("Failed to bind parameter %d to store chart, rc = %d", param, rc);
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in chart store function, rc = %d", rc);
    return 1;
}

/*
 * Store a dimension
 */
int sql_store_dimension(
    uuid_t *dim_uuid, uuid_t *chart_uuid, const char *id, const char *name, collected_number multiplier,
    collected_number divisor, int algorithm)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
            return 0;
        error_report("Database has not been initialized");
        return 1;
    }

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, SQL_STORE_DIMENSION, &res);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to store dimension, rc = %d", rc);
            return 1;
        }
    }

    rc = sqlite3_bind_blob(res, 1, dim_uuid, sizeof(*dim_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_blob(res, 2, chart_uuid, sizeof(*chart_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 3, id, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_text(res, 4, name, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_int(res, 5, multiplier);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_int(res, 6, divisor);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = sqlite3_bind_int(res, 7, algorithm);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store dimension, rc = %d", rc);

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in store dimension, rc = %d", rc);
    return 0;

bind_fail:
    error_report("Failed to bind parameter to store dimension, rc = %d", rc);
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in store dimension, rc = %d", rc);
    return 1;
}


//
// Support for archived charts
//
#define SELECT_DIMENSION "select d.id, d.name from dimension d where d.chart_id = @chart_uuid;"

void sql_rrdim2json(sqlite3_stmt *res_dim, uuid_t *chart_uuid, BUFFER *wb, size_t *dimensions_count)
{
    int rc;

    rc = sqlite3_bind_blob(res_dim, 1, chart_uuid, sizeof(*chart_uuid), SQLITE_STATIC);
    if (rc != SQLITE_OK)
        return;

    int dimensions = 0;
    buffer_sprintf(wb, "\t\t\t\"dimensions\": {\n");

    while (sqlite3_step(res_dim) == SQLITE_ROW) {
        if (dimensions)
            buffer_strcat(wb, ",\n\t\t\t\t\"");
        else
            buffer_strcat(wb, "\t\t\t\t\"");
        buffer_strcat_jsonescape(wb, (const char *) sqlite3_column_text(res_dim, 0));
        buffer_strcat(wb, "\": { \"name\": \"");
        buffer_strcat_jsonescape(wb, (const char *) sqlite3_column_text(res_dim, 1));
        buffer_strcat(wb, "\" }");
        dimensions++;
    }
    *dimensions_count += dimensions;
    buffer_sprintf(wb, "\n\t\t\t}");
}

#define SELECT_CHART "select chart_id, id, name, type, family, context, title, priority, plugin, " \
    "module, unit, chart_type, update_every from chart " \
    "where host_id = @host_uuid and chart_id not in (select chart_id from chart_active) order by chart_id asc;"

void sql_rrdset2json(RRDHOST *host, BUFFER *wb)
{
    //    time_t first_entry_t = 0; //= rrdset_first_entry_t(st);
    //   time_t last_entry_t = 0; //rrdset_last_entry_t(st);
    static char *custom_dashboard_info_js_filename = NULL;
    int rc;

    sqlite3_stmt *res_chart = NULL;
    sqlite3_stmt *res_dim = NULL;
    time_t now = now_realtime_sec();

    rc = sqlite3_prepare_v2(db_meta, SELECT_CHART, -1, &res_chart, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch host archived charts");
        return;
    }

    rc = sqlite3_bind_blob(res_chart, 1, &host->host_uuid, sizeof(host->host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host parameter to fetch archived charts");
        goto failed;
    }

    rc = sqlite3_prepare_v2(db_meta, SELECT_DIMENSION, -1, &res_dim, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch chart archived dimensions");
        goto failed;
    };

    if(unlikely(!custom_dashboard_info_js_filename))
        custom_dashboard_info_js_filename = config_get(CONFIG_SECTION_WEB, "custom dashboard_info.js", "");

    buffer_sprintf(wb, "{\n"
                       "\t\"hostname\": \"%s\""
                       ",\n\t\"version\": \"%s\""
                       ",\n\t\"release_channel\": \"%s\""
                       ",\n\t\"os\": \"%s\""
                       ",\n\t\"timezone\": \"%s\""
                       ",\n\t\"update_every\": %d"
                       ",\n\t\"history\": %ld"
                       ",\n\t\"memory_mode\": \"%s\""
                       ",\n\t\"custom_info\": \"%s\""
                       ",\n\t\"charts\": {"
        , host->hostname
        , host->program_version
        , get_release_channel()
        , host->os
        , host->timezone
        , host->rrd_update_every
        , host->rrd_history_entries
        , rrd_memory_mode_name(host->rrd_memory_mode)
        , custom_dashboard_info_js_filename
    );

    size_t c = 0;
    size_t dimensions = 0;

    while (sqlite3_step(res_chart) == SQLITE_ROW) {
        char id[512];
        sprintf(id, "%s.%s", sqlite3_column_text(res_chart, 3), sqlite3_column_text(res_chart, 1));
        RRDSET *st = rrdset_find(host, id);
        if (st && !rrdset_flag_check(st, RRDSET_FLAG_ARCHIVED))
            continue;

        if (c)
            buffer_strcat(wb, ",\n\t\t\"");
        else
            buffer_strcat(wb, "\n\t\t\"");
        c++;

        buffer_strcat(wb, id);
        buffer_strcat(wb, "\": ");

        buffer_sprintf(
            wb,
            "\t\t{\n"
            "\t\t\t\"id\": \"%s\",\n"
            "\t\t\t\"name\": \"%s\",\n"
            "\t\t\t\"type\": \"%s\",\n"
            "\t\t\t\"family\": \"%s\",\n"
            "\t\t\t\"context\": \"%s\",\n"
            "\t\t\t\"title\": \"%s (%s)\",\n"
            "\t\t\t\"priority\": %ld,\n"
            "\t\t\t\"plugin\": \"%s\",\n"
            "\t\t\t\"module\": \"%s\",\n"
            "\t\t\t\"enabled\": %s,\n"
            "\t\t\t\"units\": \"%s\",\n"
            "\t\t\t\"data_url\": \"/api/v1/data?chart=%s\",\n"
            "\t\t\t\"chart_type\": \"%s\",\n",
            id //sqlite3_column_text(res_chart, 1)
            ,
            id // sqlite3_column_text(res_chart, 2)
            ,
            sqlite3_column_text(res_chart, 3), sqlite3_column_text(res_chart, 4), sqlite3_column_text(res_chart, 5),
            sqlite3_column_text(res_chart, 6), id //sqlite3_column_text(res_chart, 2)
            ,
            (long ) sqlite3_column_int(res_chart, 7),
            (const char *) sqlite3_column_text(res_chart, 8) ? (const char *) sqlite3_column_text(res_chart, 8) : (char *) "",
            (const char *) sqlite3_column_text(res_chart, 9) ? (const char *) sqlite3_column_text(res_chart, 9) : (char *) "", (char *) "false",
            (const char *) sqlite3_column_text(res_chart, 10), id //sqlite3_column_text(res_chart, 2)
            ,
            rrdset_type_name(sqlite3_column_int(res_chart, 11)));

        sql_rrdim2json(res_dim, (uuid_t *) sqlite3_column_blob(res_chart, 0), wb, &dimensions);

        rc = sqlite3_reset(res_dim);
        if (unlikely(rc != SQLITE_OK))
            error_report("Failed to reset the prepared statement when reading archived chart dimensions");
        buffer_strcat(wb, "\n\t\t}");
    }

    buffer_sprintf(wb
        , "\n\t}"
          ",\n\t\"charts_count\": %zu"
          ",\n\t\"dimensions_count\": %zu"
          ",\n\t\"alarms_count\": %zu"
          ",\n\t\"rrd_memory_bytes\": %zu"
          ",\n\t\"hosts_count\": %zu"
          ",\n\t\"hosts\": ["
        , c
        , dimensions
        , (size_t) 0
        , (size_t) 0
        , rrd_hosts_available
    );

    if(unlikely(rrd_hosts_available > 1)) {
        rrd_rdlock();

        size_t found = 0;
        RRDHOST *h;
        rrdhost_foreach_read(h) {
            if(!rrdhost_should_be_removed(h, host, now) && !rrdhost_flag_check(h, RRDHOST_FLAG_ARCHIVED)) {
                buffer_sprintf(wb
                    , "%s\n\t\t{"
                      "\n\t\t\t\"hostname\": \"%s\""
                      "\n\t\t}"
                    , (found > 0) ? "," : ""
                    , h->hostname
                );

                found++;
            }
        }

        rrd_unlock();
    }
    else {
        buffer_sprintf(wb
            , "\n\t\t{"
              "\n\t\t\t\"hostname\": \"%s\""
              "\n\t\t}"
            , host->hostname
        );
    }

    buffer_sprintf(wb, "\n\t]\n}\n");

    rc = sqlite3_finalize(res_dim);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when reading archived chart dimensions");

failed:
    rc = sqlite3_finalize(res_chart);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when reading archived charts");

    return;
}

#define SELECT_HOST "select host_id, registry_hostname, update_every, os, timezone, tags from host where hostname = @hostname order by rowid desc;"
#define SELECT_HOST_BY_UUID "select host_id, registry_hostname, update_every, os, timezone, tags from host where host_id = @host_id ;"

RRDHOST *sql_create_host_by_uuid(char *hostname)
{
    int rc;
    RRDHOST *host = NULL;
    uuid_t host_uuid;

    sqlite3_stmt *res = NULL;

    rc = uuid_parse(hostname, host_uuid);
    if (!rc) {
        rc = sqlite3_prepare_v2(db_meta, SELECT_HOST_BY_UUID, -1, &res, 0);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to fetch host by uuid");
            return NULL;
        }
        rc = sqlite3_bind_blob(res, 1, &host_uuid, sizeof(host_uuid), SQLITE_STATIC);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to bind host_id parameter to fetch host information");
            goto failed;
        }
    }
    else {
        rc = sqlite3_prepare_v2(db_meta, SELECT_HOST, -1, &res, 0);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to fetch host by hostname");
            return NULL;
        }
        rc = sqlite3_bind_text(res, 1, hostname, -1, SQLITE_STATIC);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to bind hostname parameter to fetch host information");
            goto failed;
        }
    }

    rc = sqlite3_step(res);
    if (unlikely(rc != SQLITE_ROW)) {
        error_report("Failed to find hostname %s", hostname);
        goto failed;
    }

    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(*((uuid_t *) sqlite3_column_blob(res, 0)), uuid_str);

    host = callocz(1, sizeof(RRDHOST));

    set_host_properties(host, sqlite3_column_int(res, 2), RRD_MEMORY_MODE_DBENGINE, hostname,
                        (char *) sqlite3_column_text(res, 1), (const char *) uuid_str,
                        (char *) sqlite3_column_text(res, 3), (char *) sqlite3_column_text(res, 5),
                        (char *) sqlite3_column_text(res, 4), NULL, NULL);

    uuid_copy(host->host_uuid, *((uuid_t *) sqlite3_column_blob(res, 0)));

    host->system_info = callocz(1, sizeof(*host->system_info));;
    rrdhost_flag_set(host, RRDHOST_FLAG_ARCHIVED);
#ifdef ENABLE_DBENGINE
    host->rrdeng_ctx = &multidb_ctx;
#endif

failed:
    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when reading host information");

    return host;
}

void db_execute(const char *cmd)
{
    int rc;
    char *err_msg;
    rc = sqlite3_exec(db_meta, cmd, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        error_report("Failed to execute '%s', rc = %d (%s)", cmd, rc, err_msg);
        sqlite3_free(err_msg);
    }

    return;
}

void db_lock(void)
{
    uv_mutex_lock(&sqlite_transaction_lock);
    return;
}

void db_unlock(void)
{
    uv_mutex_unlock(&sqlite_transaction_lock);
    return;
}


#define SELECT_MIGRATED_FILE    "select 1 from metadata_migration where filename = @path;"

int file_is_migrated(char *path)
{
    sqlite3_stmt *res = NULL;
    int rc;

    rc = sqlite3_prepare_v2(db_meta, SELECT_MIGRATED_FILE, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch host");
        return 0;
    }

    rc = sqlite3_bind_text(res, 1, path, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind filename parameter to check migration");
        return 0;
    }

    rc = sqlite3_step(res);

    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when checking if metadata file is migrated");

    return (rc == SQLITE_ROW);
}

#define STORE_MIGRATED_FILE    "insert or replace into metadata_migration (filename, file_size, date_created) " \
                                "values (@file, @size, strftime('%s'));"

void add_migrated_file(char *path, uint64_t file_size)
{
    sqlite3_stmt *res = NULL;
    int rc;

    rc = sqlite3_prepare_v2(db_meta, STORE_MIGRATED_FILE, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch host");
        return;
    }

    rc = sqlite3_bind_text(res, 1, path, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind filename parameter to store migration information");
        return;
    }

    rc = sqlite3_bind_int64(res, 2, file_size);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind size parameter to store migration information");
        return;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store migrated file, rc = %d", rc);

    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when checking if metadata file is migrated");

    return;
}

#define SQL_INS_CHART_LABEL "insert or replace into chart_label " \
    "(chart_id, source_type, label_key, label_value, date_created) " \
    "values (@chart, @source, @label, @value, strftime('%s'));"

void sql_store_chart_label(uuid_t *chart_uuid, int source_type, char *label, char *value)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_INS_CHART_LABEL, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement store chart labels");
        return;
    }

    rc = sqlite3_bind_blob(res, 1, chart_uuid, sizeof(*chart_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind chart_id parameter to store label information");
        goto failed;
    }

    rc = sqlite3_bind_int(res, 2, source_type);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind type parameter to store label information");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 3, label, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind label parameter to store label information");
        goto failed;
    }

    rc = sqlite3_bind_text(res, 4, value, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind value parameter to store label information");
        goto failed;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store chart label entry, rc = %d", rc);

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when storing chart label information");

    return;
}

int find_dimension_first_last_t(char *machine_guid, char *chart_id, char *dim_id,
                                uuid_t *uuid, time_t *first_entry_t, time_t *last_entry_t, uuid_t *rrdeng_uuid)
{
#ifdef ENABLE_DBENGINE
    int rc;
    uuid_t  legacy_uuid;
    uuid_t  multihost_legacy_uuid;
    time_t dim_first_entry_t, dim_last_entry_t;

    rc = rrdeng_metric_latest_time_by_uuid(uuid, &dim_first_entry_t, &dim_last_entry_t);
    if (unlikely(rc)) {
        rrdeng_generate_legacy_uuid(dim_id, chart_id, &legacy_uuid);
        rc = rrdeng_metric_latest_time_by_uuid(&legacy_uuid, &dim_first_entry_t, &dim_last_entry_t);
        if (likely(rc)) {
            rrdeng_convert_legacy_uuid_to_multihost(machine_guid, &legacy_uuid, &multihost_legacy_uuid);
            rc = rrdeng_metric_latest_time_by_uuid(&multihost_legacy_uuid, &dim_first_entry_t, &dim_last_entry_t);
            if (likely(!rc))
                uuid_copy(*rrdeng_uuid, multihost_legacy_uuid);
        }
        else
            uuid_copy(*rrdeng_uuid, legacy_uuid);
    }
    else
        uuid_copy(*rrdeng_uuid, *uuid);

    if (likely(!rc)) {
        *first_entry_t = MIN(*first_entry_t, dim_first_entry_t);
        *last_entry_t = MAX(*last_entry_t, dim_last_entry_t);
    }
    return rc;
#else
    UNUSED(machine_guid);
    UNUSED(chart_id);
    UNUSED(dim_id);
    UNUSED(uuid);
    UNUSED(first_entry_t);
    UNUSED(last_entry_t);
    UNUSED(rrdeng_uuid);
    return 1;
#endif
}

#ifdef ENABLE_DBENGINE
static RRDDIM *create_rrdim_entry(RRDSET *st, char *id, char *name, uuid_t *metric_uuid)
{
    RRDDIM *rd = callocz(1, sizeof(*rd));
    rd->rrdset = st;
    rd->last_stored_value = NAN;
    rrddim_flag_set(rd, RRDDIM_FLAG_NONE);
    rd->state = mallocz(sizeof(*rd->state));
    rd->rrd_memory_mode = RRD_MEMORY_MODE_DBENGINE;
    rd->state->query_ops.init = rrdeng_load_metric_init;
    rd->state->query_ops.next_metric = rrdeng_load_metric_next;
    rd->state->query_ops.is_finished = rrdeng_load_metric_is_finished;
    rd->state->query_ops.finalize = rrdeng_load_metric_finalize;
    rd->state->query_ops.latest_time = rrdeng_metric_latest_time;
    rd->state->query_ops.oldest_time = rrdeng_metric_oldest_time;
    rd->state->rrdeng_uuid = mallocz(sizeof(uuid_t));
    uuid_copy(*rd->state->rrdeng_uuid, *metric_uuid);
    rd->state->metric_uuid = rd->state->rrdeng_uuid;
    rd->id = strdupz(id);
    rd->name = strdupz(name);
    return rd;
}
#endif

#define SELECT_CHART_CONTEXT  "select d.dim_id, d.id, d.name, c.id, c.type, c.name, c.update_every, c.chart_id from chart c, " \
    "dimension d, host h " \
    "where d.chart_id = c.chart_id and c.host_id = h.host_id and c.host_id = @host_id and c.context = @context " \
    "order by c.chart_id asc, c.type||c.id desc;"

#define SELECT_CHART_SINGLE  "select d.dim_id, d.id, d.name, c.id, c.type, c.name, c.update_every, c.chart_id, c.context from chart c, " \
    "dimension d, host h " \
    "where d.chart_id = c.chart_id and c.host_id = h.host_id and c.host_id = @host_id and c.type||'.'||c.id = @chart " \
    "order by c.chart_id asc, c.type||'.'||c.id desc;"

void sql_build_context_param_list(struct context_param **param_list, RRDHOST *host, char *context, char *chart)
{
#ifdef ENABLE_DBENGINE
    int rc;

    if (unlikely(!param_list) || host->rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
        return;

    if (unlikely(!(*param_list))) {
        *param_list = mallocz(sizeof(struct context_param));
        (*param_list)->first_entry_t = LONG_MAX;
        (*param_list)->last_entry_t = 0;
        (*param_list)->rd = NULL;
        (*param_list)->flags = CONTEXT_FLAGS_ARCHIVE;
        if (chart)
            (*param_list)->flags |= CONTEXT_FLAGS_CHART;
        else
            (*param_list)->flags |= CONTEXT_FLAGS_CONTEXT;
    }

    sqlite3_stmt *res = NULL;

    if (context)
        rc = sqlite3_prepare_v2(db_meta, SELECT_CHART_CONTEXT, -1, &res, 0);
    else
        rc = sqlite3_prepare_v2(db_meta, SELECT_CHART_SINGLE, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch host archived charts");
        return;
    }

    rc = sqlite3_bind_blob(res, 1, &host->host_uuid, sizeof(host->host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host parameter to fetch archived charts");
        goto failed;
    }

    if (context)
        rc = sqlite3_bind_text(res, 2, context, -1, SQLITE_STATIC);
    else
        rc = sqlite3_bind_text(res, 2, chart, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host parameter to fetch archived charts");
        goto failed;
    }

    RRDSET *st = NULL;
    char machine_guid[GUID_LEN + 1];
    uuid_unparse_lower(host->host_uuid, machine_guid);
    uuid_t rrdeng_uuid;
    uuid_t chart_id;

    while (sqlite3_step(res) == SQLITE_ROW) {
        char id[512];
        sprintf(id, "%s.%s", sqlite3_column_text(res, 3), sqlite3_column_text(res, 1));

        if (!st || uuid_compare(*(uuid_t *)sqlite3_column_blob(res, 7), chart_id)) {
            if (unlikely(st && !st->counter)) {
                freez(st->context);
                freez((char *) st->name);
                freez(st);
            }
            st = callocz(1, sizeof(*st));
            char n[RRD_ID_LENGTH_MAX + 1];

            snprintfz(
                n, RRD_ID_LENGTH_MAX, "%s.%s", (char *)sqlite3_column_text(res, 4),
                (char *)sqlite3_column_text(res, 3));
            st->name = strdupz(n);
            st->update_every = sqlite3_column_int(res, 6);
            st->counter = 0;
            if (chart) {
                st->context = strdupz((char *)sqlite3_column_text(res, 8));
                strncpyz(st->id, chart, RRD_ID_LENGTH_MAX);
            }
            uuid_copy(chart_id, *(uuid_t *)sqlite3_column_blob(res, 7));
            st->last_entry_t = 0;
            st->rrdhost = host;
        }

        if (unlikely(find_dimension_first_last_t(machine_guid, (char *)st->name, (char *)sqlite3_column_text(res, 1),
                (uuid_t *)sqlite3_column_blob(res, 0), &(*param_list)->first_entry_t, &(*param_list)->last_entry_t,
                &rrdeng_uuid)))
            continue;

        st->counter++;
        st->last_entry_t = MAX(st->last_entry_t, (*param_list)->last_entry_t);

        RRDDIM *rd = create_rrdim_entry(st, (char *)sqlite3_column_text(res, 1), (char *)sqlite3_column_text(res, 2), &rrdeng_uuid);
        rd->next = (*param_list)->rd;
        (*param_list)->rd = rd;
    }
    if (st) {
        if (!st->counter) {
            freez(st->context);
            freez((char *)st->name);
            freez(st);
        }
        else
            if (!st->context && context)
                st->context = strdupz(context);
    }

failed:
    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when reading archived charts");
#else
    UNUSED(param_list);
    UNUSED(host);
    UNUSED(context);
    UNUSED(chart);
#endif
    return;
}


/*
 * Store a chart hash in the database
 */

#define SQL_STORE_CHART_HASH "insert into chart_hash (hash_id, type, id, " \
    "name, family, context, title, unit, plugin, module, priority, last_used) " \
    "values (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11, strftime('%s')) " \
    "on conflict(hash_id) do update set last_used = strftime('%s');"

int sql_store_chart_hash(
    uuid_t *hash_id, const char *type, const char *id, const char *name, const char *family,
    const char *context, const char *title, const char *units, const char *plugin, const char *module, long priority)
{
    static __thread sqlite3_stmt *res = NULL;
    int rc, param = 0;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE)
            return 0;
        error_report("Database has not been initialized");
        return 1;
    }

    if (unlikely(!res)) {
        rc = prepare_statement(db_meta, SQL_STORE_CHART_HASH, &res);
        if (unlikely(rc != SQLITE_OK)) {
            error_report("Failed to prepare statement to store chart, rc = %d", rc);
            return 1;
        }
    }

    param++;
    rc = sqlite3_bind_blob(res, 1, hash_id, sizeof(*hash_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 2, type, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 3, id, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    if (name && *name)
        rc = sqlite3_bind_text(res, 4, name, -1, SQLITE_STATIC);
    else
        rc = sqlite3_bind_null(res, 4);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 5, family, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 6, context, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 7, title, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 8, units, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 9, plugin, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_text(res, 10, module, -1, SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    param++;
    rc = sqlite3_bind_int(res, 11, (int) priority);
    if (unlikely(rc != SQLITE_OK))
        goto bind_fail;

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store chart hash_id, rc = %d", rc);

    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in chart hash_id store function, rc = %d", rc);

    return 0;

    bind_fail:
    error_report("Failed to bind parameter %d to store chart hash_id, rc = %d", param, rc);
    rc = sqlite3_reset(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to reset statement in chart hash_id store function, rc = %d", rc);
    return 1;
}

void compute_chart_hash(RRDSET *st)
{
    EVP_MD_CTX *evpctx;
    unsigned char hash_value[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    char  priority_str[32];

    sprintf(priority_str, "%ld", st->priority);

    evpctx = EVP_MD_CTX_create();
    EVP_DigestInit_ex(evpctx, EVP_sha256(), NULL);
    //EVP_DigestUpdate(evpctx, st->type, strlen(st->type));
    EVP_DigestUpdate(evpctx, st->id, strlen(st->id));
    EVP_DigestUpdate(evpctx, st->name, strlen(st->name));
    EVP_DigestUpdate(evpctx, st->family, strlen(st->family));
    EVP_DigestUpdate(evpctx, st->context, strlen(st->context));
    EVP_DigestUpdate(evpctx, st->title, strlen(st->title));
    EVP_DigestUpdate(evpctx, st->units, strlen(st->units));
    EVP_DigestUpdate(evpctx, st->plugin_name, strlen(st->plugin_name));
    if (st->module_name)
        EVP_DigestUpdate(evpctx, st->module_name, strlen(st->module_name));
    EVP_DigestUpdate(evpctx, priority_str, strlen(priority_str));
    //EVP_DigestUpdate(evpctx, st->chart_type, strlen(st->chart_type));
    EVP_DigestFinal_ex(evpctx, hash_value, &hash_len);
    EVP_MD_CTX_destroy(evpctx);
    fatal_assert(hash_len > sizeof(uuid_t));

    char uuid_str[GUID_LEN + 1];
    uuid_unparse_lower(*((uuid_t *) &hash_value), uuid_str);
    info("Calculating HASH %s for chart %s", uuid_str, st->name);
    uuid_copy(st->state->hash_id, *((uuid_t *) &hash_value));

    (void)sql_store_chart_hash(
        (uuid_t *)&hash_value,
        st->type,
        st->id,
        st->name,
        st->family,
        st->context,
        st->title,
        st->units,
        st->plugin_name,
        st->module_name,
        st->priority);
    return;
}

#define SELECT_CHART_WITH_HASH "select hash_id, id, name, type, family, context, title, plugin, " \
    "module, unit, priority from chart_hash where hash_id = @hash_id;"

void sql_chart_from_hash_id(char *hash_str, BUFFER *wb)
{
    int rc;
    sqlite3_stmt *res_chart = NULL;

    uuid_t hash_id;
    uuid_parse(hash_str, hash_id);

    rc = sqlite3_prepare_v2(db_meta, SELECT_CHART_WITH_HASH, -1, &res_chart, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch chart config with hash");
        return;
    }

    rc = sqlite3_bind_blob(res_chart, 1, &hash_id, sizeof(hash_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host parameter to fetch chart config with hash");
        goto failed;
    }

    buffer_sprintf(wb, "{");

    size_t c = 0;

    while (sqlite3_step(res_chart) == SQLITE_ROW) {
        char id[512];
        sprintf(id, "%s.%s", sqlite3_column_text(res_chart, 3), sqlite3_column_text(res_chart, 1));

        if (c)
            buffer_strcat(wb, ",\n\t\"");
        else
            buffer_strcat(wb, "\n\t\"");
        c++;

        buffer_strcat(wb, id);
        buffer_strcat(wb, "\": ");

        buffer_sprintf(
            wb,
            "\t{\n"
            "\t\t\"id\": \"%s\",\n"
            "\t\t\"name\": \"%s\",\n"
            "\t\t\"type\": \"%s\",\n"
            "\t\t\"family\": \"%s\",\n"
            "\t\t\"context\": \"%s\",\n"
            "\t\t\"title\": \"%s (%s)\",\n"
            "\t\t\"plugin\": \"%s\",\n"
            "\t\t\"module\": \"%s\",\n"
            "\t\t\"units\": \"%s\",\n"
            "\t\t\"priority\": \"%ld\",\n"
            "\t\t\"hash_id\": \"%s\"\n",
            sqlite3_column_text(res_chart, 1),
            sqlite3_column_text(res_chart, 2),
            sqlite3_column_text(res_chart, 3), sqlite3_column_text(res_chart, 4), sqlite3_column_text(res_chart, 5),
            sqlite3_column_text(res_chart, 6), sqlite3_column_text(res_chart, 1) ,
            (const char *) sqlite3_column_text(res_chart, 7) ? (const char *) sqlite3_column_text(res_chart, 7) : (char *) "",
            (const char *) sqlite3_column_text(res_chart,8) ? (const char *) sqlite3_column_text(res_chart, 8) : (char *) "",
            (const char *) sqlite3_column_text(res_chart, 9),
            (long) sqlite3_column_int(res_chart, 10),
            hash_str);

        if (unlikely(rc != SQLITE_OK))
            error_report("Failed to reset the prepared statement when reading chart config with hash");
        buffer_strcat(wb, "\n\t}");
    }

    buffer_sprintf(wb, "\n}\n");

failed:
    rc = sqlite3_finalize(res_chart);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when reading chart config with hash");

    return;
}

#define SQL_STORE_CLAIM_ID  "insert into node_instance " \
    "(host_id, claim_id, date_created) values (@host_id, @claim_id, strftime('%s')) " \
    "on conflict(host_id) do update set claim_id = excluded.claim_id;"

void store_claim_id(uuid_t *host_id, uuid_t *claim_id)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_STORE_CLAIM_ID, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement store chart labels");
        return;
    }

    rc = sqlite3_bind_blob(res, 1, host_id, sizeof(*host_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to store node instance information");
        goto failed;
    }

    if (claim_id)
        rc = sqlite3_bind_blob(res, 2, claim_id, sizeof(*claim_id), SQLITE_STATIC);
    else
        rc = sqlite3_bind_null(res, 2);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind claim_id parameter to store node instance information");
        goto failed;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store node instance information, rc = %d", rc);

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when storing node instance information");

    return;
}

static inline void set_host_node_id(RRDHOST *host, uuid_t *node_id)
{
    if (unlikely(!host))
        return;

    if (unlikely(!node_id)) {
        freez(host->node_id);
        host->node_id = NULL;
        return;
    }

    if (unlikely(!host->node_id))
        host->node_id = mallocz(sizeof(*host->node_id));
    uuid_copy(*(host->node_id), *node_id);
    return;
}

#define SQL_UPDATE_NODE_ID  "update node_instance set node_id = @node_id where host_id = @host_id;"

int update_node_id(uuid_t *host_id, uuid_t *node_id)
{
    sqlite3_stmt *res = NULL;
    RRDHOST *host = NULL;
    int rc = 2;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return 1;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_UPDATE_NODE_ID, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to store node instance information");
        return 1;
    }

    rc = sqlite3_bind_blob(res, 1, node_id, sizeof(*node_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to store node instance information");
        goto failed;
    }

    rc = sqlite3_bind_blob(res, 2, host_id, sizeof(*host_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to store node instance information");
        goto failed;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to store node instance information, rc = %d", rc);
    rc = sqlite3_changes(db_meta);

    char host_guid[GUID_LEN + 1];
    uuid_unparse_lower(*host_id, host_guid);
    rrd_wrlock();
    host = rrdhost_find_by_guid(host_guid, 0);
    if (likely(host))
            set_host_node_id(host, node_id);
    rrd_unlock();

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when storing node instance information");

    return rc - 1;
}

#define SQL_SELECT_NODE_ID  "select node_id from node_instance where host_id = @host_id and node_id not null;"

int get_node_id(uuid_t *host_id, uuid_t *node_id)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return 1;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_SELECT_NODE_ID, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to select node instance information for a host");
        return 1;
    }

    rc = sqlite3_bind_blob(res, 1, host_id, sizeof(*host_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to select node instance information");
        goto failed;
    }

    rc = sqlite3_step(res);
    if (likely(rc == SQLITE_ROW && node_id))
        uuid_copy(*node_id, *((uuid_t *) sqlite3_column_blob(res, 0)));

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when selecting node instance information");

    return (rc == SQLITE_ROW) ? 0 : -1;
}

#define SQL_INVALIDATE_NODE_INSTANCES "update node_instance set node_id = NULL where exists " \
    "(select host_id from node_instance where host_id = @host_id and (@claim_id is null or claim_id <> @claim_id));"

void invalidate_node_instances(uuid_t *host_id, uuid_t *claim_id)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_INVALIDATE_NODE_INSTANCES, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to invalidate node instance ids");
        return;
    }

    rc = sqlite3_bind_blob(res, 1, host_id, sizeof(*host_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to invalidate node instance information");
        goto failed;
    }

    if (claim_id)
        rc = sqlite3_bind_blob(res, 2, claim_id, sizeof(*claim_id), SQLITE_STATIC);
    else
        rc = sqlite3_bind_null(res, 2);

    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind claim_id parameter to invalidate node instance information");
        goto failed;
    }

    rc = execute_insert(res);
    if (unlikely(rc != SQLITE_DONE))
        error_report("Failed to invalidate node instance information, rc = %d", rc);

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when invalidating node instance information");
}

#define SQL_GET_NODE_INSTANCE_LIST "select ni.node_id, ni.host_id, h.hostname " \
    "from node_instance ni, host h where ni.host_id = h.host_id;"

struct  node_instance_list *get_node_list(void)
{
    struct  node_instance_list *node_list = NULL;
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return NULL;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_GET_NODE_INSTANCE_LIST, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement store chart labels");
        return NULL;
    };

    int row = 0;
    char host_guid[37];
    while (sqlite3_step(res) == SQLITE_ROW)
        row++;

    if (sqlite3_reset(res) != SQLITE_OK) {
        error_report("Failed to reset the prepared statement while fetching node instance information");
        goto failed;
    }
    node_list = callocz(row + 1, sizeof(*node_list));
    int max_rows = row;
    row = 0;
    while (sqlite3_step(res) == SQLITE_ROW) {
        if (sqlite3_column_bytes(res, 0) == sizeof(uuid_t))
            uuid_copy(node_list[row].node_id, *((uuid_t *)sqlite3_column_blob(res, 0)));
        if (sqlite3_column_bytes(res, 1) == sizeof(uuid_t)) {
            uuid_t *host_id = (uuid_t *)sqlite3_column_blob(res, 1);
            uuid_copy(node_list[row].host_id, *host_id);
            node_list[row].querable = 1;
            uuid_unparse_lower(*host_id, host_guid);
            node_list[row].live = rrdhost_find_by_guid(host_guid, 0) ? 1 : 0;
            node_list[row].hops = uuid_compare(*host_id, localhost->host_uuid) ? 1 : 0;
            node_list[row].hostname =
                sqlite3_column_bytes(res, 2) ? strdupz((char *)sqlite3_column_text(res, 2)) : NULL;
        }
        row++;
        if (row == max_rows)
            break;
    }

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when fetching node instance information");

    return node_list;
};

#define SQL_GET_HOST_NODE_ID "select node_id from node_instance where host_id = @host_id;"

void sql_load_node_id(RRDHOST *host)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_GET_HOST_NODE_ID, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch node id");
        return;
    };

    rc = sqlite3_bind_blob(res, 1, &host->host_uuid, sizeof(host->host_uuid), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to load node instance information");
        goto failed;
    }

    rc = sqlite3_step(res);
    if (likely(rc == SQLITE_ROW)) {
        if (likely(sqlite3_column_bytes(res, 0) == sizeof(uuid_t)))
            set_host_node_id(host, (uuid_t *)sqlite3_column_blob(res, 0));
        else
            set_host_node_id(host, NULL);
    }

failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when loading node instance information");

    return;
};
