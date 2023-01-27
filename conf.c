#include "include.h"

extern char *task_null;
extern int conf_fetch;
extern int conf_timeout;
extern int work_restart;
static volatile dlist_head head;
static volatile TimeoutId timeout;

static void conf_latch(void) {
    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();
}

static void conf_data(const Work *w) {
    List *names = stringToQualifiedNameList(w->data);
    StringInfoData src;
    elog(DEBUG1, "user = %s, data = %s", w->shared->user, w->shared->data);
    set_ps_display_my("data");
    initStringInfoMy(&src);
    appendStringInfo(&src, SQL(CREATE DATABASE %s WITH OWNER = %s), w->data, w->user);
    SPI_connect_my(src.data);
    if (!OidIsValid(get_database_oid(strVal(linitial(names)), true))) {
        CreatedbStmt *stmt = makeNode(CreatedbStmt);
        ParseState *pstate = make_parsestate(NULL);
        stmt->dbname = w->shared->data;
        stmt->options = list_make1(makeDefElemMy("owner", (Node *)makeString(w->shared->user), -1));
        pstate->p_sourcetext = src.data;
        createdb_my(pstate, stmt);
        list_free_deep(stmt->options);
        free_parsestate(pstate);
        pfree(stmt);
    }
    SPI_finish_my();
    list_free_deep(names);
    pfree(src.data);
    set_ps_display_my("idle");
}

static void conf_free(Work *w) {
    dlist_delete(&w->node);
    dsm_detach(w->seg);
    pfree(w);
}

static void conf_sigaction(int signum, siginfo_t *siginfo, void *code)  {
    dlist_mutable_iter iter;
    elog(DEBUG1, "si_pid = %i", siginfo->si_pid);
    dlist_foreach_modify(iter, (dlist_head *)&head) {
        Work *w = dlist_container(Work, node, iter.cur);
        if (siginfo->si_pid == w->pid) conf_free(w);
    }
    if (dlist_is_empty((dlist_head *)&head)) {
        ShutdownRequestPending = true;
        SetLatch(MyLatch);
    }
}

static void conf_timeout_handler(void) {
    dlist_mutable_iter iter;
    TimestampTz finish = get_timeout_finish_time(timeout), min = 0;
    elog(DEBUG1, "%s", timestamptz_to_str(finish));
    dlist_foreach_modify(iter, (dlist_head *)&head) {
        Work *w = dlist_container(Work, node, iter.cur);
        TimestampTz start = TimestampTzPlusMilliseconds(w->start, conf_timeout);
        if (finish >= start) conf_free(w);
        else if (min == 0 || min >= start) min = start;
    }
    if (dlist_is_empty((dlist_head *)&head)) {
        ShutdownRequestPending = true;
        SetLatch(MyLatch);
    } else enable_timeout_at(timeout, min);
}

static void conf_user(const Work *w) {
    List *names = stringToQualifiedNameList(w->user);
    StringInfoData src;
    elog(DEBUG1, "user = %s", w->shared->user);
    set_ps_display_my("user");
    initStringInfoMy(&src);
    appendStringInfo(&src, SQL(CREATE ROLE %s WITH LOGIN), w->user);
    SPI_connect_my(src.data);
    if (!OidIsValid(get_role_oid(strVal(linitial(names)), true))) {
        CreateRoleStmt *stmt = makeNode(CreateRoleStmt);
        ParseState *pstate = make_parsestate(NULL);
        stmt->role = w->shared->user;
        stmt->options = list_make1(makeDefElemMy("canlogin", (Node *)makeInteger(1), -1));
        pstate->p_sourcetext = src.data;
        CreateRoleMy(pstate, stmt);
        list_free_deep(stmt->options);
        free_parsestate(pstate);
        pfree(stmt);
    }
    SPI_finish_my();
    list_free_deep(names);
    pfree(src.data);
    set_ps_display_my("idle");
}

static void conf_work(Work *w) {
    BackgroundWorkerHandle *handle;
    BackgroundWorker worker = {0};
    size_t len;
    set_ps_display_my("work");
    w->data = quote_identifier(w->shared->data);
    w->user = quote_identifier(w->shared->user);
    conf_user(w);
    conf_data(w);
    if (w->data != w->shared->data) pfree((void *)w->data);
    if (w->user != w->shared->user) pfree((void *)w->user);
    if ((len = strlcpy(worker.bgw_function_name, "work_main", sizeof(worker.bgw_function_name))) >= sizeof(worker.bgw_function_name)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("strlcpy %li >= %li", len, sizeof(worker.bgw_function_name))));
    if ((len = strlcpy(worker.bgw_library_name, "pg_task", sizeof(worker.bgw_library_name))) >= sizeof(worker.bgw_library_name)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("strlcpy %li >= %li", len, sizeof(worker.bgw_library_name))));
    if ((len = snprintf(worker.bgw_name, sizeof(worker.bgw_name) - 1, "%s %s pg_work %s %s %li", w->shared->user, w->shared->data, w->shared->schema, w->shared->table, w->shared->sleep)) >= sizeof(worker.bgw_name) - 1) ereport(WARNING, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("snprintf %li >= %li", len, sizeof(worker.bgw_name) - 1)));
#if PG_VERSION_NUM >= 110000
    if ((len = strlcpy(worker.bgw_type, worker.bgw_name, sizeof(worker.bgw_type))) >= sizeof(worker.bgw_type)) ereport(ERROR, (errcode(ERRCODE_OUT_OF_MEMORY), errmsg("strlcpy %li >= %li", len, sizeof(worker.bgw_type))));
#endif
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_main_arg = UInt32GetDatum(dsm_segment_handle(w->seg));
    worker.bgw_notify_pid = MyProcPid;
    worker.bgw_restart_time = work_restart;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    if (!RegisterDynamicBackgroundWorker(&worker, &handle)) ereport(ERROR, (errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED), errmsg("could not register background worker"), errhint("Consider increasing configuration parameter \"max_worker_processes\".")));
    switch (WaitForBackgroundWorkerStartup(handle, &w->pid)) {
        case BGWH_NOT_YET_STARTED: ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("BGWH_NOT_YET_STARTED is never returned!"))); break;
        case BGWH_POSTMASTER_DIED: ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("cannot start background worker without postmaster"), errhint("Kill all remaining database processes and restart the database."))); break;
        case BGWH_STARTED: break;
        case BGWH_STOPPED: ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("could not start background worker"), errhint("More details may be available in the server log."))); break;
    }
    w->start = GetCurrentTimestamp();
    if (!get_timeout_active(timeout)) enable_timeout_at(timeout, TimestampTzPlusMilliseconds(w->start, conf_timeout));
    pfree(handle);
}

void conf_main(Datum arg) {
    dlist_mutable_iter iter;
    Portal portal;
    StringInfoData src;
    struct sigaction act = {0}, oldact = {0};
    initStringInfoMy(&src);
    appendStringInfo(&src, SQL(
        WITH j AS (
            WITH s AS (
                WITH s AS (
                    SELECT "setdatabase", "setrole", regexp_split_to_array(UNNEST("setconfig"), '=') AS "setconfig" FROM "pg_db_role_setting"
                ) SELECT "setdatabase", "setrole", %1$s(array_agg("setconfig"[1]), array_agg("setconfig"[2])) AS "setconfig" FROM s GROUP BY 1, 2
            ) SELECT    COALESCE("data", "user", current_setting('pg_task.data')) AS "data",
                        EXTRACT(epoch FROM COALESCE("reset", (u."setconfig"->>'pg_task.reset')::interval, (d."setconfig"->>'pg_task.reset')::interval, current_setting('pg_task.reset')::interval))::bigint AS "reset",
                        COALESCE("schema", u."setconfig"->>'pg_task.schema', d."setconfig"->>'pg_task.schema', current_setting('pg_task.schema')) AS "schema",
                        COALESCE("table", u."setconfig"->>'pg_task.table', d."setconfig"->>'pg_task.table', current_setting('pg_task.table')) AS "table",
                        COALESCE("sleep", (u."setconfig"->>'pg_task.sleep')::bigint, (d."setconfig"->>'pg_task.sleep')::bigint, current_setting('pg_task.sleep')::bigint) AS "sleep",
                        COALESCE("user", "data", current_setting('pg_task.user')) AS "user"
            FROM        jsonb_to_recordset(current_setting('pg_task.json')::jsonb) AS j ("data" text, "reset" interval, "schema" text, "table" text, "sleep" bigint, "user" text)
            LEFT JOIN   s AS d on d."setdatabase" = (SELECT "oid" FROM "pg_database" WHERE "datname" = COALESCE("data", "user", current_setting('pg_task.data')))
            LEFT JOIN   s AS u on u."setrole" = (SELECT "oid" FROM "pg_authid" WHERE "rolname" = COALESCE("user", "data", current_setting('pg_task.user')))
        ) SELECT    DISTINCT j.* FROM j
        LEFT JOIN "pg_locks" AS l ON "locktype" = 'userlock' AND "mode" = 'AccessExclusiveLock' AND "granted" AND "objsubid" = 3 AND "database" = (SELECT "oid" FROM "pg_database" WHERE "datname" = "data") AND "classid" = (SELECT "oid" FROM "pg_authid" WHERE "rolname" = "user") AND "objid" = hashtext(concat_ws(' ', 'pg_work', "schema", "table", "sleep"))::oid
        WHERE "pid" IS NULL
    ),
#if PG_VERSION_NUM >= 90500
        "jsonb_object"
#else
        "json_object"
#endif
    );
    act.sa_sigaction = conf_sigaction;
    act.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR2, &act, &oldact);
    timeout = RegisterTimeout(USER_TIMEOUT, conf_timeout_handler);
    BackgroundWorkerUnblockSignals();
    CreateAuxProcessResourceOwner();
    BackgroundWorkerInitializeConnectionMy("postgres", NULL, 0);
    set_config_option_my("application_name", "pg_conf", PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    pgstat_report_appname("pg_conf");
    set_ps_display_my("main");
    process_session_preload_libraries();
    if (!lock_data_user(MyDatabaseId, GetUserId())) { elog(WARNING, "!lock_data_user(%i, %i)", MyDatabaseId, GetUserId()); return; }
    dlist_init((dlist_head *)&head);
    SPI_connect_my(src.data);
    portal = SPI_cursor_open_with_args_my(src.data, src.data, 0, NULL, NULL, NULL);
    do {
        SPI_cursor_fetch(portal, true, conf_fetch);
        for (uint64 row = 0; row < SPI_processed; row++) {
            HeapTuple val = SPI_tuptable->vals[row];
            TupleDesc tupdesc = SPI_tuptable->tupdesc;
            Work *w = MemoryContextAllocZero(TopMemoryContext, sizeof(*w));;
            set_ps_display_my("row");
            w->shared = shm_toc_allocate_my(PG_WORK_MAGIC, &w->seg, sizeof(*w->shared));
            w->shared->reset = DatumGetInt64(SPI_getbinval_my(val, tupdesc, "reset", false));
            w->shared->sleep = DatumGetInt64(SPI_getbinval_my(val, tupdesc, "sleep", false));
            text_to_cstring_buffer((text *)DatumGetPointer(SPI_getbinval_my(val, tupdesc, "data", false)), w->shared->data, sizeof(w->shared->data));
            text_to_cstring_buffer((text *)DatumGetPointer(SPI_getbinval_my(val, tupdesc, "schema", false)), w->shared->schema, sizeof(w->shared->schema));
            text_to_cstring_buffer((text *)DatumGetPointer(SPI_getbinval_my(val, tupdesc, "table", false)), w->shared->table, sizeof(w->shared->table));
            text_to_cstring_buffer((text *)DatumGetPointer(SPI_getbinval_my(val, tupdesc, "user", false)), w->shared->user, sizeof(w->shared->user));
            elog(DEBUG1, "row = %lu, user = %s, data = %s, schema = %s, table = %s, sleep = %li, reset = %li", row, w->shared->user, w->shared->data, w->shared->schema, w->shared->table, w->shared->sleep, w->shared->reset);
            dlist_push_head((dlist_head *)&head, &w->node);
        }
    } while (SPI_processed);
    SPI_cursor_close(portal);
    SPI_finish_my();
    pfree(src.data);
    set_ps_display_my("idle");
    dlist_foreach_modify(iter, (dlist_head *)&head) conf_work(dlist_container(Work, node, iter.cur));
    while (!ShutdownRequestPending) {
        int rc = WaitLatchMy(MyLatch, WL_LATCH_SET | WL_POSTMASTER_DEATH, 0, PG_WAIT_EXTENSION);
        if (rc & WL_LATCH_SET) conf_latch();
        if (rc & WL_POSTMASTER_DEATH) ShutdownRequestPending = true;
    }
    if (!unlock_data_user(MyDatabaseId, GetUserId())) elog(WARNING, "!unlock_data_user(%i, %i)", MyDatabaseId, GetUserId());
}
