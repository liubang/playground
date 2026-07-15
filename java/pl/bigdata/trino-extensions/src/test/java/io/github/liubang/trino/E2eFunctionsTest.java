package io.github.liubang.trino;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;

import io.airlift.slice.Slices;
import org.junit.jupiter.api.Test;

class E2eFunctionsTest {
    @Test
    void prefixesText() {
        assertEquals("e2e:hello", E2eFunctions.prefix(Slices.utf8Slice("hello")).toStringUtf8());
    }

    @Test
    void preservesNull() {
        assertNull(E2eFunctions.prefix(null));
    }
}
