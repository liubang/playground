## 算法

### LRU

```java 
public class LRUCache {
    private LinkedHashMap<Integer, Integer> map;
    private final int CAPACITY;

    public LRUCache(int capacity) {
        CAPACITY = capacity;
        map = new LinkedHashMap<Integer, Integer>(capacity, 0.75f, true) {
            @Override
            protected boolean removeEldestEntry(Map.Entry<Integer, Integer> eldest) {
                return size() > CAPACITY;
            }
        };
    }

    public Map<Integer, Integer> get() {
        return map;
    }

    public Integer get(Integer key) {
        return map.get(key);
    }

    public void set(Integer key, Integer val) {
        map.put(key, val);
    }

    public static void main(String[] args) {
        LRUCache lruCache = new LRUCache(10);
        for (int i = 0; i < 100; i++) {
            lruCache.set(i, i);
        }
        Map<Integer, Integer> map = lruCache.get();
        for (Map.Entry<Integer, Integer> entry : map.entrySet()) {
            System.out.println(entry.getKey() + ":" + entry.getValue());
        }
    }
}
```