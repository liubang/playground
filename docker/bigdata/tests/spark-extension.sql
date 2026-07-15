DROP DATABASE IF EXISTS e2e_extension CASCADE;
CREATE DATABASE e2e_extension;

CREATE TABLE e2e_extension.pipe_ctas
USING io.github.liubang.spark.PipeFileFormat
OPTIONS (delimiter='|', header='true')
AS
SELECT id, concat('row-', id) AS value
FROM range(1, 1001);

SELECT assert_true(count(*) = 1000, concat('unexpected row count: ', count(*)))
FROM e2e_extension.pipe_ctas;
SELECT assert_true(sum(id) = 500500, concat('unexpected id sum: ', sum(id)))
FROM e2e_extension.pipe_ctas;
SELECT assert_true(
    max(CASE WHEN id IN (1, 42, 1000) AND value != concat('row-', id) THEN 1 ELSE 0 END) = 0,
    'pipe format round-trip failed')
FROM e2e_extension.pipe_ctas;

CREATE TEMPORARY FUNCTION reverse_e2e AS 'io.github.liubang.spark.ReverseUdf';
SELECT assert_true(
    count(*) = 1000,
    concat('temporary UDF validation failed: ', count(*)))
FROM e2e_extension.pipe_ctas
WHERE reverse_e2e(value) = reverse(value);

DROP FUNCTION IF EXISTS e2e_extension.reverse_persistent_e2e;
CREATE FUNCTION e2e_extension.reverse_persistent_e2e
AS 'io.github.liubang.spark.ReverseUdf';
SELECT assert_true(
    e2e_extension.reverse_persistent_e2e('persistent') = 'tnetsisrep',
    'persistent UDF failed in creation session');
