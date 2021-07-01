## cgo导出c库

```bash 
# 导出静态库
go build -buildmode=c-archive -o demo6.a
# 导出动态库
go build -buildmode=c-shared -o demo6.so
```
