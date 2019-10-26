/* Background I/O service for Redis.
 *
 * 这个文件是redis后台IO服务的实现
 * 
 * This file implements operations that we need to perform in the background.
 * Currently there is only a single operation, that is a background close(2)
 * system call. This is needed as when the process is the last owner of a
 * reference to a file closing it means unlinking it, and the deletion of the
 * file is slow, blocking the server.
 *
 * 这个文件负责我们需要在后台执行的操作，现在redis的版本只有一类操作，后台close系统调用
 * 为了避免一个文件是最后owner在执行close操作带来的unlink使得阻塞server，
 * 将这类操作作用单独的后台线程来执行
 * 
 * In the future we'll either continue implementing new things we need or
 * we'll switch to libeio. However there are probably long term uses for this
 * file as we may want to put here Redis specific background tasks (for instance
 * it is not impossible that we'll need a non blocking FLUSHDB/FLUSHALL
 * implementation).
 *
 * 这个后台服务将来可能会增加更多功能，或者切换到 libeio 上面去。
 * 不过我们可能会长期使用这个文件，以便支持一些 Redis 所特有的后台操作。
 * 比如说，将来我们可能需要一个非阻塞的 FLUSHDB 或者 FLUSHALL 也说不定。
 * DESIGN
 * ------
 *
 * The design is trivial, we have a structure representing a job to perform
 * and a different thread and job queue for every job type.
 * Every thread waits for new jobs in its queue, and process every job
 * sequentially.
 *
 * 设计很简单：
 * 用一个结构表示要执行的工作，而每个类型的工作有一个队列和线程，
 * 每个线程都顺序地执行队列中的工作
 * 
 * Jobs of the same type are guaranteed to be processed from the least
 * recently inserted to the most recently inserted (older jobs processed
 * first).
 * 同一类型的工作按 FIFO 的顺序执行。
 * 
 * Currently there is no way for the creator of the job to be notified about
 * the completion of the operation, this will only be added when/if needed.
 * 
 * 目前还没有办法在任务完成时通知执行者，在有需要的时候，会实现这个功能。
 * ----------------------------------------------------------------------------
 *
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


#include "server.h"
#include "bio.h"
// BIO对象

// BIO线程
static pthread_t bio_threads[BIO_NUM_OPS];
// 使用互斥量+条件变量作为线程的保护条件
// BIO每个线程的mutex锁变量
static pthread_mutex_t bio_mutex[BIO_NUM_OPS];
// BIO线程锁的条件变量，监听这个条件变量唤起当前线程
static pthread_cond_t bio_newjob_cond[BIO_NUM_OPS];
// BIO线程组阻塞锁,bioWaitStepOfType监听这个条件变量被通知该操作执行
static pthread_cond_t bio_step_cond[BIO_NUM_OPS];
// BIO的工作队列
static list *bio_jobs[BIO_NUM_OPS];
/* The following array is used to hold the number of pending jobs for every
 * OP type. This allows us to export the bioPendingJobsOfType() API that is
 * useful when the main thread wants to perform some operation that may involve
 * objects shared with the background thread. The main thread will just wait
 * that there are no longer jobs of this type to be executed before performing
 * the sensible operation. This data is also useful for reporting. */
// BIO未执行的，阻塞队列，记录每种类型job队列里有多少job等待执行
static unsigned long long bio_pending[BIO_NUM_OPS];

/* This structure represents a background Job. It is only used locally to this
 * file as the API does not expose the internals at all. */
// job参数结构，后台任务的数据结构
// 这个结构只有api使用，不会被暴露给外部
struct bio_job {
    // job创建时间
    time_t time; /* Time at which the job was created. */
    /* Job specific arguments pointers. If we need to pass more than three
     * arguments we can just pass a pointer to a structure or alike. */
    // job参数指针。参数多于三个时，可以传递数组或结构
    void *arg1, *arg2, *arg3;
};

void *bioProcessBackgroundJobs(void *arg); // BIO任务执行函数
void lazyfreeFreeObjectFromBioThread(robj *o);
void lazyfreeFreeDatabaseFromBioThread(dict *ht1, dict *ht2);
void lazyfreeFreeSlotsMapFromBioThread(zskiplist *sl);

/* Make sure we have enough stack to perform all the things we do in the
 * main thread. */
// 子线程栈大小
#define REDIS_THREAD_STACK_SIZE (1024*1024*4)

/**
 * 后台任务功能主要两部分：
 * 
 *  1 bioCreateBackgroundJob:创建BIO任务，插入bio_jobs，并调用pthread_cond_signal，通知进程解锁
 *  2 bioProcessBackgroundJobs: 执行BIO任务线程。线程中通过pthread管理进程锁，当bioCreateBackgroundJob
 *    执行pthread_cond_signal通知到该任务对应的线程时，从bio_jobs读出上一任务，并执行
 * 
 * 
 * **/


/* Initialize the background system, spawning(产生) the thread. */
// 初始化后台系统，并初始化线程
void bioInit(void) {
    pthread_attr_t attr;
    pthread_t thread;
    size_t stacksize;
    int j;

    /* Initialization of state vars and objects */
    for (j = 0; j < BIO_NUM_OPS; j++) {
        pthread_mutex_init(&bio_mutex[j],NULL);
        // 初始化阻塞队列的有新任务的信号量
        pthread_cond_init(&bio_newjob_cond[j],NULL);
        // 初始化线程完成任务的信号量
        pthread_cond_init(&bio_step_cond[j],NULL);
        // 初始化阻塞队列
        bio_jobs[j] = listCreate();
        bio_pending[j] = 0;
    }

    /* Set the stack size as by default it may be small in some system */
    pthread_attr_init(&attr);
    // 设置线程栈大小
    pthread_attr_getstacksize(&attr,&stacksize);
    if (!stacksize) stacksize = 1; /* The world is full of Solaris Fixes */
    while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2;
    pthread_attr_setstacksize(&attr, stacksize);

    /* Ready to spawn our threads. We use the single argument the thread
     * function accepts in order to pass the job ID the thread is
     * responsible of. */
    for (j = 0; j < BIO_NUM_OPS; j++) {
        void *arg = (void*)(unsigned long) j;
        // 创建线程，并指定线程的执行函数
        if (pthread_create(&thread,&attr,bioProcessBackgroundJobs,arg) != 0) {
            serverLog(LL_WARNING,"Fatal: Can't initialize Background Jobs.");
            exit(1);
        }
        bio_threads[j] = thread;
    }
}

// 创建BIO任务
void bioCreateBackgroundJob(int type, void *arg1, void *arg2, void *arg3) {
    struct bio_job *job = zmalloc(sizeof(*job));

    job->time = time(NULL);
    job->arg1 = arg1;
    job->arg2 = arg2;
    job->arg3 = arg3;
    // 加锁 保护bio_jobs和bio_pending的一致性
    pthread_mutex_lock(&bio_mutex[type]);
    // 插入到任务队列中
    listAddNodeTail(bio_jobs[type],job);
    bio_pending[type]++;
    // 通过信号量方式 通知process线程，执行任务
    pthread_cond_signal(&bio_newjob_cond[type]);
    // 解锁
    pthread_mutex_unlock(&bio_mutex[type]);
}

// 执行任务 arg任务类型
void *bioProcessBackgroundJobs(void *arg) {
    struct bio_job *job;
    unsigned long type = (unsigned long) arg;
    sigset_t sigset;

    /* Check that the type is within the right interval. */
    if (type >= BIO_NUM_OPS) {
        serverLog(LL_WARNING,
            "Warning: bio thread started with wrong type %lu",type);
        return NULL;
    }

    /***
     * 小知识： pthread线程立即取消的两种方法
     *   相关函数：
     *    int pthread_cancel(pthread_t thread)
     *    发送终止信号给thread线程，如果成功则返回0，否则为非0值，发送成功并不意味thread会终止
     * 
     *    int pthread_setcancelstate(int state,int oldstate);
     *    设置本线程对Cancel信号的反应，state有两种值：
     *      PTHREAD_CANCEL_ENABLE(缺省)表示收到信号后设为CANCELD状态忽略CANCEL信号继续运行
     *      PTHREAD_CANCEL_DISABLE忽略CANCEL信号继续运行
     *      old_state如果不为NULL则存入原来的Cancel状态以便恢复。
     * 
     *    int pthread_setcanceltype(int type, int *oldtype) 
     *    设置本线程取消动作的执行时机 ，type由两种取值：
     *     PTHREAD_CANCEL_DEFFERED和PTHREAD_CANCEL_ASYCHRONOUS，仅当Cancel状态为Enable时有效，
     *    分别表示收到信号后继续运行至下一个取消点再退出和立即执行取消动作（退出）
     *    ；oldtype如果不为NULL则存入运来的取消动作类型值
     * 
     * 
     * **/
    /* Make the thread killable at any time, so that bioKillThreads()
     * can work reliably. */
    // 使线程可以被手动kill
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    // 同步取消，等待下个取点再取消
    pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

    // 加锁 确保不会有两个进程使用pthread_cond_wait监听同一个锁
    pthread_mutex_lock(&bio_mutex[type]);
    /* Block SIGALRM so we are sure that only the main thread will
     * receive the watchdog signal. */
    // 函数初始化信号集合set,将set设置为空
    sigemptyset(&sigset);
    // 将信号signo 加入到信号集合之中
    sigaddset(&sigset, SIGALRM);
    if (pthread_sigmask(SIG_BLOCK, &sigset, NULL))
        serverLog(LL_WARNING,
            "Warning: can't mask SIGALRM in bio.c thread: %s", strerror(errno));

    while(1) {
        listNode *ln;

        /* The loop always starts with the lock hold. */
        // 等待bioCreateBackgroundJob通知解锁 
        if (listLength(bio_jobs[type]) == 0) { 
            // 这个是任务未准备好，等待线程
            pthread_cond_wait(&bio_newjob_cond[type],&bio_mutex[type]);
            continue;
        }
        /* Pop the job from the queue. */
        //取队列中第一个任务
        ln = listFirst(bio_jobs[type]);
        job = ln->value;
        /* It is now possible to unlock the background system as we know have
         * a stand alone job structure to process.*/
        pthread_mutex_unlock(&bio_mutex[type]); //解锁
        /* Process the job accordingly to its type. */
        // 执行具体任务逻辑，业务逻辑
        if (type == BIO_CLOSE_FILE) { 
            // 文件句柄的关闭
            close((long)job->arg1);
        } else if (type == BIO_AOF_FSYNC) {
            // aof文件持久化
            redis_fsync((long)job->arg1);
        } else if (type == BIO_LAZY_FREE) { // 空间懒释放 redis内存释放
            /* What we free changes depending on what arguments are set:
             * arg1 -> free the object at pointer.
             * arg2 & arg3 -> free two dictionaries (a Redis DB).
             * only arg3 -> free the skiplist. */
            // 对象空间的释放
            if (job->arg1)
                lazyfreeFreeObjectFromBioThread(job->arg1);
            else if (job->arg2 && job->arg3)
                // DB空间释放
                lazyfreeFreeDatabaseFromBioThread(job->arg2,job->arg3);
            else if (job->arg3)
                // slots-keys 空间释放 集群槽节点释放
                lazyfreeFreeSlotsMapFromBioThread(job->arg3);
        } else {
            serverPanic("Wrong job type in bioProcessBackgroundJobs().");
        }
        zfree(job);

        /* Lock again before reiterating the loop, if there are no longer
         * jobs to process we'll block again in pthread_cond_wait(). */
        pthread_mutex_lock(&bio_mutex[type]);
        // 从阻塞队列中删除列表
        listDelNode(bio_jobs[type],ln);
        bio_pending[type]--;

        /* Unblock threads blocked on bioWaitStepOfType() if any. */
        //广播解锁，用于解bioWaitStepOfType中的锁， 接收阻塞。
        // 关闭任务完成信号
        pthread_cond_broadcast(&bio_step_cond[type]);
    }
}

/* Return the number of pending jobs of the specified type. */
// 获取队列中等待任务个数
unsigned long long bioPendingJobsOfType(int type) {
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex[type]);
    val = bio_pending[type];
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* If there are pending jobs for the specified type, the function blocks
 * and waits that the next job was processed. Otherwise the function
 * does not block and returns ASAP.
 *
 * The function returns the number of jobs still to process of the
 * requested type.
 *
 * This function is useful when from another thread, we want to wait
 * a bio.c thread to do more work in a blocking way.
 */
unsigned long long bioWaitStepOfType(int type) {
    unsigned long long val;
    pthread_mutex_lock(&bio_mutex[type]);
    val = bio_pending[type];
    if (val != 0) {
        // 监听任务是否执行完成 阻塞点 会收到bioProcessBackgroundJobs#pthread_cond_broadcast信号量
        // 这个是任务已经进行完成等待
        pthread_cond_wait(&bio_step_cond[type],&bio_mutex[type]);
        val = bio_pending[type];
    }
    pthread_mutex_unlock(&bio_mutex[type]);
    return val;
}

/* Kill the running bio threads in an unclean way. This function should be
 * used only when it's critical to stop the threads for some reason.
 * Currently Redis does this only on crash (for instance on SIGSEGV) in order
 * to perform a fast memory check without other threads messing with memory. */
// kill bio线程
void bioKillThreads(void) {
    int err, j;

    for (j = 0; j < BIO_NUM_OPS; j++) {
        if (pthread_cancel(bio_threads[j]) == 0) {
            if ((err = pthread_join(bio_threads[j],NULL)) != 0) {
                serverLog(LL_WARNING,
                    "Bio thread for job type #%d can be joined: %s",
                        j, strerror(err));
            } else {
                serverLog(LL_WARNING,
                    "Bio thread for job type #%d terminated",j);
            }
        }
    }
}
