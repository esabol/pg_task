#include "include.h"

static bool was_logged;

static void check_log_statement(const char *src) {
    if (log_statement == LOGSTMT_NONE) was_logged = false;
    else if (log_statement == LOGSTMT_ALL) was_logged = true;
    else was_logged = false;
    debug_query_string = src;
    SetCurrentStatementStartTimestamp();
    if (was_logged) ereport(LOG, (errmsg("statement: %s", src), errhidestmt(true)));
}

static void check_log_duration_my(const char *src) {
    char msec_str[32];
    switch (check_log_duration(msec_str, was_logged)) {
        case 1: ereport(LOG, (errmsg("duration: %s ms", msec_str), errhidestmt(true))); break;
        case 2: ereport(LOG, (errmsg("duration: %s ms  statement: %s", msec_str, src), errhidestmt(true))); break;
    }
    debug_query_string = NULL;
    was_logged = false;
}

Datum SPI_getbinval_my(HeapTupleData *tuple, TupleDesc tupdesc, const char *fname, bool allow_null) {
    bool isnull;
    Datum datum = SPI_getbinval(tuple, tupdesc, SPI_fnumber(tupdesc, fname), &isnull);
    if (allow_null) return datum;
    if (isnull) ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED), errmsg("column \"%s\" must not be null", fname)));
    return datum;
}

Portal SPI_cursor_open_my(const char *src, SPIPlanPtr plan, Datum *values, const char *nulls) {
    Portal portal;
    SPI_freetuptable(SPI_tuptable);
    check_log_statement(src);
    CurrentResourceOwner = SPIResourceOwner;
    if (!(portal = SPI_cursor_open(src, plan, values, nulls, false))) ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_cursor_open failed"), errdetail("%s", SPI_result_code_string(SPI_result))));
    CurrentResourceOwner = AuxProcessResourceOwner;
    MemoryContextSwitchTo(TopMemoryContext);
    return portal;
}

Portal SPI_cursor_open_with_args_my(const char *src, int nargs, Oid *argtypes, Datum *values, const char *nulls) {
    Portal portal;
    SPI_freetuptable(SPI_tuptable);
    check_log_statement(src);
    CurrentResourceOwner = SPIResourceOwner;
    if (!(portal = SPI_cursor_open_with_args(src, src, nargs, argtypes, values, nulls, false, 0))) ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_cursor_open_with_args failed"), errdetail("%s", SPI_result_code_string(SPI_result)), errcontext("%s", src)));
    CurrentResourceOwner = AuxProcessResourceOwner;
    MemoryContextSwitchTo(TopMemoryContext);
    return portal;
}

SPIPlanPtr SPI_prepare_my(const char *src, int nargs, Oid *argtypes) {
    int rc;
    SPIPlanPtr plan;
    check_log_statement(src);
    CurrentResourceOwner = SPIResourceOwner;
    if (!(plan = SPI_prepare(src, nargs, argtypes))) ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_prepare failed"), errdetail("%s", SPI_result_code_string(SPI_result)), errcontext("%s", src)));
    if ((rc = SPI_keepplan(plan))) ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_keepplan failed"), errdetail("%s", SPI_result_code_string(rc)), errcontext("%s", src)));
    CurrentResourceOwner = AuxProcessResourceOwner;
    MemoryContextSwitchTo(TopMemoryContext);
    check_log_duration_my(src);
    return plan;
}

void SPI_connect_my(const char *src) {
    int rc;
    debug_query_string = src;
    pgstat_report_activity(STATE_RUNNING, src);
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();
    if ((rc = SPI_connect()) != SPI_OK_CONNECT) ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_connect failed"), errdetail("%s", SPI_result_code_string(rc)), errcontext("%s", src)));
    PushActiveSnapshot(GetTransactionSnapshot());
    StatementTimeout > 0 ? enable_timeout_after(STATEMENT_TIMEOUT, StatementTimeout) : disable_timeout(STATEMENT_TIMEOUT, false);
    SPIResourceOwner = CurrentResourceOwner;
    CurrentResourceOwner = AuxProcessResourceOwner;
    MemoryContextSwitchTo(TopMemoryContext);
}

void SPI_cursor_close_my(const char *src, Portal portal) {
    CurrentResourceOwner = SPIResourceOwner;
    SPI_freetuptable(SPI_tuptable);
    SPI_cursor_close(portal);
    CurrentResourceOwner = AuxProcessResourceOwner;
    MemoryContextSwitchTo(TopMemoryContext);
    check_log_duration_my(src);
}

void SPI_cursor_fetch_my(const char *src, Portal portal, bool forward, long count) {
    CurrentResourceOwner = SPIResourceOwner;
    SPI_freetuptable(SPI_tuptable);
    SPI_cursor_fetch(portal, forward, count);
    CurrentResourceOwner = AuxProcessResourceOwner;
    MemoryContextSwitchTo(TopMemoryContext);
}

void SPI_execute_plan_my(const char *src, SPIPlanPtr plan, Datum *values, const char *nulls, int res) {
    int rc;
    SPI_freetuptable(SPI_tuptable);
    check_log_statement(src);
    CurrentResourceOwner = SPIResourceOwner;
    if ((rc = SPI_execute_plan(plan, values, nulls, false, 0)) != res) ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_execute_plan failed"), errdetail("%s while expecting %s", SPI_result_code_string(rc), SPI_result_code_string(res))));
    CurrentResourceOwner = AuxProcessResourceOwner;
    MemoryContextSwitchTo(TopMemoryContext);
    check_log_duration_my(src);
}

void SPI_execute_with_args_my(const char *src, int nargs, Oid *argtypes, Datum *values, const char *nulls, int res) {
    int rc;
    SPI_freetuptable(SPI_tuptable);
    check_log_statement(src);
    CurrentResourceOwner = SPIResourceOwner;
    if ((rc = SPI_execute_with_args(src, nargs, argtypes, values, nulls, false, 0)) != res) ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_execute_with_args failed"), errdetail("%s while expecting %s", SPI_result_code_string(rc), SPI_result_code_string(res)), errcontext("%s", src)));
    CurrentResourceOwner = AuxProcessResourceOwner;
    MemoryContextSwitchTo(TopMemoryContext);
    check_log_duration_my(src);
}

void SPI_finish_my(void) {
    int rc;
    disable_timeout(STATEMENT_TIMEOUT, false);
    PopActiveSnapshot();
    if ((rc = SPI_finish()) != SPI_OK_FINISH) ereport(ERROR, (errcode(ERRCODE_INTERNAL_ERROR), errmsg("SPI_finish failed"), errdetail("%s", SPI_result_code_string(rc))));
#if PG_VERSION_NUM < 150000
    ProcessCompletedNotifies();
#endif
    CommitTransactionCommand();
    was_logged = false;
    pgstat_report_stat(false);
    debug_query_string = NULL;
    pgstat_report_activity(STATE_IDLE, NULL);
    SPIResourceOwner = NULL;
    CurrentResourceOwner = AuxProcessResourceOwner;
    MemoryContextSwitchTo(TopMemoryContext);
}
