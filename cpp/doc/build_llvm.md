## build llvm

从源码编译llvm

1. clone 代码

```bash
git clone https://github.com/llvm/llvm-project.git
git checkout ${version}
```

2. 编译

```bash
cmake -Sllvm -Bbuild -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/app/llvm \
      -DLLVM_ENABLE_PROJECTS="bolt;clang;clang-tools-extra;lld;lldb" \
      -DLLVM_ENABLE_RUNTIMES="libc;libunwind;libcxxabi;pstl;libcxx;compiler-rt;openmp" \
      -DCLANG_ENABLE_STATIC_ANALYZER=ON \
      -DLLVM_LIBGCC_EXPLICIT_OPT_IN=Yes \
      -GNinja

ninja -Cbuild install -j 10
```
