/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#ifndef __REDIS_RIO_H
#define __REDIS_RIO_H

#include <stdio.h>
#include <stdint.h>
#include "sds.h"

// Redis 内部封装一个IO层，称为RIO

// 系统IO操作的封装
struct _rio {
    /* Backend functions.
     * Since this functions do not tolerate short writes or reads the return
     * value is simplified to: zero on error, non zero on complete success. */
    // 后端方法： 函数返回值为0表示发生错误， 非0 成功

    // 数据流读操作
    size_t (*read)(struct _rio *, void *buf, size_t len);
    // 数据流写操作
    size_t (*write)(struct _rio *, const void *buf, size_t len);
    // 读或写操作的当前偏移量
    off_t (*tell)(struct _rio *);
    // flush操作
    int (*flush)(struct _rio *);
    /* The update_cksum method if not NULL is used to compute the checksum of
     * all the data that was read or written so far. The method should be
     * designed so that can be called with the current checksum, and the buf
     * and len fields pointing to the new block of data to add to the checksum
     * computation. */
    // 更新校验和 可用用CRC64
    void (*update_cksum)(struct _rio *, const void *buf, size_t len);

    /* The current checksum */
    // 当前校验和
    uint64_t cksum;

    /* number of bytes read or written */
    // 已读或已写的字节数
    size_t processed_bytes;

    /* maximum single read or write chunk size */
    // 每次读或写操作的最大字节数
    size_t max_processing_chunk;

    /* Backend-specific vars. */
    // IO 变量 union 定义了 缓冲区IO(buffer I/O)，标准输入输出IO(Stdio file pointer),文件描述符集合(file descriptes set)
    union {
        /* In-memory buffer target. */
        // 内存缓冲区buffer的结构体
        struct {
            // buffer内容，实际就是char数组 sds
            sds ptr;
            // 偏移量
            off_t pos;
        } buffer;

        /* Stdio file pointer target. */
        // 文件机构体
        struct {
            // 打开的文件句柄
            FILE *fp;
            // 最后一个fsync后写入的字节数
            off_t buffered; /* Bytes written since last fsync. */
            // 多少字节进行一次fsync操作
            off_t autosync; /* fsync after 'autosync' bytes written. */
        } file;

        /* Multiple FDs target (used to write to N sockets). */
        // 封装了多个文件描述符结构体(用于写多个socket)
        struct {
            // 文件描述符数组
            int *fds;       /* File descriptors. */
            // 状态位，和fds对应
            int *state;     /* Error state of each fd. 0 (if ok) or errno. */
            // 文件描述符的个数
            int numfds;
            // 偏移量
            off_t pos;
            // 缓冲区
            sds buf;
        } fdset;
    } io;
};

typedef struct _rio rio;

/* The following functions are our interface with the stream. They'll call the
 * actual implementation of read / write / tell, and will update the checksum
 * if needed. */
// 统一的读写操作接口 封装了Redis对buffer读写操作，file读写操作，socket读写操作

// redis对 buffer 写操作
// 将buf数组中的长度为len字符写入rio对象中，写成功返回1，写失败返回0
static inline size_t rioWrite(rio *r, const void *buf, size_t len) {
    while (len) {
        // 判断当前要求写入的字节数是否超过max_processing_chunk规定的最大长度
        size_t bytes_to_write = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        // 写入新的数据时，更新校验和字段
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_write);
        // 调用write方法执行写入操作
        if (r->write(r,buf,bytes_to_write) == 0)
            return 0;
        // 更新buf下次写入的位置    
        buf = (char*)buf + bytes_to_write;
        len -= bytes_to_write;
        // 更新已写入的字节数
        r->processed_bytes += bytes_to_write;
    }
    return 1;
}

// redis对buffer的读操作
// 从rio对象中读出长度len字节的数据，并保存到buf数组中。读取成功返回1，失败返回0
static inline size_t rioRead(rio *r, void *buf, size_t len) {
    while (len) {
        // 判断当前要求写入的字节数是否操作了max_processing_chunk规定的最大长度
        size_t bytes_to_read = (r->max_processing_chunk && r->max_processing_chunk < len) ? r->max_processing_chunk : len;
        // 调用read方法读出数据到buf中
        if (r->read(r,buf,bytes_to_read) == 0)
            return 0;
        // 更新校验和写字段    
        if (r->update_cksum) r->update_cksum(r,buf,bytes_to_read);
        // 更新buf下次写入的位置
        buf = (char*)buf + bytes_to_read;
        len -= bytes_to_read;
        // 更新已读出的字节数
        r->processed_bytes += bytes_to_read;
    }
    return 1;
}

// 返回当前的偏移量
static inline off_t rioTell(rio *r) {
    return r->tell(r);
}

// 执行flush操作
static inline int rioFlush(rio *r) {
    return r->flush(r);
}

// 初始化一个文件rio对象
void rioInitWithFile(rio *r, FILE *fp);
// 初始化一个缓冲区(buf)rio对象
void rioInitWithBuffer(rio *r, sds s);
// 初始化一个文件描述符集合RIO对象
void rioInitWithFdset(rio *r, int *fds, int numfds);

// 释放一个文件描述符集合rio对象
void rioFreeFdset(rio *r);


// 下面是rio封装一些辅助生产AOF协议的函数

// 以"*<count>\r\n"的形式将count以字符串的格式写入rio对象中，返回写入的字节数。
size_t rioWriteBulkCount(rio *r, char prefix, long count);
// 以"$<count>\r\n<payload>\r\n"格式往rio对象中写入二进制安全字符串。
size_t rioWriteBulkString(rio *r, const char *buf, size_t len);
// 以"$<count>\r\n<payload>\r\n"的格式往rio对象中写入long long类型的值。
size_t rioWriteBulkLongLong(rio *r, long long l);
// 以"$<count>\r\n<payload>\r\n"的格式往rio对象中写入double类型的值。
size_t rioWriteBulkDouble(rio *r, double d);

struct redisObject;
// 按AOF协议将redis对象redisObect写入rio中
int rioWriteBulkObject(rio *r, struct redisObject *obj);

// 根据CRC64算法进行校验和
void rioGenericUpdateChecksum(rio *r, const void *buf, size_t len);
// 设置文件类型的rio的的属性autosync(一次fsync操作的字节数)
void rioSetAutoSync(rio *r, off_t bytes);

#endif
