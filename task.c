#include "include.h"

extern bool xact_started;
extern char *task_null;
extern int task_fetch;
extern Work work;
static emit_log_hook_type emit_log_hook_prev = NULL;
Task task = {0};

static bool task_live(const Task *t) {
    Datum values[] = {Int32GetDatum(t->shared->hash), Int32GetDatum(t->shared->max), Int32GetDatum(t->count), TimestampTzGetDatum(t->start)};
    static Oid argtypes[] = {INT4OID, INT4OID, INT4OID, TIMESTAMPTZOID};
    static SPIPlanPtr plan = NULL;
    static StringInfoData src = {0};
    elog(DEBUG1, "id = %li, hash = %i, max = %i, count = %i, start = %s", t->shared->id, t->shared->hash, t->shared->max, t->count, timestamptz_to_str(t->start));
    set_ps_display_my("live");
    if (!src.data) {
        initStringInfoMy(&src);
        appendStringInfo(&src, SQL(
            WITH s AS (SELECT "id" FROM %1$s AS t WHERE "plan" <= CURRENT_TIMESTAMP AND "state" = 'PLAN' AND "hash" = $1 AND "max" >= $2 AND CASE
                WHEN "count" > 0 AND "live" > '0 sec' THEN "count" > $3 AND $4 + "live" > CURRENT_TIMESTAMP ELSE "count" > $3 OR $4 + "live" > CURRENT_TIMESTAMP
            END ORDER BY "max" DESC, "id" LIMIT 1 FOR UPDATE OF t %2$s) UPDATE %1$s AS t SET "state" = 'TAKE' FROM s WHERE t.id = s.id RETURNING t.id
        ), work.schema_table,
#if PG_VERSION_NUM >= 90500
        "SKIP LOCKED"
#else
        ""
#endif
        );
    }
    if (!plan) plan = SPI_prepare_my(src.data, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, NULL, SPI_OK_UPDATE_RETURNING);
    t->shared->id = SPI_processed == 1 ? DatumGetInt64(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "id", false)) : 0;
    elog(DEBUG1, "id = %li", t->shared->id);
    set_ps_display_my("idle");
    return ShutdownRequestPending || !t->shared->id;
}

static void task_columns(const Task *t) {
    Datum values[] = {CStringGetTextDatumMy(work.shared->schema), CStringGetTextDatumMy(work.shared->table)};
    static Oid argtypes[] = {TEXTOID, TEXTOID};
    static const char *src = SQL(
        SELECT string_agg(quote_ident(column_name), ', ') AS columns FROM information_schema.columns WHERE table_schema = $1 AND table_name = $2 AND column_name NOT IN ('id', 'plan', 'parent', 'start', 'stop', 'hash', 'pid', 'state', 'error', 'output')
    );
    SPI_execute_with_args_my(src, countof(argtypes), argtypes, values, NULL, SPI_OK_SELECT);
    if (SPI_processed != 1) elog(WARNING, "columns id = %li, SPI_processed %lu != 1", t->shared->id, (long)SPI_processed); else {
        work.columns = TextDatumGetCStringMy(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "columns", false));
        elog(DEBUG1, "columns id = %li, %s", t->shared->id, work.columns);
    }
    if (values[0]) pfree((void *)values[0]);
    if (values[1]) pfree((void *)values[1]);
}

static void task_delete(const Task *t) {
    Datum values[] = {Int64GetDatum(t->shared->id)};
    static Oid argtypes[] = {INT8OID};
    static SPIPlanPtr plan = NULL;
    static StringInfoData src = {0};
    elog(DEBUG1, "id = %li", t->shared->id);
    set_ps_display_my("delete");
    if (!src.data) {
        initStringInfoMy(&src);
        appendStringInfo(&src, SQL(WITH s AS (SELECT "id" FROM %1$s AS t WHERE "id" = $1 FOR UPDATE OF t) DELETE FROM %1$s AS t WHERE "id" = $1 RETURNING t.id), work.schema_table);
    }
    if (!plan) plan = SPI_prepare_my(src.data, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, NULL, SPI_OK_DELETE_RETURNING);
    if (SPI_processed != 1) elog(WARNING, "delete id = %li, SPI_processed %lu != 1", t->shared->id, (long)SPI_processed);
    else elog(DEBUG1, "delete id = %li", DatumGetInt64(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "id", false)));
    set_ps_display_my("idle");
}

static void task_repeat(const Task *t) {
    Datum values[] = {Int64GetDatum(t->shared->id)};
    static Oid argtypes[] = {INT8OID};
    static SPIPlanPtr plan = NULL;
    static StringInfoData src = {0};
    elog(DEBUG1, "id = %li", t->shared->id);
    set_ps_display_my("repeat");
    if (!src.data) {
        if (!work.columns) task_columns(t);
        if (!work.columns) return;
        initStringInfoMy(&src);
        appendStringInfo(&src, SQL(
            WITH s AS (SELECT * FROM %1$s AS t WHERE "id" = $1 FOR UPDATE OF t) INSERT INTO %1$s AS t ("parent", "plan", %2$s) SELECT "id", CASE
                WHEN "drift" THEN CURRENT_TIMESTAMP + "repeat" ELSE (WITH RECURSIVE r AS (SELECT "plan" AS p UNION SELECT p + "repeat" FROM r WHERE p <= CURRENT_TIMESTAMP) SELECT * FROM r ORDER BY 1 DESC LIMIT 1)
            END AS "plan", %2$s FROM s WHERE "repeat" > '0 sec' LIMIT 1 RETURNING t.id
        ), work.schema_table, work.columns);
    }
    if (!plan) plan = SPI_prepare_my(src.data, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, NULL, SPI_OK_INSERT_RETURNING);
    if (SPI_processed != 1) elog(WARNING, "repeat id = %li, SPI_processed %lu != 1", t->shared->id, (long)SPI_processed);
    else elog(DEBUG1, "repeat id = %li", DatumGetInt64(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "id", false)));
    set_ps_display_my("idle");
}

bool task_done(Task *t) {
    bool delete = false, exit = true, repeat = false;
    char nulls[] = {' ', t->output.data ? ' ' : 'n', t->error.data ? ' ' : 'n'};
    Datum values[] = {Int64GetDatum(t->shared->id), CStringGetTextDatumMy(t->output.data), CStringGetTextDatumMy(t->error.data)};
    static Oid argtypes[] = {INT8OID, TEXTOID, TEXTOID};
    static SPIPlanPtr plan = NULL;
    static StringInfoData src = {0};
    elog(DEBUG1, "id = %li, output = %s, error = %s", t->shared->id, t->output.data ? t->output.data : task_null, t->error.data ? t->error.data : task_null);
    set_ps_display_my("done");
    if (!src.data) {
        initStringInfoMy(&src);
        appendStringInfo(&src, SQL(
            WITH s AS (SELECT "id" FROM %1$s AS t WHERE "id" = $1 FOR UPDATE OF t)
            UPDATE %1$s AS t SET "state" = 'DONE', "stop" = CURRENT_TIMESTAMP, "output" = $2, "error" = $3 FROM s WHERE t.id = s.id
            RETURNING "delete" AND "output" IS NULL AS "delete", "repeat" > '0 sec' AS "repeat", "max" >= 0 AND ("count" > 0 OR "live" > '0 sec') AS "live"
        ), work.schema_table);
    }
    SPI_connect_my(src.data);
    if (!plan) plan = SPI_prepare_my(src.data, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, nulls, SPI_OK_UPDATE_RETURNING);
    if (SPI_processed != 1) elog(WARNING, "id = %li, SPI_processed %lu != 1", t->shared->id, (long)SPI_processed); else {
        delete = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "delete", false));
        exit = !DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "live", false));
        repeat = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "repeat", false));
        elog(DEBUG1, "delete = %s, exit = %s, repeat = %s", delete ? "true" : "false", exit ? "true" : "false", repeat ? "true" : "false");
    }
    if (values[1]) pfree((void *)values[1]);
    if (values[2]) pfree((void *)values[2]);
    if (repeat) task_repeat(t);
    if (delete) task_delete(t);
    if (t->lock && !unlock_table_id(work.shared->oid, t->shared->id)) { elog(WARNING, "!unlock_table_id(%i, %li)", work.shared->oid, t->shared->id); exit = true; }
    t->lock = false;
    exit = exit || task_live(t);
    SPI_finish_my();
    task_free(t);
    set_ps_display_my("idle");
    return ShutdownRequestPending || exit;
}

bool task_work(Task *t) {
    bool exit = false;
    Datum values[] = {Int64GetDatum(t->shared->id), Int32GetDatum(t->pid)};
    static Oid argtypes[] = {INT8OID, INT4OID};
    static SPIPlanPtr plan = NULL;
    static StringInfoData src = {0};
    if (ShutdownRequestPending) return true;
    if (!lock_table_id(work.shared->oid, t->shared->id)) { elog(WARNING, "!lock_table_id(%i, %li)", work.shared->oid, t->shared->id); return true; }
    t->lock = true;
    t->count++;
    elog(DEBUG1, "id = %li, max = %i, oid = %i, count = %i, pid = %i", t->shared->id, t->shared->max, work.shared->oid, t->count, t->pid);
    set_ps_display_my("work");
    if (!t->conn) {
        StringInfoData id;
        initStringInfoMy(&id);
        appendStringInfo(&id, "%li", t->shared->id);
        set_config_option_my("pg_task.id", id.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR);
        pfree(id.data);
    }
    if (!src.data) {
        initStringInfoMy(&src);
        appendStringInfo(&src, SQL(
            WITH s AS (SELECT "id" FROM %1$s AS t WHERE "id" = $1 FOR UPDATE OF t)
            UPDATE %1$s AS t SET "state" = 'WORK', "start" = CURRENT_TIMESTAMP, "pid" = $2 FROM s WHERE t.id = s.id
            RETURNING "group", "hash", "input", EXTRACT(epoch FROM "timeout")::int * 1000 AS "timeout", "header", "string", "null", "delimiter", "quote", "escape", "plan" + "active" > CURRENT_TIMESTAMP AS "active", "remote"
        ), work.schema_table);
    }
    SPI_connect_my(src.data);
    if (!plan) plan = SPI_prepare_my(src.data, countof(argtypes), argtypes);
    SPI_execute_plan_my(plan, values, NULL, SPI_OK_UPDATE_RETURNING);
    if (SPI_processed != 1) {
        elog(WARNING, "id = %li, SPI_processed %lu != 1", t->shared->id, (long)SPI_processed);
        exit = true;
    } else {
        t->active = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "active", false));
        t->delimiter = DatumGetChar(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "delimiter", false));
        t->escape = DatumGetChar(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "escape", false));
        t->group = TextDatumGetCStringMy(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "group", false));
        t->shared->hash = DatumGetInt32(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "hash", false));
        t->header = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "header", false));
        t->input = TextDatumGetCStringMy(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "input", false));
        t->null = TextDatumGetCStringMy(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "null", false));
        t->quote = DatumGetChar(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "quote", false));
        t->remote = TextDatumGetCStringMy(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "remote", true));
        t->string = DatumGetBool(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "string", false));
        t->timeout = DatumGetInt32(SPI_getbinval_my(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, "timeout", false));
        if (0 < StatementTimeout && StatementTimeout < t->timeout) t->timeout = StatementTimeout;
        elog(DEBUG1, "group = %s, remote = %s, hash = %i, input = %s, timeout = %i, header = %s, string = %s, null = %s, delimiter = %c, quote = %c, escape = %c, active = %s", t->group, t->remote ? t->remote : task_null, t->shared->hash, t->input, t->timeout, t->header ? "true" : "false", t->string ? "true" : "false", t->null, t->delimiter, t->quote ? t->quote : 30, t->escape ? t->escape : 30, t->active ? "true" : "false");
        if (!t->remote) set_config_option_my("pg_task.group", t->group, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR);
    }
    SPI_finish_my();
    set_ps_display_my("idle");
    return exit;
}

void task_error(ErrorData *edata) {
    if ((emit_log_hook = emit_log_hook_prev)) (*emit_log_hook)(edata);
    if (!task.error.data) initStringInfoMy(&task.error);
    if (!task.output.data) initStringInfoMy(&task.output);
    appendStringInfo(&task.output, SQL(%sROLLBACK), task.output.len ? "\n" : "");
    task.skip++;
    if (task.error.len) appendStringInfoChar(&task.error, '\n');
    appendStringInfo(&task.error, "%s:  ", _(error_severity(edata->elevel)));
    if (Log_error_verbosity >= PGERROR_VERBOSE) appendStringInfo(&task.error, "%s: ", unpack_sql_state(edata->sqlerrcode));
    if (edata->message) append_with_tabs(&task.error, edata->message);
    else append_with_tabs(&task.error, _("missing error text"));
    if (edata->cursorpos > 0) appendStringInfo(&task.error, _(" at character %d"), edata->cursorpos);
    else if (edata->internalpos > 0) appendStringInfo(&task.error, _(" at character %d"), edata->internalpos);
    if (Log_error_verbosity >= PGERROR_DEFAULT) {
        if (edata->detail_log) {
            if (task.error.len) appendStringInfoChar(&task.error, '\n');
            appendStringInfoString(&task.error, _("DETAIL:  "));
            append_with_tabs(&task.error, edata->detail_log);
        } else if (edata->detail) {
            if (task.error.len) appendStringInfoChar(&task.error, '\n');
            appendStringInfoString(&task.error, _("DETAIL:  "));
            append_with_tabs(&task.error, edata->detail);
        }
        if (edata->hint) {
            if (task.error.len) appendStringInfoChar(&task.error, '\n');
            appendStringInfoString(&task.error, _("HINT:  "));
            append_with_tabs(&task.error, edata->hint);
        }
        if (edata->internalquery) {
            if (task.error.len) appendStringInfoChar(&task.error, '\n');
            appendStringInfoString(&task.error, _("QUERY:  "));
            append_with_tabs(&task.error, edata->internalquery);
        }
        if (edata->context
#if PG_VERSION_NUM >= 90500
            && !edata->hide_ctx
#endif
        ) {
            if (task.error.len) appendStringInfoChar(&task.error, '\n');
            appendStringInfoString(&task.error, _("CONTEXT:  "));
            append_with_tabs(&task.error, edata->context);
        }
        if (Log_error_verbosity >= PGERROR_VERBOSE) {
            if (edata->funcname && edata->filename) { // assume no newlines in funcname or filename...
                if (task.error.len) appendStringInfoChar(&task.error, '\n');
                appendStringInfo(&task.error, _("LOCATION:  %s, %s:%d"), edata->funcname, edata->filename, edata->lineno);
            } else if (edata->filename) {
                if (task.error.len) appendStringInfoChar(&task.error, '\n');
                appendStringInfo(&task.error, _("LOCATION:  %s:%d"), edata->filename, edata->lineno);
            }
        }
#if PG_VERSION_NUM >= 130000
        if (edata->backtrace) {
            if (task.error.len) appendStringInfoChar(&task.error, '\n');
            appendStringInfoString(&task.error, _("BACKTRACE:  "));
            append_with_tabs(&task.error, edata->backtrace);
        }
#endif
    }
    if (task.input && is_log_level_output(edata->elevel, log_min_error_statement) && !edata->hide_stmt) { // If the user wants the query that generated this error logged, do it.
        if (task.error.len) appendStringInfoChar(&task.error, '\n');
        appendStringInfoString(&task.error, _("STATEMENT:  "));
        append_with_tabs(&task.error, task.input);
    }
}

static void task_execute(void) {
    bool count = false;
    bool insert = false;
    char completionTag[COMPLETION_TAG_BUFSIZE];
    int rc = SPI_execute(task.input, false, 0);
    const char *tagname = SPI_result_code_string(rc) + sizeof("SPI_OK_") - 1;
    switch (rc) {
        case SPI_ERROR_ARGUMENT: ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("SPI_ERROR_ARGUMENT"))); break;
        case SPI_ERROR_COPY: ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("SPI_ERROR_COPY"))); break;
        case SPI_ERROR_OPUNKNOWN: ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("SPI_ERROR_OPUNKNOWN"))); break;
        case SPI_ERROR_TRANSACTION: ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("SPI_ERROR_TRANSACTION"))); break;
        case SPI_OK_DELETE: count = true; break;
        case SPI_OK_DELETE_RETURNING: count = true; break;
        case SPI_OK_INSERT: count = true; insert = true; break;
        case SPI_OK_INSERT_RETURNING: count = true; insert = true; break;
        case SPI_OK_SELECT: count = true; task.skip = 1; break;
        case SPI_OK_UPDATE: count = true; break;
        case SPI_OK_UPDATE_RETURNING: count = true; break;
    }
    elog(DEBUG1, "id = %li, commandTag = %s", task.shared->id, tagname);
    if (SPI_tuptable) for (uint64 row = 0; row < SPI_processed; row++) {
        task.skip = 1;
        if (!task.output.data) initStringInfoMy(&task.output);
        if (task.header && !row && SPI_tuptable->tupdesc->natts > 1) {
            if (task.output.len) appendStringInfoString(&task.output, "\n");
            for (int col = 1; col <= SPI_tuptable->tupdesc->natts; col++) {
                if (col > 1) appendStringInfoChar(&task.output, task.delimiter);
                appendBinaryStringInfoEscapeQuote(&task.output, SPI_fname(SPI_tuptable->tupdesc, col), strlen(SPI_fname(SPI_tuptable->tupdesc, col)), false, task.escape, task.quote);
            }
        }
        if (task.output.len) appendStringInfoString(&task.output, "\n");
        for (int col = 1; col <= SPI_tuptable->tupdesc->natts; col++) {
            char *value = SPI_getvalue(SPI_tuptable->vals[row], SPI_tuptable->tupdesc, col);
            if (col > 1) appendStringInfoChar(&task.output, task.delimiter);
            if (!value) appendStringInfoString(&task.output, task.null); else {
                appendBinaryStringInfoEscapeQuote(&task.output, value, strlen(value), !init_oid_is_string(SPI_gettypeid(SPI_tuptable->tupdesc, col)) && task.string, task.escape, task.quote);
                pfree(value);
            }
        }
    }
    if (count) snprintf(completionTag, COMPLETION_TAG_BUFSIZE, insert ? "%s 0 " UINT64_FORMAT : "%s " UINT64_FORMAT, tagname, SPI_processed);
    else snprintf(completionTag, COMPLETION_TAG_BUFSIZE, "%s", tagname);
    elog(DEBUG1, "id = %li, completionTag = %s", task.shared->id, completionTag);
    if (task.skip) task.skip = 0; else {
        if (!task.output.data) initStringInfoMy(&task.output);
        if (task.output.len) appendStringInfoString(&task.output, "\n");
        appendStringInfoString(&task.output, completionTag);
    }
}

static void task_proc_exit(int code, Datum arg) {
    elog(DEBUG1, "code = %i", code);
}

static void task_catch(void) {
    emit_log_hook_prev = emit_log_hook;
    emit_log_hook = task_error;
    EmitErrorReport();
    FlushErrorState();
}

static void task_latch(void) {
    ResetLatch(MyLatch);
    CHECK_FOR_INTERRUPTS();
}

static bool task_timeout(void) {
    int StatementTimeoutMy = StatementTimeout;
    if (task_work(&task)) return true;
    elog(DEBUG1, "id = %li, timeout = %i, input = %s, count = %i", task.shared->id, task.timeout, task.input, task.count);
    set_ps_display_my("timeout");
    StatementTimeout = task.timeout;
    SPI_connect_my(task.input);
    BeginInternalSubTransaction(NULL);
    PG_TRY();
        if (!task.active) ereport(ERROR, (errcode(ERRCODE_QUERY_CANCELED), errmsg("task not active")));
        task_execute();
        ReleaseCurrentSubTransaction();
    PG_CATCH();
        task_catch();
        RollbackAndReleaseCurrentSubTransaction();
    PG_END_TRY();
    SPI_finish_my();
    StatementTimeout = StatementTimeoutMy;
    pgstat_report_stat(false);
    pgstat_report_activity(STATE_IDLE, NULL);
    set_ps_display_my("idle");
    return task_done(&task);
}

void task_free(Task *t) {
    if (t->error.data) { pfree(t->error.data); t->error.data = NULL; t->error.len = 0; }
    if (t->group) { pfree(t->group); t->group = NULL; }
    if (t->input) { pfree(t->input); t->input = NULL; }
    if (t->null) { pfree(t->null); t->null = NULL; }
    if (t->output.data) { pfree(t->output.data); t->output.data = NULL; t->output.len = 0; }
    if (t->remote) { pfree(t->remote); t->remote = NULL; }
}

static void task_on_dsm_detach_callback(dsm_segment *seg, Datum arg) {
    elog(DEBUG1, "seg = %u", dsm_segment_handle(seg));
}

void task_main(Datum arg) {
    const char *application_name;
    shm_toc *toc;
    StringInfoData oid, schema_table;
    on_proc_exit(task_proc_exit, (Datum)NULL);
    BackgroundWorkerUnblockSignals();
    CreateAuxProcessResourceOwner();
    if (!(task.seg = dsm_attach(DatumGetUInt32(arg)))) ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("unable to map dynamic shared memory segment")));
    on_dsm_detach(task.seg, task_on_dsm_detach_callback, (Datum)NULL);
    if (!(toc = shm_toc_attach(PG_TASK_MAGIC, dsm_segment_address(task.seg)))) ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("bad magic number in dynamic shared memory segment")));
    task.shared = shm_toc_lookup_my(toc, 0);
    if (!(work.seg = dsm_attach(task.shared->handle))) ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("unable to map dynamic shared memory segment")));
    on_dsm_detach(work.seg, task_on_dsm_detach_callback, (Datum)NULL);
    if (!(toc = shm_toc_attach(PG_WORK_MAGIC, dsm_segment_address(work.seg)))) ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("bad magic number in dynamic shared memory segment")));
    work.shared = shm_toc_lookup_my(toc, 0);
    work.data = quote_identifier(work.shared->data);
    work.schema = quote_identifier(work.shared->schema);
    work.table = quote_identifier(work.shared->table);
    work.user = quote_identifier(work.shared->user);
    if (kill(MyBgworkerEntry->bgw_notify_pid, SIGUSR2)) ereport(ERROR, (errmsg("could not send signal SIGUSR2 to process %d: %m", MyBgworkerEntry->bgw_notify_pid)));
    BackgroundWorkerInitializeConnectionMy(work.shared->data, work.shared->user);
    CurrentResourceOwner = AuxProcessResourceOwner;
    MemoryContextSwitchTo(TopMemoryContext);
    application_name = MyBgworkerEntry->bgw_name + strlen(work.shared->user) + 1 + strlen(work.shared->data) + 1;
    set_config_option_my("application_name", application_name, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR);
    pgstat_report_appname(application_name);
    set_ps_display_my("main");
    process_session_preload_libraries();
    elog(DEBUG1, "oid = %i, id = %li, hash = %i, max = %i", work.shared->oid, task.shared->id, task.shared->hash, task.shared->max);
    set_config_option_my("pg_task.schema", work.shared->schema, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR);
    set_config_option_my("pg_task.table", work.shared->table, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR);
    if (!MessageContext) MessageContext = AllocSetContextCreate(TopMemoryContext, "MessageContext", ALLOCSET_DEFAULT_SIZES);
    initStringInfoMy(&schema_table);
    appendStringInfo(&schema_table, "%s.%s", work.schema, work.table);
    work.schema_table = schema_table.data;
    initStringInfoMy(&oid);
    appendStringInfo(&oid, "%i", work.shared->oid);
    set_config_option_my("pg_task.oid", oid.data, PGC_USERSET, PGC_S_SESSION, GUC_ACTION_SET, true, ERROR);
    pfree(oid.data);
    task.pid = MyProcPid;
    task.start = GetCurrentTimestamp();
    set_ps_display_my("idle");
    if (!lock_table_pid_hash(work.shared->oid, task.pid, task.shared->hash)) { elog(WARNING, "!lock_table_pid_hash(%i, %i, %i)", work.shared->oid, task.pid, task.shared->hash); return; }
    while (!ShutdownRequestPending) {
        int rc = WaitLatchMy(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_POSTMASTER_DEATH, 0);
        if (rc & WL_TIMEOUT) if (task_timeout()) ShutdownRequestPending = true;
        if (rc & WL_LATCH_SET) task_latch();
        if (rc & WL_POSTMASTER_DEATH) ShutdownRequestPending = true;
    }
    if (!unlock_table_pid_hash(work.shared->oid, task.pid, task.shared->hash)) elog(WARNING, "!unlock_table_pid_hash(%i, %i, %i)", work.shared->oid, task.pid, task.shared->hash);
}
