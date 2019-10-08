-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION pg_self_query" to load this file. \quit

CREATE FUNCTION pg_self_query(pid integer)
	RETURNS TABLE (pid integer, frame_number integer, query_text text)
	AS 'MODULE_PATHNAME'
	LANGUAGE C STRICT VOLATILE;
