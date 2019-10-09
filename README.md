# Fork info
`pg_self_query` is based on the `pg_query_state` extension and is necessary for one purpose - to receive the current (not the top level like `current_query()`) query for saving it in the audit system. It is used in audit triggers.

The `pg_self_query` function does not accept any parameters, since it is called only for the current PID. 
Also, unlike `pg_query_state`, it does not return anything except the frame number and the query (previous frame from the current one) for writing to the log. 
Thus, it is easy to capture a query, which actually changes the data in tables.
So, you can use this function in your audit triggers.

## Use cases
Using this module there can help in the following things:
 - log (in audit triggers) queries, which actually changes the data in tables

## Installation
To install `pg_self_query`, please apply corresponding patches `runtime_explain.patch` (or `runtime_explain_11.0.patch` for PG11) to reqired stable version of PostgreSQL and rebuild PostgreSQL.


Then execute this in the module's directory:
```
make install USE_PGXS=1
```
Add module name to the `shared_preload_libraries` parameter in `postgresql.conf`:
```
shared_preload_libraries = 'pg_self_query'
```
It is essential to restart the PostgreSQL instance. After that, execute the following query in psql:
```
CREATE EXTENSION pg_self_query;
```
Done!

## Function pg\_self\_query\
```plpgsql
pg_self_query()
	returns TABLE ( frame_number integer,
	                query_text text)
```
Extract current query state from the current `pid`.