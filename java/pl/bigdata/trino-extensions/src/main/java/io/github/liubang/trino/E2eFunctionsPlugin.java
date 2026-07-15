package io.github.liubang.trino;

import io.trino.spi.Plugin;
import java.util.Set;

public final class E2eFunctionsPlugin implements Plugin {
    @Override
    public Set<Class<?>> getFunctions() {
        return Set.of(E2eFunctions.class);
    }
}
