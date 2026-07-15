package io.github.liubang.spark;

import org.apache.spark.sql.execution.datasources.csv.CSVFileFormat;
import org.apache.spark.sql.sources.DataSourceRegister;

/** CSV-compatible file format whose default delimiter is a pipe character. */
public final class PipeFileFormat extends CSVFileFormat implements DataSourceRegister {
    @Override
    public String shortName() {
        return "pipe";
    }
}
