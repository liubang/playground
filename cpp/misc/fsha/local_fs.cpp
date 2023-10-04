//=====================================================================
//
// local_fs.cpp -
//
// Created by liubang on 2023/10/01 22:27
// Last Modified: 2023/10/01 22:27
//
//=====================================================================

#include "cpp/misc/fsha/local_fs.h"

#include <sys/stat.h>
#include <sys/types.h>

namespace pl {

ssize_t LocalFile::size() const { 
    struct stat statbuf; 
}

} // namespace pl
