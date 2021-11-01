#include "include.h"

PG_MODULE_MAGIC;

char *default_null;
static char *default_data;
static char *default_json;
static char *default_live;
static char *default_partman;
static char *default_schema;
static char *default_table;
static char *default_user;
static int default_count;
static int default_timeout;

static bool init_check_ascii(char *data) {
    for (char *ch = data; *ch; ch++) {
        if (32 <= *ch && *ch <= 127) continue;
        if (*ch == '\n' || *ch == '\r' || *ch == '\t') continue;
        return true;
    }
    return false;
}

bool init_check_ascii_all(BackgroundWorker *worker) {
#if PG_VERSION_NUM >= 110000
    if (init_check_ascii(worker->bgw_type)) return true;
#endif
    if (init_check_ascii(worker->bgw_name)) return true;
    return false;
}

bool init_data_user_table_lock(Oid data, Oid user, Oid table) {
    LOCKTAG tag = {data, user, table, 3, LOCKTAG_USERLOCK, USER_LOCKMETHOD};
    return LockAcquire(&tag, AccessExclusiveLock, true, true) != LOCKACQUIRE_NOT_AVAIL;
}

bool init_data_user_table_unlock(Oid data, Oid user, Oid table) {
    LOCKTAG tag = {data, user, table, 3, LOCKTAG_USERLOCK, USER_LOCKMETHOD};
    return LockRelease(&tag, AccessExclusiveLock, true);
}

bool init_oid_is_string(Oid oid) {
    switch (oid) {
        case BITOID:
        case BOOLOID:
        case CIDOID:
        case FLOAT4OID:
        case FLOAT8OID:
        case INT2OID:
        case INT4OID:
        case INT8OID:
        case NUMERICOID:
        case OIDOID:
        case TIDOID:
        case XIDOID:
            return false;
        default: return true;
    }
}

bool init_table_id_lock(Oid table, int64 id) {
    LOCKTAG tag = {table, (uint32)(id >> 32), (uint32)id, 4, LOCKTAG_USERLOCK, USER_LOCKMETHOD};
    return LockAcquire(&tag, AccessExclusiveLock, true, true) != LOCKACQUIRE_NOT_AVAIL;
}

bool init_table_id_unlock(Oid table, int64 id) {
    LOCKTAG tag = {table, (uint32)(id >> 32), (uint32)id, 4, LOCKTAG_USERLOCK, USER_LOCKMETHOD};
    return LockRelease(&tag, AccessExclusiveLock, true);
}

bool init_table_pid_hash_lock(Oid table, int pid, int hash) {
    LOCKTAG tag = {table, (uint32)pid, (uint32)hash, 5, LOCKTAG_USERLOCK, USER_LOCKMETHOD};
    return LockAcquire(&tag, AccessShareLock, true, true) != LOCKACQUIRE_NOT_AVAIL;
}

bool init_table_pid_hash_unlock(Oid table, int pid, int hash) {
    LOCKTAG tag = {table, (uint32)pid, (uint32)hash, 5, LOCKTAG_USERLOCK, USER_LOCKMETHOD};
    return LockRelease(&tag, AccessShareLock, true);
}

static char *text_to_cstring_my(MemoryContext memoryContext, const text *t) {
    MemoryContext oldMemoryContext = MemoryContextSwitchTo(memoryContext);
    char *result = text_to_cstring(t);
    MemoryContextSwitchTo(oldMemoryContext);
    return result;
}

char *TextDatumGetCStringMy(MemoryContext memoryContext, Datum datum) {
    return datum ? text_to_cstring_my(memoryContext, (text *)DatumGetPointer(datum)) : NULL;
}

static text *cstring_to_text_my(MemoryContext memoryContext, const char *s) {
    MemoryContext oldMemoryContext = MemoryContextSwitchTo(memoryContext);
    text *result = cstring_to_text(s);
    MemoryContextSwitchTo(oldMemoryContext);
    return result;
}

Datum CStringGetTextDatumMy(MemoryContext memoryContext, const char *s) {
    return s ? PointerGetDatum(cstring_to_text_my(memoryContext, s)) : (Datum)NULL;
}

void init_escape(StringInfoData *buf, const char *data, int len, char escape) {
    for (int i = 0; len-- > 0; i++) {
        if (escape == data[i]) appendStringInfoChar(buf, escape);
        appendStringInfoChar(buf, data[i]);
    }
}

static void init_work(bool dynamic) {
    BackgroundWorker worker;
    MemSet(&worker, 0, sizeof(worker));
    if (strlcpy(worker.bgw_function_name, "conf_main", sizeof(worker.bgw_function_name)) >= sizeof(worker.bgw_function_name)) E("strlcpy");
    if (strlcpy(worker.bgw_library_name, "pg_task", sizeof(worker.bgw_library_name)) >= sizeof(worker.bgw_library_name)) E("strlcpy");
    if (snprintf(worker.bgw_name, sizeof(worker.bgw_name) - 1, "postgres postgres pg_conf") >= sizeof(worker.bgw_name) - 1) E("snprintf");
#if PG_VERSION_NUM >= 110000
    if (strlcpy(worker.bgw_type, "pg_conf", sizeof(worker.bgw_type)) >= sizeof(worker.bgw_type)) E("strlcpy");
#endif
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_restart_time = BGW_DEFAULT_RESTART_INTERVAL;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    if (init_check_ascii_all(&worker)) E("init_check_ascii_all");
    if (dynamic) {
        IsUnderPostmaster = true;
        if (!RegisterDynamicBackgroundWorker(&worker, NULL)) E("!RegisterDynamicBackgroundWorker");
        IsUnderPostmaster = false;
    } else RegisterBackgroundWorker(&worker);
}

static void init_assign(const char *newval, void *extra) {
    bool new_isnull, old_isnull;
    const char *oldval;
    if (PostmasterPid != MyProcPid) return;
    if (process_shared_preload_libraries_in_progress) return;
    oldval = GetConfigOption("pg_task.json", true, true);
    old_isnull = !oldval || oldval[0] == '\0';
    new_isnull = !newval || newval[0] == '\0';
    if (old_isnull && new_isnull) return;
    if (!old_isnull && !new_isnull && !strcmp(oldval, newval)) return;
    D1("oldval = %s, newval = %s", !old_isnull ? oldval : default_null, !new_isnull ? newval : default_null);
    init_work(true);
}

static void init_conf(void) {
    DefineCustomIntVariable("pg_task.default_count", "pg_task default count", NULL, &default_count, 1000, 0, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomIntVariable("pg_task.default_timeout", "pg_task default timeout", NULL, &default_timeout, 1000, 1, INT_MAX, PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("pg_task.default_data", "pg_task default data", NULL, &default_data, "postgres", PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("pg_task.default_live", "pg_task default live", NULL, &default_live, "1 hour", PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("pg_task.default_null", "pg_task default null", NULL, &default_null, "\\N", PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("pg_task.default_partman", "pg_task default partman", NULL, &default_partman, NULL, PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("pg_task.default_schema", "pg_task default schema", NULL, &default_schema, "public", PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("pg_task.default_table", "pg_task default table", NULL, &default_table, "task", PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("pg_task.default_user", "pg_task default user", NULL, &default_user, "postgres", PGC_SIGHUP, 0, NULL, NULL, NULL);
    DefineCustomStringVariable("pg_task.json", "pg_task json", NULL, &default_json, SQL([{"data":"postgres"}]), PGC_SIGHUP, 0, NULL, init_assign, NULL);
    D1("json = %s, schema = %s, table = %s, null = %s, timeout = %i, count = %i, live = %s, partman = %s", default_json, default_schema, default_table, default_null, default_timeout, default_count, default_live, default_partman ? default_partman : default_null);
}

void initStringInfoMy(MemoryContext memoryContext, StringInfoData *buf) {
    MemoryContext oldMemoryContext = MemoryContextSwitchTo(memoryContext);
    initStringInfo(buf);
    MemoryContextSwitchTo(oldMemoryContext);
}

void _PG_init(void) {
    if (IsBinaryUpgrade) { W("IsBinaryUpgrade"); return; }
    if (!process_shared_preload_libraries_in_progress) F("!process_shared_preload_libraries_in_progress");
    init_conf();
    init_work(false);
}

#if PG_VERSION_NUM >= 130000
#else
volatile sig_atomic_t ShutdownRequestPending;

void
SignalHandlerForConfigReload(SIGNAL_ARGS)
{
	int			save_errno = errno;

	ConfigReloadPending = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

void
SignalHandlerForShutdownRequest(SIGNAL_ARGS)
{
	int			save_errno = errno;

	ShutdownRequestPending = true;
	SetLatch(MyLatch);

	errno = save_errno;
}
#endif
