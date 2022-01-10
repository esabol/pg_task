void BeginCommandMy(const char *commandTag, CommandDest dest) {
    elog(DEBUG1, "id = %li, commandTag = %s", task->id, commandTag);
}

void EndCommandMy(const char *commandTag, CommandDest dest) {
    elog(DEBUG1, "id = %li, commandTag = %s", task->id, commandTag);
    if (task->skip) task->skip = 0; else {
        if (!task->output.data) initStringInfoMy(TopMemoryContext, &task->output);
        if (task->output.len) appendStringInfoString(&task->output, "\n");
        appendStringInfoString(&task->output, commandTag);
    }
}