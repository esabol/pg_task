#include "include.h"

extern const char *null;
extern volatile sig_atomic_t sighup;
extern volatile sig_atomic_t sigterm;

static void work_schema(Work *work) {
    StringInfoData buf;
    List *names;
    const char *schema_quote = quote_identifier(work->schema);
    D1("user = %s, data = %s, schema = %s, table = %s", work->user, work->data, work->schema, work->table);
    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE SCHEMA %s", schema_quote);
    names = stringToQualifiedNameList(schema_quote);
    SPI_connect_my(buf.data);
    if (!OidIsValid(get_namespace_oid(strVal(linitial(names)), true))) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    else D1("schema %s already exists", schema_quote);
    SPI_commit_my();
    SPI_finish_my();
    list_free_deep(names);
    if (schema_quote != work->schema) pfree((void *)schema_quote);
    pfree(buf.data);
    set_config_option("pg_task.schema", work->schema, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
}

static void work_type(Work *work) {
    StringInfoData buf;
    Oid type = InvalidOid;
    int32 typmod;
    const char *schema_quote = work->schema ? quote_identifier(work->schema) : NULL;
    D1("user = %s, data = %s, schema = %s, table = %s", work->user, work->data, work->schema ? work->schema : null, work->table);
    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE TYPE %s AS ENUM ('PLAN', 'TAKE', 'WORK', 'DONE', 'FAIL', 'STOP')", work->schema_type);
    SPI_connect_my(buf.data);
    parseTypeString(work->schema_type, &type, &typmod, true);
    if (!OidIsValid(type)) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    else D1("type %s already exists", work->schema_type);
    SPI_commit_my();
    SPI_finish_my();
    if (work->schema && schema_quote && work->schema != schema_quote) pfree((void *)schema_quote);
    pfree(buf.data);
}

static bool work_table(Work *work) {
    StringInfoData buf;
    List *names;
    const RangeVar *rangevar;
    D1("user = %s, data = %s, schema = %s, table = %s, schema_table = %s, schema_type = %s", work->user, work->data, work->schema ? work->schema : null, work->table, work->schema_table, work->schema_type);
    if (work->oid) pg_advisory_unlock_int8_my(work->oid);
    set_config_option("pg_task.table", work->table, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    initStringInfo(&buf);
    appendStringInfo(&buf,
        "CREATE TABLE %1$s (\n"
        "    id bigserial NOT NULL PRIMARY KEY,\n"
        "    parent int8 DEFAULT current_setting('pg_task.id', true)::int8 REFERENCES %1$s (id) MATCH SIMPLE ON UPDATE CASCADE ON DELETE SET NULL,\n"
        "    dt timestamptz NOT NULL DEFAULT current_timestamp,\n"
        "    start timestamptz,\n"
        "    stop timestamptz,\n"
        "    \"group\" text NOT NULL DEFAULT 'group',\n"
        "    max int4,\n"
        "    pid int4,\n"
        "    input text NOT NULL,\n"
        "    output text,\n"
        "    error text,\n"
        "    state %2$s NOT NULL DEFAULT 'PLAN'::%2$s,\n"
        "    timeout interval,\n"
        "    delete boolean NOT NULL DEFAULT false,\n"
        "    repeat interval,\n"
        "    drift boolean NOT NULL DEFAULT true,\n"
        "    count int4,\n"
        "    live interval,\n"
        "    remote text,\n"
        "    append boolean NOT NULL DEFAULT false,\n"
        "    header boolean NOT NULL DEFAULT true,\n"
        "    string boolean NOT NULL DEFAULT true,\n"
        "    \"null\" text NOT NULL DEFAULT '\\N',\n"
        "    delimiter \"char\" NOT NULL DEFAULT '\t',\n"
        "    quote \"char\",\n"
        "    escape \"char\"\n"
        ")", work->schema_table, work->schema_type);
    names = stringToQualifiedNameList(work->schema_table);
    rangevar = makeRangeVarFromNameList(names);
    SPI_connect_my(buf.data);
    if (!OidIsValid(RangeVarGetRelid(rangevar, NoLock, true))) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    else D1("table %s already exists", work->schema_table);
    work->oid = RangeVarGetRelid(rangevar, NoLock, false);
    SPI_commit_my();
    SPI_finish_my();
    pfree((void *)rangevar);
    list_free_deep(names);
    set_config_option("pg_task.table", work->table, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    resetStringInfo(&buf);
    appendStringInfo(&buf, "%i", work->oid);
    set_config_option("pg_task.oid", buf.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    pfree(buf.data);
    if (!pg_try_advisory_lock_int8_my(work->oid)) { W("!pg_try_advisory_lock_int8_my(%i)", work->oid); return true; }
    return false;
}

static void work_index(Work *work, const char *index) {
    StringInfoData buf, name, idx;
    List *names;
    Relation relation;
    const RangeVar *rangevar;
    const char *name_quote;
    const char *index_quote = quote_identifier(index);
    const char *schema_quote = work->schema ? quote_identifier(work->schema) : NULL;
    D1("user = %s, data = %s, schema = %s, table = %s, index = %s, schema_table = %s", work->user, work->data, work->schema ? work->schema : null, work->table, index, work->schema_table);
    initStringInfo(&name);
    appendStringInfo(&name, "%s_%s_idx", work->table, index);
    name_quote = quote_identifier(name.data);
    initStringInfo(&buf);
    appendStringInfo(&buf, "CREATE INDEX %s ON %s USING btree (%s)", name_quote, work->schema_table, index_quote);
    initStringInfo(&idx);
    if (work->schema) appendStringInfo(&idx, "%s.", schema_quote);
    appendStringInfoString(&idx, name_quote);
    names = stringToQualifiedNameList(idx.data);
    rangevar = makeRangeVarFromNameList(names);
    SPI_connect_my(buf.data);
    if (!OidIsValid(RangeVarGetRelid(rangevar, NoLock, true))) {
        SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
    } else if ((relation = relation_openrv_extended(rangevar, NoLock, true))) {
        if (relation->rd_index && relation->rd_index->indrelid != work->oid) SPI_execute_with_args_my(buf.data, 0, NULL, NULL, NULL, SPI_OK_UTILITY, false);
        else D1("index %s already exists", idx.data);
        relation_close(relation, NoLock);
    } else {
        D1("index %s already exists", idx.data);
    }
    SPI_commit_my();
    SPI_finish_my();
    pfree((void *)rangevar);
    list_free_deep(names);
    pfree(buf.data);
    pfree(name.data);
    pfree(idx.data);
    if (work->schema && schema_quote && work->schema != schema_quote) pfree((void *)schema_quote);
    if (name_quote != name.data) pfree((void *)name_quote);
    if (index_quote != index) pfree((void *)index_quote);
}

static void work_free(Task *task) {
    if (task->group) pfree(task->group);
    if (task->null) pfree(task->null);
    if (task->remote) pfree(task->remote);
    if (task->input) pfree(task->input);
    if (task->output.data) pfree(task->output.data);
    if (task->error.data) pfree(task->error.data);
    pfree(task);
}

static void work_finish(Task *task) {
    queue_remove(&task->queue);
    PQfinish(task->conn);
    work_free(task);
}

static void work_error(Task *task, const char *msg) {
    char *err = PQerrorMessage(task->conn);
    int len = strlen(err);
    initStringInfo(&task->output);
    appendStringInfoString(&task->output, msg);
    if (len) appendStringInfo(&task->output, " and %.*s", len - 1, err);
    W(task->output.data);
    task->fail = true;
    task_done(task);
    work_finish(task);
}

static void work_error2(Task *task, const char *msg, const char *err) {
    initStringInfo(&task->output);
    appendStringInfoString(&task->output, msg);
    if (err) {
        int len = strlen(err);
        if (len) appendStringInfo(&task->output, " and %.*s", len - 1, err);
    }
    W(task->output.data);
    task->fail = true;
    task_done(task);
    work_free(task);
}

static void work_remote(Work *work, const int64 id, char *group, char *remote, const int max) {
    const char **keywords;
    const char **values;
    StringInfoData buf, buf2;
    int arg = 3;
    char *err;
    char *options = NULL;
    bool password = false;
    PQconninfoOption *opts = PQconninfoParse(remote, &err);
    MemoryContext oldMemoryContext = MemoryContextSwitchTo(TopMemoryContext);
    Task *task = palloc0(sizeof(*task));
    task->group = pstrdup(group);
    task->remote = pstrdup(remote);
    MemoryContextSwitchTo(oldMemoryContext);
    task->id = id;
    task->max = max;
    task->work = work;
    D1("id = %li, group = %s, remote = %s, max = %i, oid = %i", task->id, task->group, task->remote ? task->remote : null, task->max, work->oid);
    if (!opts) { work_error2(task, "!PQconninfoParse", err); if (err) PQfreemem(err); return; }
    for (PQconninfoOption *opt = opts; opt->keyword; opt++) {
        if (!opt->val) continue;
        D1("%s = %s", opt->keyword, opt->val);
        if (!pg_strcasecmp(opt->keyword, "password")) password = true;
        if (!pg_strcasecmp(opt->keyword, "fallback_application_name")) continue;
        if (!pg_strcasecmp(opt->keyword, "application_name")) continue;
        if (!pg_strcasecmp(opt->keyword, "options")) { options = opt->val; continue; }
        arg++;
    }
    if (!superuser() && !password) { work_error2(task, "!superuser && !password", NULL); return; }
    keywords = palloc(arg * sizeof(*keywords));
    values = palloc(arg * sizeof(*values));
    initStringInfo(&buf);
    appendStringInfo(&buf, "pg_task %s%s%s %s", work->schema ? work->schema : "", work->schema ? " " : "", work->table, group);
    arg = 0;
    keywords[arg] = "application_name";
    values[arg] = buf.data;
    initStringInfo(&buf2);
    if (options) appendStringInfoString(&buf2, options);
    appendStringInfo(&buf2, "%s-c pg_task.data=%s", buf2.len ? " " : "", work->data);
    appendStringInfo(&buf2, " -c pg_task.user=%s", work->user);
    if (work->schema) appendStringInfo(&buf2, " -c pg_task.schema=%s", work->schema);
    appendStringInfo(&buf2, " -c pg_task.table=%s", work->table);
    appendStringInfo(&buf2, " -c pg_task.oid=%i", work->oid);
    appendStringInfo(&buf2, " -c pg_task.group=%s", group);
    arg++;
    keywords[arg] = "options";
    values[arg] = buf2.data;
    for (PQconninfoOption *opt = opts; opt->keyword; opt++) {
        if (!opt->val) continue;
        if (!pg_strcasecmp(opt->keyword, "fallback_application_name")) continue;
        if (!pg_strcasecmp(opt->keyword, "application_name")) continue;
        if (!pg_strcasecmp(opt->keyword, "options")) continue;
        arg++;
        keywords[arg] = opt->keyword;
        values[arg] = opt->val;
    }
    arg++;
    keywords[arg] = NULL;
    values[arg] = NULL;
    task->events = WL_SOCKET_WRITEABLE;
    task->start = GetCurrentTimestamp();
    queue_insert_tail(&work->queue, &task->queue);
    if (!(task->conn = PQconnectStartParams(keywords, values, false))) work_error(task, "!PQconnectStartParams"); else
    if (PQstatus(task->conn) == CONNECTION_BAD) work_error(task, "PQstatus == CONNECTION_BAD"); else
    if (!PQisnonblocking(task->conn) && PQsetnonblocking(task->conn, true) == -1) work_error(task, "PQsetnonblocking == -1"); else
    if ((task->fd = PQsocket(task->conn)) < 0) work_error(task, "PQsocket < 0"); else
    if (!superuser() && !PQconnectionUsedPassword(task->conn)) work_error(task, "!superuser && !PQconnectionUsedPassword"); else
    if (PQclientEncoding(task->conn) != GetDatabaseEncoding()) PQsetClientEncoding(task->conn, GetDatabaseEncodingName());
    pfree(buf.data);
    pfree(buf2.data);
    pfree(keywords);
    pfree(values);
    PQconninfoFree(opts);
}

static void work_task(const Work *work, const int64 id, char *group, const int max) {
    BackgroundWorkerHandle *handle;
    pid_t pid;
    StringInfoData buf;
    int user_len = strlen(work->user), data_len = strlen(work->data), schema_len = work->schema ? strlen(work->schema) : 0, table_len = strlen(work->table), group_len = strlen(group), max_len = sizeof(max), oid_len = sizeof(work->oid);
    BackgroundWorker worker;
    char *p = worker.bgw_extra;
    D1("user = %s, data = %s, schema = %s, table = %s, id = %li, group = %s, max = %i, oid = %i", work->user, work->data, work->schema ? work->schema : null, work->table, id, group, max, work->oid);
    MemSet(&worker, 0, sizeof(worker));
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_main_arg = id;
    worker.bgw_notify_pid = MyProcPid;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    initStringInfo(&buf);
    appendStringInfoString(&buf, "pg_task");
    if (buf.len + 1 > BGW_MAXLEN) E("%i > BGW_MAXLEN", buf.len + 1);
    memcpy(worker.bgw_library_name, buf.data, buf.len);
    resetStringInfo(&buf);
    appendStringInfoString(&buf, "task_worker");
    if (buf.len + 1 > BGW_MAXLEN) E("%i > BGW_MAXLEN", buf.len + 1);
    memcpy(worker.bgw_function_name, buf.data, buf.len);
    resetStringInfo(&buf);
    appendStringInfo(&buf, "pg_task %s%s%s %s", work->schema ? work->schema : "", work->schema ? " " : "", work->table, group);
    if (buf.len + 1 > BGW_MAXLEN) E("%i > BGW_MAXLEN", buf.len + 1);
    memcpy(worker.bgw_type, buf.data, buf.len);
    resetStringInfo(&buf);
    appendStringInfo(&buf, "%s %s %s", work->user, work->data, worker.bgw_type);
    if (buf.len + 1 > BGW_MAXLEN) E("%i > BGW_MAXLEN", buf.len + 1);
    memcpy(worker.bgw_name, buf.data, buf.len);
    pfree(buf.data);
    if (user_len + 1 + data_len + 1 + schema_len + 1 + table_len + 1 + oid_len + group_len + 1 + max_len > BGW_EXTRALEN) E("%i > BGW_EXTRALEN", user_len + 1 + data_len + 1 + schema_len + 1 + table_len + 1 + oid_len + group_len + 1 + max_len);
    p = (char *)memcpy(p, work->user, user_len) + user_len + 1;
    p = (char *)memcpy(p, work->data, data_len) + data_len + 1;
    p = (char *)memcpy(p, work->schema, schema_len) + schema_len + 1;
    p = (char *)memcpy(p, work->table, table_len) + table_len + 1;
    p = (char *)memcpy(p, &work->oid, oid_len) + oid_len;
    p = (char *)memcpy(p, group, group_len) + group_len + 1;
    p = (char *)memcpy(p, &max, max_len) + max_len;
    if (!RegisterDynamicBackgroundWorker(&worker, &handle)) E("!RegisterDynamicBackgroundWorker");
    switch (WaitForBackgroundWorkerStartup(handle, &pid)) {
        case BGWH_NOT_YET_STARTED: E("WaitForBackgroundWorkerStartup == BGWH_NOT_YET_STARTED"); break;
        case BGWH_POSTMASTER_DIED: E("WaitForBackgroundWorkerStartup == BGWH_POSTMASTER_DIED"); break;
        case BGWH_STARTED: break;
        case BGWH_STOPPED: E("WaitForBackgroundWorkerStartup == BGWH_STOPPED"); break;
    }
    pfree(handle);
}

static void work_update(Work *work) {
    static SPI_plan *plan = NULL;
    static char *command = NULL;
    if (!command) {
        StringInfoData buf;
        initStringInfo(&buf);
        appendStringInfo(&buf,
            "WITH s AS (SELECT id FROM %1$s AS t WHERE dt < current_timestamp - concat_ws(' ', (current_setting('pg_task.reset', false)::int4 * current_setting('pg_task.timeout', false)::int4)::text, 'msec')::interval AND state IN ('TAKE'::%2$s, 'WORK'::%2$s) AND pid NOT IN (\n"
            "    SELECT  pid\n"
            "    FROM    pg_stat_activity\n"
            "    WHERE   datname = current_catalog\n"
            "    AND     usename = current_user\n"
            "    AND     application_name = concat_ws(' ', 'pg_task', current_setting('pg_task.schema', true), current_setting('pg_task.table', false), \"group\")\n"
            ") FOR UPDATE SKIP LOCKED) UPDATE %1$s AS u SET state = 'PLAN'::%2$s FROM s WHERE u.id = s.id", work->schema_table, work->schema_type);
        command = buf.data;
    }
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, 0, NULL);
    SPI_execute_plan_my(plan, NULL, NULL, SPI_OK_UPDATE, true);
    SPI_finish_my();
}

void work_timeout(Work *work) {
    static SPI_plan *plan = NULL;
    static char *command = NULL;
    work_update(work);
    if (!command) {
        StringInfoData buf;
        initStringInfo(&buf);
        appendStringInfo(&buf,
            "WITH s AS (WITH s AS (WITH s AS (WITH s AS (WITH s AS (\n"
            "SELECT      id, \"group\", COALESCE(max, ~(1<<31)) AS max, a.pid\n"
            "FROM        %1$s AS t\n"
            "LEFT JOIN   pg_stat_activity AS a\n"
            "ON          datname = current_catalog\n"
            "AND         usename = current_user\n"
            "AND         application_name = concat_ws(' ', 'pg_task', current_setting('pg_task.schema', true), current_setting('pg_task.table', false), \"group\")\n"
            "WHERE       t.state = 'PLAN'::%2$s\n"
            "AND         dt + concat_ws(' ', (CASE WHEN max < 0 THEN -max ELSE 0 END)::text, 'msec')::interval <= current_timestamp\n"
            ") SELECT id, \"group\", CASE WHEN max > 0 THEN max ELSE 1 END - count(pid) AS count FROM s GROUP BY id, \"group\", max\n"
            ") SELECT array_agg(id ORDER BY id) AS id, \"group\", count FROM s WHERE count > 0 GROUP BY \"group\", count\n"
            ") SELECT unnest(id[:count]) AS id, \"group\", count FROM s ORDER BY count DESC\n"
            ") SELECT s.* FROM s INNER JOIN %1$s USING (id) FOR UPDATE SKIP LOCKED\n"
            ") UPDATE %1$s AS u SET state = 'TAKE'::%2$s FROM s WHERE u.id = s.id RETURNING u.id, u.group, u.remote, COALESCE(u.max, ~(1<<31)) AS max", work->schema_table, work->schema_type);
        command = buf.data;
    }
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, 0, NULL);
    SPI_execute_plan_my(plan, NULL, NULL, SPI_OK_UPDATE_RETURNING, true);
    for (uint64 row = 0; row < SPI_processed; row++) {
        int64 id = DatumGetInt64(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "id", false));
        int max = DatumGetInt32(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "max", false));
        MemoryContext oldMemoryContext = MemoryContextSwitchTo(TopMemoryContext);
        char *group = TextDatumGetCStringMy(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "group", false));
        char *remote = TextDatumGetCStringMy(SPI_getbinval_my(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, "remote", true));
        MemoryContextSwitchTo(oldMemoryContext);
        D1("row = %lu, id = %li, group = %s, remote = %s, max = %i", row, id, group, remote ? remote : null, max);
        if (remote) work_remote(work, id, group, remote, max); else work_task(work, id, group, max);
        pfree(group);
        if (remote) pfree(remote);
    }
    SPI_finish_my();
}

static bool work_check(void) {
    bool exit = false;
    static SPI_plan *plan = NULL;
    static const char *command =
        "WITH s AS ("
        "SELECT      COALESCE(COALESCE(usename, \"user\"), data)::text AS user,\n"
        "            COALESCE(datname, data)::text AS data,\n"
        "            schema,\n"
        "            COALESCE(\"table\", current_setting('pg_task.default_table', false)) AS table,\n"
        "            COALESCE(reset, current_setting('pg_task.default_reset', false)::int4) AS reset,\n"
        "            COALESCE(timeout, current_setting('pg_task.default_timeout', false)::int4) AS timeout\n"
        "FROM        json_populate_recordset(NULL::record, current_setting('pg_task.json', false)::json) AS s (\"user\" text, data text, schema text, \"table\" text, reset int4, timeout int4)\n"
        "LEFT JOIN   pg_database AS d ON (data IS NULL OR datname = data) AND NOT datistemplate AND datallowconn\n"
        "LEFT JOIN   pg_user AS u ON usename = COALESCE(COALESCE(\"user\", (SELECT usename FROM pg_user WHERE usesysid = datdba)), data)\n"
        ") SELECT DISTINCT * FROM s WHERE \"user\" = current_user AND data = current_catalog AND schema IS NOT DISTINCT FROM current_setting('pg_task.schema', true) AND \"table\" = current_setting('pg_task.table', false) AND reset = current_setting('pg_task.reset', false)::int4 AND timeout = current_setting('pg_task.timeout', false)::int4";
    SPI_connect_my(command);
    if (!plan) plan = SPI_prepare_my(command, 0, NULL);
    SPI_execute_plan_my(plan, NULL, NULL, SPI_OK_SELECT, true);
    if (!SPI_processed) exit = true;
    SPI_finish_my();
    return exit;
}

static void work_init_conf(Work *work) {
    char *p = MyBgworkerEntry->bgw_extra;
    work->user = p;
    p += strlen(work->user) + 1;
    work->data = p;
    p += strlen(work->data) + 1;
    work->schema = p;
    p += strlen(work->schema) + 1;
    work->table = p;
    p += strlen(work->table) + 1;
    work->reset = *(typeof(work->reset) *)p;
    p += sizeof(work->reset);
    work->timeout = *(typeof(work->timeout) *)p;
    if (work->table == work->schema + 1) work->schema = NULL;
    if (!MyProcPort && !(MyProcPort = (Port *) calloc(1, sizeof(Port)))) E("!calloc");
    if (!MyProcPort->remote_host) MyProcPort->remote_host = "[local]";
    if (!MyProcPort->user_name) MyProcPort->user_name = work->user;
    if (!MyProcPort->database_name) MyProcPort->database_name = work->data;
    null = GetConfigOption("pg_task.null", false, true);
    set_config_option("application_name", MyBgworkerEntry->bgw_type, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    D1("user = %s, data = %s, schema = %s, table = %s, reset = %i, timeout = %i", work->user, work->data, work->schema ? work->schema : null, work->table, work->reset, work->timeout);
    pqsignal(SIGHUP, init_sighup);
    pqsignal(SIGTERM, init_sigterm);
    BackgroundWorkerUnblockSignals();
    BackgroundWorkerInitializeConnection(work->data, work->user, 0);
    pgstat_report_appname(MyBgworkerEntry->bgw_type);
    process_session_preload_libraries();
}

bool work_init(Work *work) {
    bool exit = false;
    const char *schema_quote = work->schema ? quote_identifier(work->schema) : NULL;
    const char *table_quote = quote_identifier(work->table);
    StringInfoData buf;
    MemoryContext oldMemoryContext = MemoryContextSwitchTo(TopMemoryContext);
    initStringInfo(&buf);
    MemoryContextSwitchTo(oldMemoryContext);
    if (work->schema) appendStringInfo(&buf, "%s.", schema_quote);
    appendStringInfoString(&buf, table_quote);
    if (work->schema_table) pfree(work->schema_table);
    work->schema_table = buf.data;
    oldMemoryContext = MemoryContextSwitchTo(TopMemoryContext);
    initStringInfo(&buf);
    MemoryContextSwitchTo(oldMemoryContext);
    if (work->schema) appendStringInfo(&buf, "%s.", schema_quote);
    appendStringInfoString(&buf, "state");
    if (work->schema_type) pfree(work->schema_type);
    work->schema_type = buf.data;
    if (work->schema && schema_quote && work->schema != schema_quote) pfree((void *)schema_quote);
    if (work->table != table_quote) pfree((void *)table_quote);
    D1("user = %s, data = %s, schema = %s, table = %s, reset = %i, timeout = %i, schema_table = %s, schema_table = %s", work->user, work->data, work->schema ? work->schema : null, work->table, work->reset, work->timeout, work->schema_table, work->schema_type);
    if (work->schema) work_schema(work);
    work_type(work);
    if (work_table(work)) return true;
    work_index(work, "dt");
    work_index(work, "state");
    set_config_option("pg_task.data", work->data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    set_config_option("pg_task.user", work->user, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    initStringInfo(&buf);
    appendStringInfo(&buf, "%i", work->reset);
    set_config_option("pg_task.reset", buf.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    resetStringInfo(&buf);
    appendStringInfo(&buf, "%i", work->timeout);
    set_config_option("pg_task.timeout", buf.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    pfree(buf.data);
    queue_init(&work->queue);
    return exit;
}

static bool work_reload(void) {
    sighup = false;
    ProcessConfigFile(PGC_SIGHUP);
    return work_check();
}

static bool work_latch(void) {
    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();
    if (sighup) return work_reload();
    return false;
}

static void work_command(Task *task, PGresult *result) {
    if (task->skip) { task->skip--; return; }
    if (!task->output.data) initStringInfo(&task->output);
    appendStringInfo(&task->output, "%s%s", task->output.len ? "\n" : "", PQcmdStatus(result));
}

static void work_success(Task *task, PGresult *result) {
    if (task->length == 1 && !PQntuples(result)) return;
    if (!task->output.data) initStringInfo(&task->output);
    if (task->header && (task->length > 1 || PQnfields(result) > 1)) {
        if (task->output.len) appendStringInfoString(&task->output, "\n");
        for (int col = 0; col < PQnfields(result); col++) {
            const char *value = PQfname(result, col);
            if (col > 0) appendStringInfoChar(&task->output, task->delimiter);
            if (task->quote) appendStringInfoChar(&task->output, task->quote);
            if (task->escape) init_escape(&task->output, value, strlen(value), task->escape);
            else appendStringInfoString(&task->output, value);
            if (task->append && !strstr(value, "::")) {
                Oid oid = PQftype(result, col);
                const char *type = PQftypeMy(oid);
                if (task->escape) init_escape(&task->output, "::", sizeof("::") - 1, task->escape);
                else appendStringInfoString(&task->output, "::");
                if (type) {
                    if (task->escape) init_escape(&task->output, type, strlen(type), task->escape);
                    else appendStringInfoString(&task->output, type);
                } else appendStringInfo(&task->output, "%i", oid);
            }
            if (task->quote) appendStringInfoChar(&task->output, task->quote);
        }
    }
    for (int row = 0; row < PQntuples(result); row++) {
        if (task->output.len) appendStringInfoString(&task->output, "\n");
        for (int col = 0; col < PQnfields(result); col++) {
            const char *value = PQgetvalue(result, row, col);
            int len = PQgetlength(result, row, col);
            if (col > 0) appendStringInfoChar(&task->output, task->delimiter);
            if (PQgetisnull(result, row, col)) appendStringInfoString(&task->output, task->null); else {
                if (!init_oid_is_string(PQftype(result, col)) && task->string) {
                    if (len) appendStringInfoString(&task->output, value);
                } else {
                    if (task->quote) appendStringInfoChar(&task->output, task->quote);
                    if (len) {
                        if (task->escape) init_escape(&task->output, value, len, task->escape);
                        else appendStringInfoString(&task->output, value);
                    }
                    if (task->quote) appendStringInfoChar(&task->output, task->quote);
                }
            }
        }
    }
}

static void work_fail(Task *task, PGresult *result) {
    char *value;
    if (!task->output.data) initStringInfo(&task->output);
    if (!task->error.data) initStringInfo(&task->error);
    if ((value = PQresultErrorField(result, PG_DIAG_SEVERITY))) appendStringInfo(&task->error, "%sseverity%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SEVERITY_NONLOCALIZED))) appendStringInfo(&task->error, "%sseverity_nonlocalized%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SQLSTATE))) appendStringInfo(&task->error, "%ssqlstate%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY))) appendStringInfo(&task->error, "%smessage_primary%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_MESSAGE_DETAIL))) appendStringInfo(&task->error, "%smessage_detail%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_MESSAGE_HINT))) appendStringInfo(&task->error, "%smessage_hint%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_STATEMENT_POSITION))) appendStringInfo(&task->error, "%sstatement_position%s%c%s", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_INTERNAL_POSITION))) appendStringInfo(&task->error, "%sinternal_position%s%c%s", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_INTERNAL_QUERY))) appendStringInfo(&task->error, "%sinternal_query%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_CONTEXT))) appendStringInfo(&task->error, "%scontext%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SCHEMA_NAME))) appendStringInfo(&task->error, "%sschema_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_TABLE_NAME))) appendStringInfo(&task->error, "%stable_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_COLUMN_NAME))) appendStringInfo(&task->error, "%scolumn_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_DATATYPE_NAME))) appendStringInfo(&task->error, "%sdatatype_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_CONSTRAINT_NAME))) appendStringInfo(&task->error, "%sconstraint_name%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SOURCE_FILE))) appendStringInfo(&task->error, "%ssource_file%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SOURCE_LINE))) appendStringInfo(&task->error, "%ssource_line%s%c%s", task->error.len ? "\n" : "", task->append ? "::int4" : "", task->delimiter, value);
    if ((value = PQresultErrorField(result, PG_DIAG_SOURCE_FUNCTION))) appendStringInfo(&task->error, "%ssource_function%s%c%s", task->error.len ? "\n" : "", task->append ? "::text" : "", task->delimiter, value);
    appendStringInfo(&task->output, "%sROLLBACK", task->output.len ? "\n" : "");
    task->skip++;
    task->fail = true;
}

static void work_query(Task *task) {
    StringInfoData buf;
    List *list;
    if (task_work(task)) { work_finish(task); return; }
    D1("id = %li, timeout = %i, input = %s, count = %i", task->id, task->timeout, task->input, task->count);
    PG_TRY();
        list = pg_parse_query(task->input);
        task->length = list_length(list);
        list_free_deep(list);
    PG_CATCH();
        FlushErrorState();
    PG_END_TRY();
    initStringInfo(&buf);
    task->skip = 0;
    appendStringInfo(&buf, "SET \"pg_task.id\" = %li;\n", task->id);
    task->skip++;
    if (task->timeout) {
        appendStringInfo(&buf, "SET \"statement_timeout\" = %i;\n", task->timeout);
        task->skip++;
    }
    if (task->append) {
        appendStringInfoString(&buf, "SET \"config.append_type_to_column_name\" = true;\n");
        task->skip++;
    }
    appendStringInfoString(&buf, task->input);
    pfree(task->input);
    task->input = buf.data;
    if (!PQsendQuery(task->conn, task->input)) work_error(task, "!PQsendQuery"); else {
        pfree(task->input);
        task->input = NULL;
        task->events = WL_SOCKET_WRITEABLE;
    }
}

static void work_repeat(Task *task) {
    if (PQtransactionStatus(task->conn) != PQTRANS_IDLE) { if (!PQsendQuery(task->conn, "COMMIT")) work_error(task, "!PQsendQuery"); else task->events = WL_SOCKET_WRITEABLE; return; }
    if (task_done(task)) { work_finish(task); return; }
    D1("repeat = %s, delete = %s, live = %s", task->repeat ? "true" : "false", task->delete ? "true" : "false", task->live ? "true" : "false");
    if (task->repeat) task_repeat(task);
    if (task->delete && !task->output.data) task_delete(task);
    if (task->output.data) pfree(task->output.data);
    task->output.data = NULL;
    if (task->error.data) pfree(task->error.data);
    task->error.data = NULL;
    if (!task->live || task_live(task)) work_finish(task); else work_query(task);
}

static void work_result(Task *task) {
    if (!PQconsumeInput(task->conn)) work_error(task, "!PQconsumeInput"); else
    if (PQisBusy(task->conn)) task->events = WL_SOCKET_READABLE; else {
        for (PGresult *result; (result = PQgetResult(task->conn)); PQclear(result)) switch (PQresultStatus(result)) {
            case PGRES_COMMAND_OK: work_command(task, result); break;
            case PGRES_FATAL_ERROR: work_fail(task, result); break;
            case PGRES_TUPLES_OK: work_success(task, result); break;
            default: D1(PQresStatus(PQresultStatus(result))); break;
        }
        work_repeat(task);
    }
}

static void work_connect(Task *task) {
    switch (PQstatus(task->conn)) {
        case CONNECTION_AUTH_OK: D1("PQstatus == CONNECTION_AUTH_OK"); break;
        case CONNECTION_AWAITING_RESPONSE: D1("PQstatus == CONNECTION_AWAITING_RESPONSE"); break;
        case CONNECTION_BAD: D1("PQstatus == CONNECTION_BAD"); work_error(task, "PQstatus == CONNECTION_BAD"); return;
#if (PG_VERSION_NUM >= 130000)
        case CONNECTION_CHECK_TARGET: D1("PQstatus == CONNECTION_CHECK_TARGET"); break;
#endif
        case CONNECTION_CHECK_WRITABLE: D1("PQstatus == CONNECTION_CHECK_WRITABLE"); break;
        case CONNECTION_CONSUME: D1("PQstatus == CONNECTION_CONSUME"); break;
        case CONNECTION_GSS_STARTUP: D1("PQstatus == CONNECTION_GSS_STARTUP"); break;
        case CONNECTION_MADE: D1("PQstatus == CONNECTION_MADE"); break;
        case CONNECTION_NEEDED: D1("PQstatus == CONNECTION_NEEDED"); break;
        case CONNECTION_OK: D1("PQstatus == CONNECTION_OK"); task->connected = true; break;
        case CONNECTION_SETENV: D1("PQstatus == CONNECTION_SETENV"); break;
        case CONNECTION_SSL_STARTUP: D1("PQstatus == CONNECTION_SSL_STARTUP"); break;
        case CONNECTION_STARTED: D1("PQstatus == CONNECTION_STARTED"); break;
    }
    switch (PQconnectPoll(task->conn)) {
        case PGRES_POLLING_ACTIVE: D1("PQconnectPoll == PGRES_POLLING_ACTIVE"); break;
        case PGRES_POLLING_FAILED: D1("PQconnectPoll == PGRES_POLLING_FAILED"); work_error(task, "PQconnectPoll == PGRES_POLLING_FAILED"); return;
        case PGRES_POLLING_OK: D1("PQconnectPoll == PGRES_POLLING_OK"); task->connected = true; break;
        case PGRES_POLLING_READING: D1("PQconnectPoll == PGRES_POLLING_READING"); task->events = WL_SOCKET_READABLE; break;
        case PGRES_POLLING_WRITING: D1("PQconnectPoll == PGRES_POLLING_WRITING"); task->events = WL_SOCKET_WRITEABLE; break;
    }
    if ((task->fd = PQsocket(task->conn)) < 0) work_error(task, "PQsocket < 0");
    if (task->connected) {
        if (!(task->pid = PQbackendPID(task->conn))) work_error(task, "!PQbackendPID"); else work_query(task);
    }
}

void work_socket(Task *task) {
    D1("connected = %s", task->connected ? "true" : "false");
    if (task->connected) work_result(task); else work_connect(task);
}

void work_worker(Datum main_arg); void work_worker(Datum main_arg) {
    TimestampTz stop = GetCurrentTimestamp(), start = stop;
    Work work;
    MemSet(&work, 0, sizeof(work));
    work_init_conf(&work);
    sigterm = sigterm || work_init(&work);
    while (!sigterm) {
        int nevents = queue_size(&work.queue) + 2;
        WaitEvent *events = palloc0(nevents * sizeof(*events));
        WaitEventSet *set = CreateWaitEventSet(TopMemoryContext, nevents);
        AddWaitEventToSet(set, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
        AddWaitEventToSet(set, WL_EXIT_ON_PM_DEATH, PGINVALID_SOCKET, NULL, NULL);
        queue_each(&work.queue, queue) {
            Task *task = queue_data(queue, Task, queue);
            if (task->events & WL_SOCKET_WRITEABLE) switch (PQflush(task->conn)) {
                case 0: /*D1("PQflush = 0");*/ break;
                case 1: D1("PQflush = 1"); break;
                default: D1("PQflush = default"); break;
            }
            AddWaitEventToSet(set, task->events & WL_SOCKET_MASK, task->fd, NULL, task);
        }
        nevents = WaitEventSetWait(set, work.timeout, events, nevents, PG_WAIT_EXTENSION);
        for (int i = 0; i < nevents; i++) {
            WaitEvent *event = &events[i];
            if (event->events & WL_LATCH_SET) D1("WL_LATCH_SET");
            if (event->events & WL_SOCKET_READABLE) D1("WL_SOCKET_READABLE");
            if (event->events & WL_SOCKET_WRITEABLE) D1("WL_SOCKET_WRITEABLE");
            if (event->events & WL_POSTMASTER_DEATH) D1("WL_POSTMASTER_DEATH");
            if (event->events & WL_EXIT_ON_PM_DEATH) D1("WL_EXIT_ON_PM_DEATH");
            if (event->events & WL_LATCH_SET) sigterm = sigterm || work_latch();
            if (event->events & WL_SOCKET_MASK) work_socket(event->user_data);
        }
        stop = GetCurrentTimestamp();
        if (work.timeout > 0 && (TimestampDifferenceExceeds(start, stop, work.timeout) || !nevents)) {
            work_timeout(&work);
            start = stop;
        }
        FreeWaitEventSet(set);
        pfree(events);
    }
}