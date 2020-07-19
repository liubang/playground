package cn.iliubang.exercises.redislock;

public interface Lock {
    boolean lock(String resource, int ttl);

    boolean unlock(String resource);
}
