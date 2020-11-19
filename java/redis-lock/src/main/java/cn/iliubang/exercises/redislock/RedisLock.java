package cn.iliubang.exercises.redislock;

import redis.clients.jedis.Jedis;
import redis.clients.jedis.JedisPool;
import redis.clients.jedis.JedisPoolConfig;
import redis.clients.jedis.params.SetParams;

import java.util.Collections;

public class RedisLock implements Lock {

    private static JedisPool jedisPool;
    private String uuid;

    public RedisLock() {
        if (null == jedisPool) {
            synchronized (this) {
                if (null == jedisPool) {
                    JedisPoolConfig jedisPoolConfig = new JedisPoolConfig();
                    jedisPoolConfig.setMaxWaitMillis(2000000);
                    jedisPool = new JedisPool(jedisPoolConfig, "localhost", 6379);
                }
            }
        }

        uuid = "" + hashCode();
    }

    @Override
    public boolean lock(String resource, int ttl) {
        Jedis jedis = jedisPool.getResource();
        if (null == jedis) {
            return false;
        }
        SetParams setParams = new SetParams();
        setParams.nx();
        setParams.ex(ttl);
        String result = jedis.set(resource, uuid, setParams);
        return "OK".equals(result);
    }

    @Override
    public boolean unlock(String resource) {

        String lua =
                "if redis.call(\"get\",KEYS[1]) == ARGV[1] then\n" +
                        "    return redis.call(\"del\",KEYS[1])\n" +
                        "else\n" +
                        "    return 0\n" +
                        "end";
        Jedis jedis = jedisPool.getResource();
        if (null == jedis) {
            return false;
        }

        Object result = jedis.eval(lua, Collections.singletonList(resource), Collections.singletonList(uuid));
        if (result instanceof String && "OK".equals(result)) {
            return true;
        }

        return false;
    }
}
