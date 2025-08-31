## build gcc-15

```bash
set -ex

cd ./gcc-15.2.0/

./configure --prefix=/opt/app/gcc-15 \
            --disable-multilib \
            --enable-languages=c,c++,fortran,lto \
            --with-system-zlib \
            --enable-lto \
            --enable-checking=release \
            --enable-threads=posix \
            --enable-shared \
            --enable-static \
            --enable-__cxa_atexit \
            --target=x86_64-linux-gnu \
            --host=x86_64-linux-gnu \
            --build=x86_64-linux-gnu \
            --with-arch=native \
            --with-tune=native \
            --enable-checking=yes \
            --enable-bootstrap

make -j$(nproc)
sudo make install
```
