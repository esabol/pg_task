PostgreSQL and Greenplum job scheduler pg_task allows to execute any sql command at any specific time at background asynchronously

# pg_task config
to run pg_task add it to line
```conf
shared_preload_libraries = 'pg_task'
```

by default pg_task
1) executes
```conf
pg_task.count = 1000
```
tasks until exit

2) uses database
```conf
pg_task.data = 'postgres'
```
3) deletes task if output is null
```conf
pg_task.delete = 'on'
```
4) uses in output delimiter
```conf
pg_task.delimiter = '\t'
```
5) uses drift
```conf
pg_task.drift = 'on'
```
6) groupes tasks by
```conf
pg_task.group = 'group'
```
7) prints headers in output
```conf
pg_task.header = 'on'
```
8) processes tasks
```conf
pg_task.live = '1 hour'
```
before exit

9) executes simultaniously
```conf
pg_task.max = 0
```
tasks

10) prints null in output as
```conf
pg_task.null = '\N'
```

11) uses schema
```conf
pg_task.schema = 'public'
```
for tasks

12) prints only strings in quotes in output
```conf
pg_task.string = 'on'
```
13) uses table
```conf
pg_task.table = 'task'
```
for tasks

14) uses sleep timeout
```conf
pg_task.sleep = 1000
```
milliseconds

15) uses user
```conf
pg_task.user = 'postgres'
```

by default pg_task run on default database with default user with default schema with default table with default timeout

to run specific database and/or specific user and/or specific schema and/or specific table and/or specific timeout, set config (in json format)
```conf
pg_task.json = '[{"data":"database1"},{"data":"database2","user":"username2"},{"data":"database3","schema":"schema3"},{"data":"database4","table":"table4"},{"data":"database5","timeout":100}]'
```

if database and/or user and/or schema and/or table does not exist then pg_task create it/their

# pg_task using

by default pg_task create table with folowing columns

id bigserial - primary key

parent bigint - parent task (if exists)

plan timestamp - planned time of start

start timestamp - actual time of start

stop timestamp - actual time of stop

group text - task groupping

max int - maximum concurently tasks in group

pid int - id of process executing task

input text - sql to execute

output text - received result

error text - occured error

state state - PLAN, TAKE, WORK, DONE or STOP

timeout interval - allowed time to run

delete boolean - autodelete (if output is null)

repeat interval - autorepeat interval

drift boolean - see below

count integer - maximum tasks executed by current worker

live interval - maximum time of live of current worker

remote text - connect to remote database (if need)

but you may add any needed colums and/or make partitions

to run task more quickly execute sql command
```sql
INSERT INTO task (input) VALUES ('SELECT now()')
```

to run task after 5 minutes write plannded time
```sql
INSERT INTO task (plan, input) VALUES (now() + '5 min':INTERVAL, 'SELECT now()')
```

to run task at specific time so write
```sql
INSERT INTO task (plan, input) VALUES ('2029-07-01 12:51:00', 'SELECT now()')
```

to repeat task every 5 minutes write
```sql
INSERT INTO task (repeat, input) VALUES ('5 min', 'SELECT now()')
```

if write so
```sql
INSERT INTO task (repeat, input, drift) VALUES ('5 min', 'SELECT now()', false)
```
then repeat task will start after 5 minutes after task done (instead after planned time as default)

if exception occures it catched and writed in error as text
```sql
INSERT INTO task (input) VALUES ('SELECT 1/0')
```

if some group needs concurently run only 2 tasks then use command
```sql
INSERT INTO task (group, max, input) VALUES ('group', 1, 'SELECT now()')
```

if in this group there are more tasks and they are executing concurently by 2 then command
```sql
INSERT INTO task (group, max, input) VALUES ('group', 2, 'SELECT now()')
```
will execute task as more early in this group (as like priority)

to run task on remote database use sql command
```sql
INSERT INTO task (input, remote) VALUES ('SELECT now()', 'user=user host=host')
```
