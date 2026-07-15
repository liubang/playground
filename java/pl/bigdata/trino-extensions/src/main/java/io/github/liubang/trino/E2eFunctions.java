package io.github.liubang.trino;

import static io.trino.spi.type.StandardTypes.VARCHAR;

import io.airlift.slice.Slice;
import io.airlift.slice.Slices;
import io.trino.spi.function.ScalarFunction;
import io.trino.spi.function.SqlNullable;
import io.trino.spi.function.SqlType;

public final class E2eFunctions {
    private E2eFunctions() {}

    @ScalarFunction("e2e_prefix")
    @SqlNullable
    @SqlType(VARCHAR)
    public static Slice prefix(@SqlNullable @SqlType(VARCHAR) Slice value) {
        if (value == null) {
            return null;
        }
        return Slices.utf8Slice("e2e:" + value.toStringUtf8());
    }
}
