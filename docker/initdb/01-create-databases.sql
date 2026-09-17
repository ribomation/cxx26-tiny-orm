-- Extra databases, created by initdb on first start only -- the same semantics
-- as POSTGRES_DB, which can name just one.
--
-- The demo and the integration tests are kept apart on purpose: both create and
-- drop tables, so sharing one database would let a test run wipe a loaded demo
-- and a demo run confuse a test.
CREATE DATABASE tiny_orm_demo;
