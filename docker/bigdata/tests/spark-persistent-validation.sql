SELECT assert_true(
    e2e_extension.reverse_persistent_e2e('row-321') = '123-wor',
    'persistent UDF failed in a new Spark SQL session');
SELECT assert_true(count(*) = 1000, 'table missing in new Spark SQL session')
FROM e2e_extension.pipe_ctas;
