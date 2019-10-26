/*
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

/**
 *  BIO顾名思义，background IO,是redis中运行的后台IO。 网上千篇一律的说法是redis是单线程单进程。
 *  实际上redis运行过程中并不是严格单进程单线程应用。 
 * 
 *  Redis中的多进程： 
 *  在写入备份（RDB，AOF）的时候，会fork出子进程进行备份文件的写入。
 *  
 *  Redis中的多线程：
 *
 *   1 AOF的备份模式中，如果我们设置的是AOF_FSYNC_EVERYSEC（每秒备份一次，这个设置可理解为弱同步备份），
 *   redis会create一个backgroud线程，在这个线程中执行aof备份文件的写入。
 * 
 *   2 新生成的AOF文件，在覆盖旧AOF文件时。 如果在此之前AOF备份已经开启，在执行该fd的close前，
 *     我们的Redis进程与旧的AOF文件存在引用， 旧的AOF文件不会真正被删除。
 *     所以当我们执行close(oldfd)时，旧AOF文件的被打开该文件的进程数为0，即没有进程打开过这个文件，
 *     这时这个文件在执行close时会被真正删除。 而删除旧AOF文件可能会阻塞服务，所以我们将它放到另一个线程调用。
 * 
 *   3 执行DEL操作，假如碰巧这个key对应有非常多对象，那么这个删除操作会阻塞服务器几秒钟时间， 所以将删除操作放到另一个线程执行。 
 *     具体可看这篇文章： Lazy Redis is better Redis(http://antirez.com/news/93)。
 * 
 * **/


/* Exported API */
// Redis将所有多线程操作封装到BIO中

// 初始化BIO
void bioInit(void);
// 新建一个BIO任务
void bioCreateBackgroundJob(int type, void *arg1, void *arg2, void *arg3);
// 获取当前BIO任务类型，队列中待执行的任务个数
unsigned long long bioPendingJobsOfType(int type);
// 阻塞等待某个类型的BIO任务执行，返回任务个数
unsigned long long bioWaitStepOfType(int type);
time_t bioOlderJobOfType(int type);
// 中断所有BIO进程
void bioKillThreads(void);

/* Background job opcodes */
// BIO操作类型

// 关闭文件
#define BIO_CLOSE_FILE    0 /* Deferred close(2) syscall. */
// AOF写入
#define BIO_AOF_FSYNC     1 /* Deferred AOF fsync. */
// 释放对象
#define BIO_LAZY_FREE     2 /* Deferred objects freeing. */
// BIO数
#define BIO_NUM_OPS       3
