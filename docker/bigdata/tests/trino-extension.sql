SELECT IF(e2e_prefix('hello') = 'e2e:hello', true, fail('e2e_prefix returned an unexpected value'));
SELECT IF(e2e_prefix(CAST(NULL AS varchar)) IS NULL, true, fail('e2e_prefix must preserve NULL'));
SELECT IF(
    count(*) = 10000 AND count_if(e2e_prefix(CAST(n AS varchar)) = 'e2e:' || CAST(n AS varchar)) = 10000,
    true,
    fail('bulk e2e_prefix validation failed'))
FROM UNNEST(sequence(1, 10000)) AS t(n);
SELECT IF(
    count(*) = 2 AND count_if(state = 'active') = 2 AND count_if(coordinator) = 1,
    true,
    fail('expected one active coordinator and one active worker'))
FROM system.runtime.nodes;
SELECT IF(
    array_sort(array_agg(catalog_name)) = ARRAY['hive', 'iceberg', 'mysql', 'system'],
    true,
    fail('catalog set is incomplete'))
FROM system.metadata.catalogs;
