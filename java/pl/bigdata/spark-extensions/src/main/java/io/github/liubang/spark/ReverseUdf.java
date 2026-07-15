package io.github.liubang.spark;

import org.apache.hadoop.hive.ql.exec.UDF;
import org.apache.hadoop.io.Text;

/** Hive-style UDF suitable for Spark SQL CREATE FUNCTION. */
public final class ReverseUdf extends UDF {
    public Text evaluate(Text value) {
        if (value == null) {
            return null;
        }
        return new Text(new StringBuilder(value.toString()).reverse().toString());
    }
}
