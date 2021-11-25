#include "include.h"

extern bool xact_started;
extern char *default_null;
extern Work *work;
Task *task;

bool task_done(Task *task) {
    char nulls[] = {' ', task->output.data ? ' ' : 'n', task->error.data ? ' ' : 'n', ' ', task->remote ? ' ' : 'n', ' ', ' ', ' '};
    Datum values[] = {Int64GetDatum(task->id), CStringGetTextDatumMy(TopMemoryContext, task->output.data), CStringGetTextDatumMy(TopMemoryContext, task->error.data), CStringGetTextDatumMy(TopMemoryContext, task->group), CStringGetTextDatumMy(TopMemoryContext, task->remote), Int32GetDatum(task->max), Int32GetDatum(task->count), TimestampTzGetDatum(task->start)};
    int64 live = 0;
    static Oid argtypes[] = {INT8OID, TEXTOID, TEXTOID, TEXTOID, TEXTOID, INT4OID, INT4OID, TIMESTAMPTZOID};
    static SPIPlanPtr plan = NULL;
    static StringInfoData src = {0};
    D1("id = %li, group = %s, output = %s, error = %s, max = %i, count = %i", task->id, task->group, task->output.data ? task->output.data : default_null, task->error.data ? task->error.data : default_null, task->max, task->count);
    set_ps_display_my("done");
    if (!src.data) {
        initStringInfoMy(TopMemoryContext, &src);
        appendStringInfo(&src, SQL(
            WITH a AS (
                SELECT t.* FROM %1$s AS t WHERE max < 0 AND plan < CURRENT_TIMESTAMP AND "group" = $4 AND state = 'PLAN'::%2$s FOR UPDATE OF t SKIP LOCKED
            ), au AS (
                UPDATE %1$s AS t SET plan = CURRENT_TIMESTAMP FROM a WHERE t.id = a.id RETURNING t.*
            ), c AS (
                SELECT "group", count(id) FROM au GROUP BY 1
            ), s AS (
                SELECT t.* FROM %1$s AS t WHERE id = $1 FOR UPDATE OF t
            ), si AS (
                INSERT INTO %1$s AS t (parent, plan, "group", max, input, timeout, delete, repeat, drift, count, live) SELECT id, CASE
                    WHEN drift THEN CURRENT_TIMESTAMP + repeat
                    ELSE (WITH RECURSIVE r AS (SELECT plan AS p UNION SELECT p + repeat FROM r WHERE p <= CURRENT_TIMESTAMP) SELECT * FROM r ORDER BY 1 DESC LIMIT 1)
                END AS plan, "group", max, input, timeout, delete, repeat, drift, count, live FROM s WHERE repeat > '0 sec' LIMIT 1 RETURNING t.*
            ), sd AS (
                DELETE FROM %1$s AS t WHERE id = $1 AND delete AND $2 IS NULL RETURNING t.*
            ), su AS (
                UPDATE %1$s AS t SET state = 'DONE'::%2$s, stop = CURRENT_TIMESTAMP, output = $2, error = $3 FROM s WHERE t.id = s.id RETURNING t.*
            ), l AS (
                SELECT t.* FROM %1$s AS t
                WHERE state = 'PLAN'::%2$s AND plan <= CURRENT_TIMESTAMP AND t.group = $4 AND remote IS NOT DISTINCT FROM $5 AND max >= $6 AND CASE
                    WHEN count > 0 AND live > '0 sec' THEN count > $7 AND $8 + live > CURRENT_TIMESTAMP ELSE count > $7 OR $8 + live > CURRENT_TIMESTAMP
                END AND t.start IS NULL AND t.stop IS NULL AND t.pid IS NULL ORDER BY max DESC, id LIMIT 1 FOR UPDATE OF t SKIP LOCKED
            ), lu AS (
                UPDATE %1$s AS t SET state = 'TAKE'::%2$s FROM l WHERE t.id = l.id RETURNING t.*
            ) SELECT s.id, lu.id AS live, su.id IS NOT NULL AS update, si.id IS NOT NULL AS insert, sd.id IS NOT NULL AS delete, c.count IS NOT NULL AS count
            FROM s LEFT JOIN lu ON true LEFT JOIN si ON true LEFT JOIN sd ON true LEFT JOIN su ON true LEFT JOIN c ON true
        ), work->schema_table, work->schema_type);
    }
    SPI_connect_my(src.data);
    if (!plan) plan = SPI_prepare_my(src.data, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, nulls, SPI_OK_SELECT, true);
    if (SPI_processed != 1) W("%li: SPI_processed != 1", task->id); else {
        bool count = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "count", false));
        bool delete = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "delete", false));
        bool insert = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "insert", false));
        bool update = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "update", false));
        live = DatumGetInt64(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "live", true));
        D1("live = %li, update = %s, insert = %s, delete = %s, count = %s", live, update ? "true" : "false", insert ? "true" : "false", delete ? "true" : "false", count ? "true" : "false");
    }
    SPI_finish_my();
    if (values[1]) pfree((void *)values[1]);
    if (values[2]) pfree((void *)values[2]);
    if (values[3]) pfree((void *)values[3]);
    if (values[4]) pfree((void *)values[4]);
    task_free(task);
    if (!unlock_table_id(work->oid.table, task->id)) { W("!unlock_table_id(%i, %li)", work->oid.table, task->id); live = 0; }
    set_ps_display_my("idle");
    task->id = live;
    return ShutdownRequestPending || !live;
}

bool task_work(Task *task) {
    bool exit = false;
    Datum values[] = {Int64GetDatum(task->id), Int32GetDatum(task->pid)};
    static Oid argtypes[] = {INT8OID, INT4OID};
    static SPIPlanPtr plan = NULL;
    static StringInfoData src = {0};
    if (ShutdownRequestPending) return true;
    if (!lock_table_id(work->oid.table, task->id)) { W("!lock_table_id(%i, %li)", work->oid.table, task->id); return true; }
    task->count++;
    D1("id = %li, max = %i, oid = %i, count = %i, pid = %i", task->id, task->max, work->oid.table, task->count, task->pid);
    set_ps_display_my("work");
    if (!task->conn) {
        StringInfoData id;
        initStringInfoMy(TopMemoryContext, &id);
        appendStringInfo(&id, "%li", task->id);
        set_config_option("pg_task.id", id.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
        pfree(id.data);
    }
    if (!src.data) {
        initStringInfoMy(TopMemoryContext, &src);
        appendStringInfo(&src, SQL(
            WITH s AS (
                SELECT id FROM %1$s AS t WHERE id = $1 FOR UPDATE OF t
            ) UPDATE %1$s AS u SET state = 'WORK'::%2$s, start = CURRENT_TIMESTAMP, pid = $2 FROM s WHERE u.id = s.id
            RETURNING "group", input, EXTRACT(epoch FROM timeout)::integer * 1000 AS timeout, header, string, u.null, delimiter, quote, escape, plan + active > CURRENT_TIMESTAMP AS active
        ), work->schema_table, work->schema_type);
    }
    SPI_connect_my(src.data);
    if (!plan) plan = SPI_prepare_my(src.data, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, NULL, SPI_OK_UPDATE_RETURNING, true);
    if (SPI_processed != 1) {
        W("%li: SPI_processed != 1", task->id);
        exit = true;
    } else {
        task->active = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "active", false));
        task->delimiter = DatumGetChar(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "delimiter", false));
        task->escape = DatumGetChar(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "escape", true));
        task->group = TextDatumGetCStringMy(TopMemoryContext, SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "group", false));
        task->header = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "header", false));
        task->input = TextDatumGetCStringMy(TopMemoryContext, SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "input", false));
        task->null = TextDatumGetCStringMy(TopMemoryContext, SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "null", false));
        task->quote = DatumGetChar(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "quote", true));
        task->string = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "string", false));
        task->timeout = DatumGetInt32(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "timeout", false));
        if (0 < StatementTimeout && StatementTimeout < task->timeout) task->timeout = StatementTimeout;
        D1("group = %s, input = %s, timeout = %i, header = %s, string = %s, null = %s, delimiter = %c, quote = %c, escape = %c, active = %s", task->group, task->input, task->timeout, task->header ? "true" : "false", task->string ? "true" : "false", task->null, task->delimiter, task->quote ? task->quote : 30, task->escape ? task->escape : 30, task->active ? "true" : "false");
    }
    SPI_finish_my();
    set_ps_display_my("idle");
    return exit;
}

void task_error(Task *task, ErrorData *edata) {
    if (!task->error.data) initStringInfoMy(TopMemoryContext, &task->error);
    if (!task->output.data) initStringInfoMy(TopMemoryContext, &task->output);
    if (edata->elevel) appendStringInfo(&task->error, "%selevel%c%i", task->error.len ? "\n" : "", task->delimiter, edata->elevel);
    if (edata->output_to_server) appendStringInfo(&task->error, "%soutput_to_server%ctrue", task->error.len ? "\n" : "", task->delimiter);
    if (edata->output_to_client) appendStringInfo(&task->error, "%soutput_to_client%ctrue", task->error.len ? "\n" : "", task->delimiter);
#if PG_VERSION_NUM >= 140000
#else
    if (edata->show_funcname) appendStringInfo(&task->error, "%sshow_funcname%ctrue", task->error.len ? "\n" : "", task->delimiter);
#endif
    if (edata->hide_stmt) appendStringInfo(&task->error, "%shide_stmt%ctrue", task->error.len ? "\n" : "", task->delimiter);
    if (edata->hide_ctx) appendStringInfo(&task->error, "%shide_ctx%ctrue", task->error.len ? "\n" : "", task->delimiter);
    if (edata->filename) appendStringInfo(&task->error, "%sfilename%c%s", task->error.len ? "\n" : "", task->delimiter, edata->filename);
    if (edata->lineno) appendStringInfo(&task->error, "%slineno%c%i", task->error.len ? "\n" : "", task->delimiter, edata->lineno);
    if (edata->funcname) appendStringInfo(&task->error, "%sfuncname%c%s", task->error.len ? "\n" : "", task->delimiter, edata->funcname);
    if (edata->domain) appendStringInfo(&task->error, "%sdomain%c%s", task->error.len ? "\n" : "", task->delimiter, edata->domain);
    if (edata->context_domain) appendStringInfo(&task->error, "%scontext_domain%c%s", task->error.len ? "\n" : "", task->delimiter, edata->context_domain);
    if (edata->sqlerrcode) appendStringInfo(&task->error, "%ssqlerrcode%c%i", task->error.len ? "\n" : "", task->delimiter, edata->sqlerrcode);
    if (edata->message) appendStringInfo(&task->error, "%smessage%c%s", task->error.len ? "\n" : "", task->delimiter, edata->message);
    if (edata->detail) appendStringInfo(&task->error, "%sdetail%c%s", task->error.len ? "\n" : "", task->delimiter, edata->detail);
    if (edata->detail_log) appendStringInfo(&task->error, "%sdetail_log%c%s", task->error.len ? "\n" : "", task->delimiter, edata->detail_log);
    if (edata->hint) appendStringInfo(&task->error, "%shint%c%s", task->error.len ? "\n" : "", task->delimiter, edata->hint);
    if (edata->context) appendStringInfo(&task->error, "%scontext%c%s", task->error.len ? "\n" : "", task->delimiter, edata->context);
    if (edata->message_id) appendStringInfo(&task->error, "%smessage_id%c%s", task->error.len ? "\n" : "", task->delimiter, edata->message_id);
    if (edata->schema_name) appendStringInfo(&task->error, "%sschema_name%c%s", task->error.len ? "\n" : "", task->delimiter, edata->schema_name);
    if (edata->table_name) appendStringInfo(&task->error, "%stable_name%c%s", task->error.len ? "\n" : "", task->delimiter, edata->table_name);
    if (edata->column_name) appendStringInfo(&task->error, "%scolumn_name%c%s", task->error.len ? "\n" : "", task->delimiter, edata->column_name);
    if (edata->datatype_name) appendStringInfo(&task->error, "%sdatatype_name%c%s", task->error.len ? "\n" : "", task->delimiter, edata->datatype_name);
    if (edata->constraint_name) appendStringInfo(&task->error, "%sconstraint_name%c%s", task->error.len ? "\n" : "", task->delimiter, edata->constraint_name);
    if (edata->cursorpos) appendStringInfo(&task->error, "%scursorpos%c%i", task->error.len ? "\n" : "", task->delimiter, edata->cursorpos);
    if (edata->internalpos) appendStringInfo(&task->error, "%sinternalpos%c%i", task->error.len ? "\n" : "", task->delimiter, edata->internalpos);
    if (edata->internalquery) appendStringInfo(&task->error, "%sinternalquery%c%s", task->error.len ? "\n" : "", task->delimiter, edata->internalquery);
    if (edata->saved_errno) appendStringInfo(&task->error, "%ssaved_errno%c%i", task->error.len ? "\n" : "", task->delimiter, edata->saved_errno);
    appendStringInfo(&task->output, SQL(%sROLLBACK), task->output.len ? "\n" : "");
}

static void task_execute(void) {
    int StatementTimeoutMy = StatementTimeout;
    MemoryContext oldMemoryContext = MemoryContextSwitchTo(MessageContext);
    MemoryContextResetAndDeleteChildren(MessageContext);
    InvalidateCatalogSnapshotConditionally();
    MemoryContextSwitchTo(oldMemoryContext);
    whereToSendOutput = DestDebug;
    ReadyForQueryMy(whereToSendOutput);
    SetCurrentStatementStartTimestamp();
    StatementTimeout = task->timeout;
    exec_simple_query_my(task->input);
    if (IsTransactionState()) exec_simple_query_my(SQL(COMMIT));
    if (IsTransactionState()) E("IsTransactionState");
    StatementTimeout = StatementTimeoutMy;
}

static void task_catch(void) {
    MemoryContext oldMemoryContext = MemoryContextSwitchTo(TopMemoryContext);
    ErrorData *edata = CopyErrorData();
    MemoryContextSwitchTo(oldMemoryContext);
    task_error(task, edata);
    FreeErrorData(edata);
    HOLD_INTERRUPTS();
    disable_all_timeouts(false);
    QueryCancelPending = false;
    EmitErrorReport();
    debug_query_string = NULL;
    AbortOutOfAnyTransaction();
#if PG_VERSION_NUM >= 110000
    PortalErrorCleanup();
    SPICleanup();
#endif
    if (MyReplicationSlot) ReplicationSlotRelease();
#if PG_VERSION_NUM >= 100000
    ReplicationSlotCleanup();
#endif
#if PG_VERSION_NUM >= 110000
    jit_reset_after_error();
#endif
    MemoryContextSwitchTo(TopMemoryContext);
    FlushErrorState();
    xact_started = false;
    RESUME_INTERRUPTS();
}

static void SignalHandlerForShutdownRequestMy(SIGNAL_ARGS) {
    int save_errno = errno;
    ShutdownRequestPending = true;
    SetLatch(MyLatch);
    if (!DatumGetBool(DirectFunctionCall1(pg_cancel_backend, Int32GetDatum(MyProcPid)))) E("!pg_cancel_backend(%i)", MyProcPid);
    errno = save_errno;
}

static void task_init(void) {
    char *p = MyBgworkerEntry->bgw_extra;
    MemoryContext oldcontext = CurrentMemoryContext;
    StringInfoData oid, schema_table, schema_type;
    task = MemoryContextAllocZero(TopMemoryContext, sizeof(*task));
    work = MemoryContextAllocZero(TopMemoryContext, sizeof(*work));
#define X(name, serialize, deserialize) deserialize(name);
    TASK
#undef X
    pqsignal(SIGTERM, SignalHandlerForShutdownRequestMy);
    BackgroundWorkerUnblockSignals();
#if PG_VERSION_NUM >= 110000
    BackgroundWorkerInitializeConnectionByOid(work->oid.data, work->oid.user, 0);
    pgstat_report_appname(MyBgworkerEntry->bgw_type);
#else
    BackgroundWorkerInitializeConnectionByOid(work->oid.data, work->oid.user);
#endif
    set_ps_display_my("init");
    process_session_preload_libraries();
    StartTransactionCommand();
    MemoryContextSwitchTo(oldcontext);
    if (!(work->str.data = get_database_name(work->oid.data))) E("!get_database_name");
    if (!(work->str.schema = get_namespace_name(work->oid.schema))) E("!get_namespace_name");
    if (!(work->str.table = get_rel_name(work->oid.table))) E("!get_rel_name");
    if (!(work->str.user = GetUserNameFromId(work->oid.user, true))) E("!GetUserNameFromId");
    CommitTransactionCommand();
    MemoryContextSwitchTo(oldcontext);
    work->quote.data = (char *)quote_identifier(work->str.data);
    work->quote.schema = (char *)quote_identifier(work->str.schema);
    work->quote.table = (char *)quote_identifier(work->str.table);
    work->quote.user = (char *)quote_identifier(work->str.user);
#if PG_VERSION_NUM >= 110000
#else
    pgstat_report_appname(MyBgworkerEntry->bgw_name + strlen(work->str.user) + 1 + strlen(work->str.data) + 1);
#endif
    task->id = DatumGetInt64(MyBgworkerEntry->bgw_main_arg);
    if (!MyProcPort && !(MyProcPort = (Port *) calloc(1, sizeof(Port)))) E("!calloc");
    if (!MyProcPort->remote_host) MyProcPort->remote_host = "[local]";
    if (!MyProcPort->user_name) MyProcPort->user_name = work->str.user;
    if (!MyProcPort->database_name) MyProcPort->database_name = work->str.data;
#if PG_VERSION_NUM >= 110000
    set_config_option("application_name", MyBgworkerEntry->bgw_type, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
#else
    set_config_option("application_name", MyBgworkerEntry->bgw_name + strlen(work->str.user) + 1 + strlen(work->str.data) + 1, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
#endif
    set_config_option("pg_task.data", work->str.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    set_config_option("pg_task.group", task->group, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    set_config_option("pg_task.schema", work->str.schema, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    set_config_option("pg_task.table", work->str.table, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    set_config_option("pg_task.user", work->str.user, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    if (!MessageContext) MessageContext = AllocSetContextCreate(TopMemoryContext, "MessageContext", ALLOCSET_DEFAULT_SIZES);
    D1("user = %s, data = %s, schema = %s, table = %s, oid = %i, id = %li, hash = %i, group = %s, max = %i", work->str.user, work->str.data, work->str.schema, work->str.table, work->oid.table, task->id, task->hash, task->group, task->max);
    initStringInfoMy(TopMemoryContext, &schema_table);
    appendStringInfo(&schema_table, "%s.%s", work->quote.schema, work->quote.table);
    work->schema_table = schema_table.data;
    initStringInfoMy(TopMemoryContext, &schema_type);
    appendStringInfo(&schema_type, "%s.state", work->quote.schema);
    work->schema_type = schema_type.data;
    initStringInfoMy(TopMemoryContext, &oid);
    appendStringInfo(&oid, "%i", work->oid.table);
    set_config_option("pg_task.oid", oid.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR, false);
    pfree(oid.data);
    task->pid = MyProcPid;
    task->start = GetCurrentTimestamp();
    set_ps_display_my("idle");
}

static void task_latch(void) {
    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();
}

static bool task_timeout(void) {
    if (task_work(task)) return true;
    D1("id = %li, timeout = %i, input = %s, count = %i", task->id, task->timeout, task->input, task->count);
    set_ps_display_my("timeout");
    PG_TRY();
        if (!task->active) E("task %li not active", task->id);
        task_execute();
    PG_CATCH();
        task_catch();
    PG_END_TRY();
    pgstat_report_stat(false);
    pgstat_report_activity(STATE_IDLE, NULL);
    set_ps_display_my("idle");
    return task_done(task);
}

void task_free(Task *task) {
    if (task->error.data) { pfree(task->error.data); task->error.data = NULL; }
    if (task->group) { pfree(task->group); task->group = NULL; }
    if (task->input) { pfree(task->input); task->input = NULL; }
    if (task->null) { pfree(task->null); task->null = NULL; }
    if (task->output.data) { pfree(task->output.data); task->output.data = NULL; }
    if (task->remote) { pfree(task->remote); task->remote = NULL; }
}

void task_main(Datum main_arg) {
    task_init();
    if (!lock_table_pid_hash(work->oid.table, task->pid, task->hash)) { W("!lock_table_pid_hash(%i, %i, %i)", work->oid.table, task->pid, task->hash); return; }
    while (!ShutdownRequestPending) {
#if PG_VERSION_NUM >= 100000
        int rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 0, PG_WAIT_EXTENSION);
#else
        int rc = WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 0);
#endif
        if (rc & WL_TIMEOUT) if (task_timeout()) ShutdownRequestPending = true;
        if (rc & WL_LATCH_SET) task_latch();
        if (rc & WL_POSTMASTER_DEATH) ShutdownRequestPending = true;
    }
    if (!unlock_table_pid_hash(work->oid.table, task->pid ? task->pid : MyProcPid, task->hash)) W("!unlock_table_pid_hash(%i, %i, %i)", work->oid.table, task->pid ? task->pid : MyProcPid, task->hash);
}
