## 网络编程实践

### TTCP

**1. What performance do we care?**

- Bandwidth, MB/s
- Throughput, messages/s, queries/s(QPS), transactions/s(TPS)
- Latency, milliseconds, percentiles
- Utilization, percent, payload vs. carrier, goodput vs. theory BW
- Overhead, eg. CPU usage, for compression and/or encryption

**2. Why do we re-implement TTCP?**

- It uses all basic Sockets APIs: `socket`, `listen`, `bind`, `accept`, `connect`, `read/recv`, `write/send`, `shutdown`, `close`, etc.
- The protocol is binary, not just byte stream, so it's better than the classic "echo" example.
- Typical behaviors, meaningful results, instead of packets/s.
- Service as benchmark for programming language as well, by comparing CPU usage.
- Not concurrent, at least in the very basic form.
