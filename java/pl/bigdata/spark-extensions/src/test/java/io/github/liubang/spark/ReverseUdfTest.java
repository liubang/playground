package io.github.liubang.spark;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;

import org.apache.hadoop.io.Text;
import org.junit.jupiter.api.Test;

class ReverseUdfTest {
    private final ReverseUdf udf = new ReverseUdf();

    @Test
    void reversesText() {
        assertEquals("0001-wor", udf.evaluate(new Text("row-1000")).toString());
    }

    @Test
    void preservesNull() {
        assertNull(udf.evaluate(null));
    }
}
