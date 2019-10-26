/* Hash Tables Implementation.
 *
 * This file implements in memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "fmacros.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <sys/time.h>

#include "dict.h"
#include "zmalloc.h"
#ifndef DICT_BENCHMARK_MAIN
#include "redisassert.h"
#else
#include <assert.h>
#endif

/* Using dictEnableResize() / dictDisableResize() we make possible to
 * enable/disable resizing of the hash table as needed. This is very important
 * for Redis, as we use copy-on-write and don't want to move too much memory
 * around when there is a child performing saving operations.
 * 
 * 通过 dictEnableResize() 和 dictDisableResize() 两个函数，
 * 程序可以手动地允许或阻止哈希表进行 rehash ，
 * 这在 Redis 使用子进程进行保存操作时，可以有效地利用 copy-on-write 机制。
 * 
 * Note that even when dict_can_resize is set to 0, not all resizes are
 * prevented: a hash table is still allowed to grow if the ratio between
 * the number of elements and the buckets > dict_force_resize_ratio.
 * 
 * 需要注意的是，并非所有 rehash 都会被 dictDisableResize 阻止：
 * 如果已使用节点的数量和字典大小之间的比率，
 * 大于字典强制 rehash 比率 dict_force_resize_ratio ，
 * 那么 rehash 仍然会（强制）进行。
 * 
 *  */

// 指示字典是否启用 rehash 的标识
static int dict_can_resize = 1;
// 强制 rehash 的比率
static unsigned int dict_force_resize_ratio = 5;

/* -------------------------- private prototypes ---------------------------- */

static int _dictExpandIfNeeded(dict *ht);
static unsigned long _dictNextPower(unsigned long size);
static long _dictKeyIndex(dict *ht, const void *key, uint64_t hash, dictEntry **existing);
static int _dictInit(dict *ht, dictType *type, void *privDataPtr);

/* -------------------------- hash functions -------------------------------- */

static uint8_t dict_hash_function_seed[16];

void dictSetHashFunctionSeed(uint8_t *seed) {
    memcpy(dict_hash_function_seed,seed,sizeof(dict_hash_function_seed));
}

uint8_t *dictGetHashFunctionSeed(void) {
    return dict_hash_function_seed;
}

/* The default hashing function uses SipHash implementation
 * in siphash.c. */

uint64_t siphash(const uint8_t *in, const size_t inlen, const uint8_t *k);
uint64_t siphash_nocase(const uint8_t *in, const size_t inlen, const uint8_t *k);

uint64_t dictGenHashFunction(const void *key, int len) {
    return siphash(key,len,dict_hash_function_seed);
}

uint64_t dictGenCaseHashFunction(const unsigned char *buf, int len) {
    return siphash_nocase(buf,len,dict_hash_function_seed);
}

/* ----------------------------- API implementation ------------------------- */

/* Reset a hash table already initialized with ht_init().
 * NOTE: This function should only be called by ht_destroy(). */
// 重置或初始化给定哈希表的各项属性值
static void _dictReset(dictht *ht)
{
    ht->table = NULL;
    ht->size = 0;
    ht->sizemask = 0;
    ht->used = 0;
}

/* Create a new hash table */
/**
 * 创建一个新的字典  O(1) 
 */
dict *dictCreate(dictType *type,
        void *privDataPtr)
{
    dict *d = zmalloc(sizeof(*d));

    _dictInit(d,type,privDataPtr);
    return d;
}

/* Initialize the hash table */
// 初始化哈希表 O(1)
int _dictInit(dict *d, dictType *type,
        void *privDataPtr)
{
    // 初始化连个哈希表各项属性值，但暂时还不分配内存给哈希数组
    _dictReset(&d->ht[0]);
    _dictReset(&d->ht[1]);
    d->type = type;
    // 设置私有数据
    d->privdata = privDataPtr;
    // 设置哈希表 rehash状态
    d->rehashidx = -1;
    // 设置字典的安全迭代器数量
    d->iterators = 0;
    return DICT_OK;
}

/* Resize the table to the minimal size that contains all the elements,
 * but with the invariant of a USED/BUCKETS ratio near to <= 1 */
/**
 * 缩小给定字典
 * 让它的已用节点数和字典大小之间的比率接近 1:1
 * 
 * 返回 DICT_ERR 表示字典已经在rehash，或者 dict_can_resize 为 false
 * 
 * 成功创建体积更小的 ht[1],可开始resize时，返回 DICT_OK
 * O(N)
 * */
int dictResize(dict *d)
{
    int minimal;
    // 不能在关闭rehash或者正在rehash的时候调用
    if (!dict_can_resize || dictIsRehashing(d)) return DICT_ERR;
    // 计算让比率接近1:1 所需要的最小节点数量
    minimal = d->ht[0].used;
    if (minimal < DICT_HT_INITIAL_SIZE)
        minimal = DICT_HT_INITIAL_SIZE;
    // 调整字典的大小 O(n)
    return dictExpand(d, minimal);
}

/* Expand or create the hash table */
/**
 * 创建一个新的哈希表，并根据字典情况，选择以下其中一个动作来进行:
 * 
 * 1) 如果字典的0号哈希表为空，那么将新的哈希表设置为0号哈希表
 * 2) 如果字典的0号哈希表非空，那么将新的哈希表设置为1号哈希表，
 *    并打开字典的rehash标识，使得程序可以开始对字典进行rehash
 * 
 * size 参数不够大，或者rehash已经在进行时，返回DICT_ERRO
 * 
 * 成功创建0号哈希表，或1号哈希表时 返回DICT_OK
 * O(n)
 * 
 * */
int dictExpand(dict *d, unsigned long size)
{
    /* the size is invalid if it is smaller than the number of
     * elements already inside the hash table */
    // 不能在字典正在rehash时进行
    // size的值也不能小于0号哈希表的当前已使用的节点
    if (dictIsRehashing(d) || d->ht[0].used > size)
        return DICT_ERR;

    // 新哈希表
    dictht n; /* the new hash table */
    // 根据size参数，计算哈希表的大小
    unsigned long realsize = _dictNextPower(size);

    /* Rehashing to the same table size is not useful. */
    if (realsize == d->ht[0].size) return DICT_ERR;

    /* Allocate the new hash table and initialize all pointers to NULL */   
    // 为哈希表分配空间
    n.size = realsize;
    n.sizemask = realsize-1;
    n.table = zcalloc(realsize*sizeof(dictEntry*));
    n.used = 0;

    /* Is this the first initialization? If so it's not really a rehashing
     * we just set the first hash table so that it can accept keys. */
    // 如果0号哈希表为空，那么这是一次初始化
    // 程序将新哈希表给0号哈希表的指针，然后字典就可以开始处理键值对
    if (d->ht[0].table == NULL) {
        d->ht[0] = n;
        return DICT_OK;
    }

    /* Prepare a second hash table for incremental rehashing */
    // 如果0号哈希表费控，那么为将要rehash操作分配空间
    // 将新哈希表设置1，打开rehash标识，让字典可以进行rehash
    d->ht[1] = n;
    d->rehashidx = 0;
    return DICT_OK;
}

/* Performs N steps of incremental rehashing. Returns 1 if there are still
 * keys to move from the old to the new hash table, otherwise 0 is returned.
 *
 * Note that a rehashing step consists in moving a bucket (that may have more
 * than one key as we use chaining) from the old to the new hash table, however
 * since part of the hash table may be composed of empty spaces, it is not
 * guaranteed that this function will rehash even a single bucket, since it
 * will visit at max N*10 empty buckets in total, otherwise the amount of
 * work it does would be unbound and the function may block for a long time.
 * 
 * 执行N步渐进式rehash
 * 返回 1 表示然有键需要从0号哈希表移动到1号哈希表
 * 返回 0 则表示所有键都已经迁移完毕
 * 
 * 注意，每步rehash都是以一个哈希表索引(桶)做为单位的
 *  一个桶可能会有多个节点
 *  被rehash的桶的所有节点都会被移动到新的哈希表
 *  O(N)
 *  */
int dictRehash(dict *d, int n) {
    // 一次迁移rehash的最大数量 （从时间考虑)
    int empty_visits = n*10; /* Max number of empty buckets to visit. */
    // rehash已经完成 返回 0 
    if (!dictIsRehashing(d)) return 0;

    // 进行 N 步迁移
    while(n-- && d->ht[0].used != 0) {
        dictEntry *de, *nextde;

        /* Note that rehashidx can't overflow as we are sure there are more
         * elements because ht[0].used != 0 */
        // 校验 确保redhashIndex 没有越界
        assert(d->ht[0].size > (unsigned long)d->rehashidx);
        
        // 略过数组为空的索引，找到下一个非空索引
        while(d->ht[0].table[d->rehashidx] == NULL) {
            d->rehashidx++;
            if (--empty_visits == 0) return 1;
        }
        // 指向该索引的链表表头节点 正要迁移的桶
        de = d->ht[0].table[d->rehashidx];
        /* Move all the keys in this bucket from the old to the new hash HT */
        while(de) {
            uint64_t h;

            // 保存下个节点的指针
            nextde = de->next;
            /* Get the index in the new hash table */
            // 计算新哈希表的哈希值，已经节点插入的索引位置
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;

            // 插入节点到新哈希表 每次将新的节点都插入 新哈希表ht[1] 头节点位置上
            de->next = d->ht[1].table[h];
            d->ht[1].table[h] = de;
            // 更新计算器
            d->ht[0].used--;
            d->ht[1].used++;
            // 继续处理下一个节点
            de = nextde;
        }
        // 将刚迁移完成的哈希表索引设为空 ？这个没有释放内存?
        d->ht[0].table[d->rehashidx] = NULL;
        // 更新rehash索引
        d->rehashidx++;
    }

    /* Check if we already rehashed the whole table... */
    // 如果 0 号哈希表为空，那么表示rehash执行完毕 O(1)
    if (d->ht[0].used == 0) {
        // 释放0号哈希表
        zfree(d->ht[0].table);
        // 将1号哈希表设置为新的0号哈希表
        d->ht[0] = d->ht[1];
        // 重置旧的1号哈希表
        _dictReset(&d->ht[1]);
        // 关闭rehash标识
        d->rehashidx = -1;
        // 返回0，向调用者表示rehash已经完成
        return 0;
    }

    /* More to rehash... */
    return 1;
}

/**
 * 返回以毫秒为单位的unix时间戳
 * O(1) 
 **/
long long timeInMilliseconds(void) {
    struct timeval tv;

    gettimeofday(&tv,NULL);
    return (((long long)tv.tv_sec)*1000)+(tv.tv_usec/1000);
}

/* Rehash for an amount of time between ms milliseconds and ms+1 milliseconds */
/**
 * 在给定毫秒内，以100步为单位，对字典进行rehash
 * **/
int dictRehashMilliseconds(dict *d, int ms) {
    // 记录开始时间
    long long start = timeInMilliseconds();
    int rehashes = 0;

    while(dictRehash(d,100)) {
        rehashes += 100;
        // 如果时间已过，跳出
        if (timeInMilliseconds()-start > ms) break;
    }
    return rehashes;
}

/* This function performs just a step of rehashing, and only if there are
 * no safe iterators bound to our hash table. When we have iterators in the
 * middle of a rehashing we can't mess with the two hash tables otherwise
 * some element can be missed or duplicated.
 *
 * This function is called by common lookup or update operations in the
 * dictionary so that the hash table automatically migrates from H1 to H2
 * while it is actively used.
 * 
 * 在字典不存在安全迭代器的情况下，对字典进行单步rehash
 * 
 * 字典有安全迭代器的情况下不能进行rehash
 * 因为两种不同迭代器和修改操作可能会弄乱字典
 * 
 * 这个函数被多个通用查找、更新操作调用，
 * 它可以让字典被使用的同时进行rehash
 * O(1)
 *  */
static void _dictRehashStep(dict *d) {
    if (d->iterators == 0) dictRehash(d,1);
}

/* Add an element to the target hash table */
/**
 * 尝试将给定键值对添加到字典中
 * 
 * 只有给定键key不存在于字典，添加操作会成功
 * 添加成功返回DICT_OK,失败返回DICT_ERR
 * 最坏O(N) 平摊O(1) 
 * 
 **/
int dictAdd(dict *d, void *key, void *val)
{   // 尝试添加键到字典，并返回包含了这个键的新哈希节点O(N)
    dictEntry *entry = dictAddRaw(d,key,NULL);
    // 键已存在添加失败
    if (!entry) return DICT_ERR;
    // 键不存在，设置节点的值
    dictSetVal(d, entry, val);
    // 添加成功
    return DICT_OK;
}

/* Low level add or find:
 * This function adds the entry but instead of setting a value returns the
 * dictEntry structure to the user, that will make sure to fill the value
 * field as he wishes.
 *
 * This function is also directly exposed to the user API to be called
 * mainly in order to store non-pointers inside the hash value, example:
 *
 * entry = dictAddRaw(dict,mykey,NULL);
 * if (entry != NULL) dictSetSignedIntegerVal(entry,1000);
 *
 * Return values:
 *
 * If key already exists NULL is returned, and "*existing" is populated
 * with the existing entry if existing is not NULL.
 *
 * If key was added, the hash entry is returned to be manipulated by the caller.
 * 尝试将键插入字典中
 * 如果键已经在字典存在，那么返回null
 * 如果不存在，那么程序创建新的哈希节点
 * 将节点和键关联，并插入到字典，然后返回节点本身
 * O(N)
 */
dictEntry *dictAddRaw(dict *d, void *key, dictEntry **existing)
{
    long index;
    dictEntry *entry;
    dictht *ht;
    // 如果条件允许的话，进行单步rehash
    if (dictIsRehashing(d)) _dictRehashStep(d);

    /* Get the index of the new element, or -1 if
     * the element already exists. */
    // 计算键在哈希表中的索引值
    // 如果值为-1，那么表示键存在
    if ((index = _dictKeyIndex(d, key, dictHashKey(d,key), existing)) == -1)
        return NULL;

    /* Allocate the memory and store the new entry.
     * Insert the element in top, with the assumption that in a database
     * system it is more likely that recently added entries are accessed
     * more frequently. */
    // 如果字典正在rehash，那么将新键添加到1号哈希表
    // 否则，将新键添加到0号哈希表
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];
    // 为新节点分配空间
    entry = zmalloc(sizeof(*entry));
    //将新节点插入到链表头
    entry->next = ht->table[index];
    ht->table[index] = entry;
    // 更新哈希表已使用节点梳理
    ht->used++;

    /* Set the hash entry fields. */
    // 设置新节点的键
    dictSetKey(d, entry, key);
    return entry;
}

/* Add or Overwrite:
 * Add an element, discarding the old value if the key already exists.
 * Return 1 if the key was added from scratch, 0 if there was already an
 * element with such key and dictReplace() just performed a value update
 * operation.
 * 
 * 将给定的键值对添加到字典中，如果键已经存在，那么删除旧有的键值对
 * 
 * 如果键值对为全新添加，那么返回-1
 * 如果键值对是通过原有的键值对更新得来的，那么返回0
 *  */
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry, *existing, auxentry;

    /* Try to add the element. If the key
     * does not exists dictAdd will succeed. */
    // 将键添加的到字典
    entry = dictAddRaw(d,key,&existing);
    // key不存在情况
    if (entry) {
        dictSetVal(d, entry, val);
        return 1;
    }

    /* Set the new value and free the old one. Note that it is important
     * to do that in this order, as the value may just be exactly the same
     * as the previous one. In this context, think to reference counting,
     * you want to increment (set), and then decrement (free), and not the
     * reverse. */
    // 这里说明key已经存在，保存原有的值指针
    auxentry = *existing;
    // 设置新的值
    dictSetVal(d, existing, val);
    // 释放旧值
    dictFreeVal(d, &auxentry);
    return 0;
}

/* Add or Find:
 * dictAddOrFind() is simply a version of dictAddRaw() that always
 * returns the hash entry of the specified key, even if the key already
 * exists and can't be added (in that case the entry of the already
 * existing key is returned.)
 *
 * See dictAddRaw() for more information. 
 * 
 * 返回给定的key的字典节点
 * 如果key 存在，返回字典中key的节点
 * 如果key 不存在，将生产key节点添加字典中，并返回新的key节点
 * */
dictEntry *dictAddOrFind(dict *d, void *key) {
    dictEntry *entry, *existing;
    entry = dictAddRaw(d,key,&existing);
    return entry ? entry : existing;
}

/* Search and remove an element. This is an helper function for
 * dictDelete() and dictUnlink(), please check the top comment
 * of those functions.
 * 查找并删除包含key的节点 查到返回节点，没有查到返回null
 * 
 * 参数nofree 决定 是否调用key和value的释放函数
 * 0 调用 1 不调用
 *  */
static dictEntry *dictGenericDelete(dict *d, const void *key, int nofree) {
    uint64_t h, idx;
    dictEntry *he, *prevHe;
    int table;
    // 哈希表为空
    if (d->ht[0].used == 0 && d->ht[1].used == 0) return NULL;
    // 执行单步哈希
    if (dictIsRehashing(d)) _dictRehashStep(d);
    // 计算哈希值
    h = dictHashKey(d, key);
    // 遍历哈希表
    for (table = 0; table <= 1; table++) { 
        // 计算索引值
        idx = h & d->ht[table].sizemask;
        // 指向该索引上的链表
        he = d->ht[table].table[idx];
        prevHe = NULL;
        while(he) {
            // 查找key
            if (key==he->key || dictCompareKeys(d, key, he->key)) {
                /* Unlink the element from the list */
                // 从链表中删除找的key节点
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht[table].table[idx] = he->next;
                // 调用键和值得释放函数
                if (!nofree) {
                    dictFreeKey(d, he);
                    dictFreeVal(d, he);
                    zfree(he);
                }
                // 更新已使用节点数量
                d->ht[table].used--;
                // 返回找的key节点
                return he;
            }
            prevHe = he;
            he = he->next;
        }
        // 执行到这，说明在0号哈希表中找不到给定key
        // 根据字典是否正在rehash，决定是否要查找1号哈希表
        if (!dictIsRehashing(d)) break;
    }
    // 没有找到
    return NULL; /* not found */
}

/* Remove an element, returning DICT_OK on success or DICT_ERR if the
 * element was not found. 
 * 
 * 从字典中删除包含给定key的节点
 * 并调用键值得释放函数来删除键值
 * 
 * 找到并成功删除返回DICT_OK,没有则返回DICT_ERR
 * */
int dictDelete(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,0) ? DICT_OK : DICT_ERR;
}

/* Remove an element from the table, but without actually releasing
 * the key, value and dictionary entry. The dictionary entry is returned
 * if the element was found (and unlinked from the table), and the user
 * should later call `dictFreeUnlinkedEntry()` with it in order to release it.
 * Otherwise if the key is not found, NULL is returned.
 *
 * This function is useful when we want to remove something from the hash
 * table but want to use its value before actually deleting the entry.
 * Without this function the pattern would require two lookups:
 *
 *  entry = dictFind(...);
 *  // Do something with entry
 *  dictDelete(dictionary,entry);
 *
 * Thanks to this function it is possible to avoid this, and use
 * instead:
 *
 * entry = dictUnlink(dictionary,entry);
 * // Do something with entry
 * dictFreeUnlinkedEntry(entry); // <- This does not need to lookup again.
 * 
 * 从字典中查找并删除给定的key节点 但不会调用键值得释放函数
 * 找到key返回节点值,没有返回null
 */
dictEntry *dictUnlink(dict *ht, const void *key) {
    return dictGenericDelete(ht,key,1);
}

/* You need to call this function to really free the entry after a call
 * to dictUnlink(). It's safe to call this function with 'he' = NULL. */
/**
 * 释放给定节点的键值内存
 * **/
void dictFreeUnlinkedEntry(dict *d, dictEntry *he) {
    if (he == NULL) return;
    // 调用节点key的释放函数
    dictFreeKey(d, he);
    // 调用节点value的释放函数
    dictFreeVal(d, he);
    // 释放节点内存空间
    zfree(he);
}

/* Destroy an entire dictionary 

删除哈希表的所有节点，并重置哈希表的各项属性
*/
int _dictClear(dict *d, dictht *ht, void(callback)(void *)) {
    unsigned long i;

    /* Free all the elements */
    for (i = 0; i < ht->size && ht->used > 0; i++) {
        dictEntry *he, *nextHe;

        if (callback && (i & 65535) == 0) callback(d->privdata);
        // 跳过空索引
        if ((he = ht->table[i]) == NULL) continue;
        // 遍历整个链表
        while(he) {
            nextHe = he->next;
            // 删除键
            dictFreeKey(d, he);
            // 删除值
            dictFreeVal(d, he);
            // 释放节点
            zfree(he);
            // 更新已使用节点数
            ht->used--;
            // 处理下一个节点
            he = nextHe;
        }
    }
    /* Free the table and the allocated cache structure */
    // 释放哈希表结构
    zfree(ht->table);
    /* Re-initialize the table */
    // 重置哈希表属性
    _dictReset(ht);
    return DICT_OK; /* never fails */
}

/* Clear & Release the hash table */
/**
 * 删除并释放这个字典
*/
void dictRelease(dict *d)
{   
    // 释放0号哈希表
    _dictClear(d,&d->ht[0],NULL);
    // 释放1号哈希表
    _dictClear(d,&d->ht[1],NULL);
    // 释放字典内存
    zfree(d);
}

/**
 * 查找包含key的节点
 * 找到返回节点，找不到返回null
 * **/
dictEntry *dictFind(dict *d, const void *key)
{
    dictEntry *he;
    uint64_t h, idx, table;
    // 字典为空
    if (d->ht[0].used + d->ht[1].used == 0) return NULL; /* dict is empty */
    // 条件允许执行单步哈希
    if (dictIsRehashing(d)) _dictRehashStep(d);
    // 计算哈希值
    h = dictHashKey(d, key);
    // 在字典中查找key
    for (table = 0; table <= 1; table++) { 
        // 计算索引值
        idx = h & d->ht[table].sizemask;
        // 遍历给定索引上链表所有的节点，查找key
        he = d->ht[table].table[idx];
        while(he) { 
            // 找key，返回节点
            if (key==he->key || dictCompareKeys(d, key, he->key))
                return he;
            he = he->next;
        }
        // 0号哈希表上没有找到，判断是否要在1号哈希表上查找key
        if (!dictIsRehashing(d)) return NULL;
    }
    // 没找到
    return NULL;
}

/**
 * 获取包含key节点的值
 * 在字典key对应的value
 * **/
void *dictFetchValue(dict *d, const void *key) {
    dictEntry *he;
    // 查找字典对应的key的节点
    he = dictFind(d,key);
    // 返回节点的value
    return he ? dictGetVal(he) : NULL;
}

/* A fingerprint is a 64 bit number that represents the state of the dictionary
 * at a given time, it's just a few dict properties xored together.
 * When an unsafe iterator is initialized, we get the dict fingerprint, and check
 * the fingerprint again when the iterator is released.
 * If the two fingerprints are different it means that the user of the iterator
 * performed forbidden operations against the dictionary while iterating. */
long long dictFingerprint(dict *d) {
    long long integers[6], hash = 0;
    int j;

    integers[0] = (long) d->ht[0].table;
    integers[1] = d->ht[0].size;
    integers[2] = d->ht[0].used;
    integers[3] = (long) d->ht[1].table;
    integers[4] = d->ht[1].size;
    integers[5] = d->ht[1].used;

    /* We hash N integers by summing every successive integer with the integer
     * hashing of the previous sum. Basically:
     *
     * Result = hash(hash(hash(int1)+int2)+int3) ...
     *
     * This way the same set of integers in a different order will (likely) hash
     * to a different number. */
    for (j = 0; j < 6; j++) {
        hash += integers[j];
        /* For the hashing step we use Tomas Wang's 64 bit integer hash. */
        hash = (~hash) + (hash << 21); // hash = (hash << 21) - hash - 1;
        hash = hash ^ (hash >> 24);
        hash = (hash + (hash << 3)) + (hash << 8); // hash * 265
        hash = hash ^ (hash >> 14);
        hash = (hash + (hash << 2)) + (hash << 4); // hash * 21
        hash = hash ^ (hash >> 28);
        hash = hash + (hash << 31);
    }
    return hash;
}

// 创建并返回给定字典的不安全迭代器
dictIterator *dictGetIterator(dict *d)
{
    dictIterator *iter = zmalloc(sizeof(*iter));

    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->safe = 0;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

// 创建并返回给定字典的安全迭代器
dictIterator *dictGetSafeIterator(dict *d) {
    dictIterator *i = dictGetIterator(d);

    i->safe = 1;
    return i;
}

/**
 * 返回迭代器指向的下个节点
 * 字典迭代完毕时，返回null
 **/
dictEntry *dictNext(dictIterator *iter)
{
    while (1) {
        // 进入这个循环有两种可能
        // 1) 这个迭代器第一次运行
        // 2) 当前索引列表的节点已经迭代完(null为链表的表尾)
        if (iter->entry == NULL) {
            // 指向被迭代的哈希表
            dictht *ht = &iter->d->ht[iter->table];
            // 初次迭代时运行
            if (iter->index == -1 && iter->table == 0) { 
                // 如果安全迭代器，那么更新安全迭代器计数器
                if (iter->safe)
                    iter->d->iterators++;
                // 如果不安全迭代器，那么计算指纹    
                else
                    iter->fingerprint = dictFingerprint(iter->d);
            }
            // 更新索引
            iter->index++;
            
            // 如果迭代器的当前索引值大于当前被迭代的哈希表的大小
            // 那么说明这个哈希表已经迭代完成
            if (iter->index >= (long) ht->size) { 
                //如果正在rehash的话，那么说明1号哈希表也在使用中
                //那么继续对1号表进行迭代
                if (dictIsRehashing(iter->d) && iter->table == 0) {
                    iter->table++;
                    iter->index = 0;
                    ht = &iter->d->ht[1];
                } else { // 如果没有rehash 说明已经迭代完成
                    break;
                }
            }
            // 如果进行到这里，说明这个哈希表未迭代完成
            // 更新几点指针，指向下个索引的表头节点
            iter->entry = ht->table[iter->index];
        } else {
            // 执行到这里，说明程序正在迭代某个链表
            // 将节点指针指向链表的下一个节点
            iter->entry = iter->nextEntry;
        }
        // 如果当前节点不为空，那么记录下该节点的下个节点
        // 因为安全迭代器有可能会将迭代器返回当前节点删除
        if (iter->entry) {
            /* We need to save the 'next' here, the iterator user
             * may delete the entry we are returning. */
            iter->nextEntry = iter->entry->next;
            // 迭代完毕 找到下个节点
            return iter->entry;
        }
    }
    return NULL;
}

/**
 * 释放给定字典的迭代器
 * **/
void dictReleaseIterator(dictIterator *iter)
{
    if (!(iter->index == -1 && iter->table == 0)) {
        // 释放安全迭代器时，安全迭代其计算器-1
        if (iter->safe)
            iter->d->iterators--;
        else // 释放不安全迭代器，验证指纹是否有变化
            assert(iter->fingerprint == dictFingerprint(iter->d));
    }
    zfree(iter);
}

/* Return a random entry from the hash table. Useful to
 * implement randomized algorithms
 * 
 * 随机返回字典中任意一个节点
 * 可用于实现随机算法
 * 如果字典为空 返回null
 * 
 * 两次随机 先随机找到哈希表的一个桶，
 * 在随机找到桶上的一个节点
 * 
 *  */
dictEntry *dictGetRandomKey(dict *d)
{
    dictEntry *he, *orighe;
    unsigned long h;
    int listlen, listele;
    // 字典为空
    if (dictSize(d) == 0) return NULL;
    // 进行单步rehash
    if (dictIsRehashing(d)) _dictRehashStep(d);

    // 如果正在rehash，那么将1号哈希表也作为随机查找的目标
    if (dictIsRehashing(d)) {
        do {
            /* We are sure there are no elements in indexes from 0
             * to rehashidx-1 */
            h = d->rehashidx + (random() % (d->ht[0].size +
                                            d->ht[1].size -
                                            d->rehashidx));
            he = (h >= d->ht[0].size) ? d->ht[1].table[h - d->ht[0].size] :
                                      d->ht[0].table[h];
        } while(he == NULL);
    // 否则，只从0号哈希表中查找节点    
    } else {
        do {
            h = random() & d->ht[0].sizemask;
            he = d->ht[0].table[h];
        } while(he == NULL);
    }

    /* Now we found a non empty bucket, but it is a linked
     * list and we need to get a random element from the list.
     * The only sane way to do so is counting the elements and
     * select a random index. */
    // 目前he已经指向一个非空的节点链表
    // 程序将从这个链表随机返回一个节点
    listlen = 0;
    orighe = he;
    // 计算链表上就节点数量
    while(he) {
        he = he->next;
        listlen++;
    }
    // 取模，得出随机节点的索引
    listele = random() % listlen;
    he = orighe;
    // 按索引查找节点
    while(listele--) he = he->next;
    // 返回随机节点
    return he;
}

/* This function samples the dictionary to return a few keys from random
 * locations.
 * 这个函数对字典进行采样，从random中返回几个键的位置。
 * 
 * It does not guarantee to return all the keys specified in 'count', nor
 * it does guarantee to return non-duplicated elements, however it will make
 * some effort to do both things.
 * 它不保证返回'count'中指定的所有键，也不它保证返回非重复的元素，
 * 但是它会这样做两件事都要努力去做。
 * 
 * Returned pointers to hash table entries are stored into 'des' that
 * points to an array of dictEntry pointers. The array must have room for
 * at least 'count' elements, that is the argument we pass to the function
 * to tell how many random elements we need.
 * 返回到哈希表项的指针存储在'des'中指向一个dictEntry指针数组。
 * 数组必须有足够的空间至少“count”元素是我们传递给函数的参数告诉我们需要多少随机元素。
 * 
 * The function returns the number of items stored into 'des', that may
 * be less than 'count' if the hash table has less than 'count' elements
 * inside, or if not enough elements were found in a reasonable amount of
 * steps.
 * 该函数返回存储到'des'中的项的数量，即可能如果哈希表的元素数小于count，
 * 则小于count内部，或如果没有足够的元素被发现在一个合理的数量步骤。
 * 
 * Note that this function is not suitable when you need a good distribution
 * of the returned items, but only when you need to "sample" a given number
 * of continuous elements to run some kind of algorithm or to produce
 * statistics. However the function is much faster than dictGetRandomKey()
 * at producing N elements.
 * 请注意，当您需要良好的分布时，此函数并不适用返回的项目，
 * 但只有当您需要“采样”一个给定的数字连续的元素来运行某种算法或生成统计数据。
 * 但是这个函数比dictGetRandomKey()要快得多生成N个元素。
 * 
 * 
 * 抽样函数 在淘汰策略中的在LRU算法中使用
 * 
 * param 
 *  d 字典 
 *  des 存储从字典中采样出来的节点数组
 *  count 单次采样频率
 * return 采样的节点数
 *  */
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count) {
    unsigned long j; /* internal hash table id, 0 or 1. */
    unsigned long tables; /* 1 or 2 tables? */
    unsigned long stored = 0, maxsizemask;
    unsigned long maxsteps;

    // 如果字典大小于采样数，将采样节点设置为字典的大小
    if (dictSize(d) < count) count = dictSize(d);

    // 设置最大采样步数
    maxsteps = count*10;

    /* Try to do a rehashing work proportional to 'count'. */
    // 进行count次的单步rehash操作
    for (j = 0; j < count; j++) {
        if (dictIsRehashing(d))
            _dictRehashStep(d);
        else
            break;
    }

    // 有效哈希表数量
    tables = dictIsRehashing(d) ? 2 : 1;

    // 设置最大掩码
    maxsizemask = d->ht[0].sizemask;
    if (tables > 1 && maxsizemask < d->ht[1].sizemask)
        maxsizemask = d->ht[1].sizemask;

    /* Pick a random point inside the larger table. */
    // 在较大的表中随机选择一个采样点
    unsigned long i = random() & maxsizemask;
    unsigned long emptylen = 0; /* Continuous empty entries so far. */
    while(stored < count && maxsteps--) {
        for (j = 0; j < tables; j++) {
            /* Invariant(不变) of the dict.c rehashing: up to the indexes already
             * visited in ht[0] during the rehashing, there are no populated(填充)
             * buckets, so we can skip ht[0] for indexes between 0 and idx-1. */
            // 正在rehash 且采样点<rehashidx 在1号表中进行采样
            if (tables == 2 && j == 0 && i < (unsigned long) d->rehashidx) {
                /* Moreover, if we are currently out of range in the second
                 * table, there will be no elements in both tables up to
                 * the current rehashing index, so we jump if possible.
                 * (this happens when going from big to small table). */
                if (i >= d->ht[1].size)
                    i = d->rehashidx;
                else
                    continue;
            }
            // 越界判断
            if (i >= d->ht[j].size) continue; /* Out of range for this table. */
            // 在j号哈希表中采样i桶节点数据
            dictEntry *he = d->ht[j].table[i];

            /* Count contiguous(连续的) empty buckets, and jump to other
             * locations if they reach 'count' (with a minimum of 5). */
            if (he == NULL) {
                emptylen++;
                if (emptylen >= 5 && emptylen > count) {
                    i = random() & maxsizemask;
                    emptylen = 0;
                }
            } else {
                emptylen = 0;
                while (he) {
                    /* Collect all the elements of the buckets found non
                     * empty while iterating. */
                    // 采样的数据
                    *des = he;
                    des++;
                    he = he->next;
                    stored++;
                    if (stored == count) return stored;
                }
            }
        }
        i = (i+1) & maxsizemask;
    }
    return stored;
}

/* Function to reverse bits. Algorithm from:
 * http://graphics.stanford.edu/~seander/bithacks.html#ReverseParallel 
 * 位反转函数 
 * 
 * */
static unsigned long rev(unsigned long v) {
    unsigned long s = 8 * sizeof(v); // bit size; must be power of 2
    unsigned long mask = ~0;
    while ((s >>= 1) > 0) {
        mask ^= (mask << s);
        v = ((v >> s) & mask) | ((v << s) & ~mask);
    }
    return v;
}

/* dictScan() is used to iterate over the elements of a dictionary.
 * dictScan() 用于迭代给定字典中的元素 
 * 
 * Iterating works the following way:
 *
 * 1) Initially you call the function using a cursor (v) value of 0.
 * 2) The function performs one step of the iteration, and returns the
 *    new cursor value you must use in the next call.
 * 3) When the returned cursor is 0, the iteration is complete.
 * 迭代按照以下方式
 *  1) 一开始，你使用0作为游标来调用函数
 *  2) 函数执行一步迭代操作，并返回一个下次迭代时使用的新游标
 *  3) 当函数返回的游标为0时，迭代完成
 * 
 * The function guarantees all elements present in the
 * dictionary get returned between the start and end of the iteration.
 * However it is possible some elements get returned multiple times.
 * 函数保证，在迭代从开始到结束期间，一直存在于字典的元素肯定会被迭代到
 * 但一个元素可能会被返回多次
 * 
 * For every element returned, the callback argument 'fn' is
 * called with 'privdata' as first argument and the dictionary entry
 * 'de' as second argument.
 * 每当一个元素返回时，回调函数fn就会执行
 * fn 函数第一个参数是privdata,而第二个参数则是字典节点de
 * 
 * HOW IT WORKS.
 *
 * The iteration algorithm was designed by Pieter Noordhuis.
 * The main idea is to increment a cursor starting from the higher order
 * bits. That is, instead of incrementing the cursor normally, the bits
 * of the cursor are reversed, then the cursor is incremented, and finally
 * the bits are reversed again.
 * 工作原理
 *   迭代所使用的算法是由Pieter Noordhuis 设计的
 *   算法的主要思路是在二进制高位上对游标进行算法计算
 *   也就是说，不是按正常的办法来对游标进行加法算法，
 *   而是首先将游标的二进制位翻转(reverse)过来
 *   然后对翻转后的值进行加法算法，
 *   最后再次对加法算法之后的结果进行翻转
 * 
 * This strategy is needed because the hash table may be resized between
 * iteration calls.
 * 这一策略是必要的，因为在一次完整的迭代过程中，哈希表的大小由可能在两次迭代之间发生改变
 * 
 * dict.c hash tables are always power of two in size, and they
 * use chaining, so the position of an element in a given table is given
 * by computing the bitwise AND between Hash(key) and SIZE-1
 * (where SIZE-1 is always the mask that is equivalent to taking the rest
 *  of the division between the Hash of the key and SIZE).
 * 哈希表的大小总是2的某个次方，并且哈希表使用链表来解决冲突，
 * 因此一个指定元素在一个给定表的位置总可以通过Hash(key)&SIZE-1公式来计算出，
 * 其中SIZE-1 是哈希表的最大索引值，这个最大索引值就是哈希表的mast(掩码)。
 * 
 * For example if the current hash table size is 16, the mask is
 * (in binary) 1111. The position of a key in the hash table will always be
 * the last four bits of the hash output, and so forth.
 * 举个例子，如果当前哈希表的大小为16，
 * 那么他的掩码就是二机制 1111
 * 这个哈希表的所有位置都可以用哈希值的最后四个二进制来记录
 * 
 * WHAT HAPPENS IF THE TABLE CHANGES IN SIZE?
 * 如果哈希表的大小改变了怎么办？

 * If the hash table grows, elements can go anywhere in one multiple of
 * the old bucket: for example let's say we already iterated with
 * a 4 bit cursor 1100 (the mask is 1111 because hash table size = 16).
 * 当对哈希表进行扩展时，元素可能从一个槽位移动到另一个槽，
 * 举个例子，假设我刚好迭代值4位游标1100，
 * 而哈希表的mask为1111(哈希表的大小为16)
 * 
 * If the hash table will be resized to 64 elements, then the new mask will
 * be 111111. The new buckets you obtain by substituting in ??1100
 * with either 0 or 1 can be targeted only by keys we already visited
 * when scanning the bucket 1100 in the smaller hash table.
 * 如果这时哈希表将大小改为64，那么哈希表的mask将变成111111
 * 
 * By iterating the higher bits first, because of the inverted counter, the
 * cursor does not need to restart if the table size gets bigger. It will
 * continue iterating using cursors without '1100' at the end, and also
 * without any other combination of the final 4 bits already explored.
 *
 * Similarly when the table size shrinks over time, for example going from
 * 16 to 8, if a combination of the lower three bits (the mask for size 8
 * is 111) were already completely explored, it would not be visited again
 * because we are sure we tried, for example, both 0111 and 1111 (all the
 * variations of the higher bit) so we don't need to test it again.
 *
 * WAIT... YOU HAVE *TWO* TABLES DURING REHASHING!
 * 等待。。。 在rehash的时候可能会出现两个哈希表的啊！
 * 
 * Yes, this is true, but we always iterate the smaller table first, then
 * we test all the expansions of the current cursor into the larger
 * table. For example if the current cursor is 101 and we also have a
 * larger table of size 16, we also test (0)101 and (1)101 inside the larger
 * table. This reduces the problem back to having only one table, where
 * the larger one, if it exists, is just an expansion of the smaller one.
 *
 * LIMITATIONS
 * 限制
 * 
 * This iterator is completely stateless, and this is a huge advantage,
 * including no additional memory used.
 * 这个迭代器是完全无状态的，这时一个巨大的优势
 * 因为迭代可以在不适用任何额外的内存的情况下进行
 * 
 * The disadvantages resulting from this design are:
 * 这个设计缺陷在于：
 * 
 * 1) It is possible we return elements more than once. However this is usually
 *    easy to deal with in the application level.
 *  函数可能会返回重复的元素，不过这个问题可以很容易在应用层解决。
 * 2) The iterator must return multiple elements per call, as it needs to always
 *    return all the keys chained in a given bucket, and all the expansions, so
 *    we are sure we don't miss keys moving during rehashing.
 *    为了不错过任何元素，
 *    迭代器需要返回给定桶上的所有键，
 *    以及因为扩展哈希表而产生出来的新表，
 *    所以迭代器必须在一次迭代中返回多个元素。
 * 3) The reverse cursor is somewhat hard to understand at first, but this
 *    comment is supposed to help.
 *     对游标进行翻转（reverse）的原因初看上去比较难以理解，
 *    不过阅读这份注释应该会有所帮助。
 * 
 * 
 * d 字典
 * v 哈希桶的位置 可以从1-size进行迭代
 *    用于计算哈希桶的索引 idx = dictHashKey(d, key) & d->ht[table].sizemask
 *   size = 16 ,sizemask = 15 (1111) 
 *   v 为 5 时 5&15 -> 5 桶位置 
 *  
 */
unsigned long dictScan(dict *d,
                       unsigned long v,
                       dictScanFunction *fn,
                       dictScanBucketFunction* bucketfn,
                       void *privdata)
{
    dictht *t0, *t1;
    const dictEntry *de, *next;
    unsigned long m0, m1;

    // 字典为空，跳过
    if (dictSize(d) == 0) return 0;

    // 字典没有rehash 迭代0号哈希表
    if (!dictIsRehashing(d)) { 
        // 指向0号哈希表
        t0 = &(d->ht[0]);
        // 记录0号哈希表mask
        m0 = t0->sizemask;

        /* Emit entries at cursor */
        // 哈希桶的回调函数  idx = dictHashKey(d, key) & d->ht[table].sizemask;
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]);
        // 指向哈希桶
        de = t0->table[v & m0];
        // 遍历哈希桶
        while (de) {
            next = de->next;
            // 遍历到桶上的节点的回调好函数
            fn(privdata, de);
            de = next;
        }

        /* Set unmasked bits so incrementing the reversed cursor
         * operates on the masked bits */
        v |= ~m0;

        /* Increment the reverse cursor */
        v = rev(v);
        v++;
        v = rev(v);

    } else {
        // 指向连个哈希表
        t0 = &d->ht[0];
        t1 = &d->ht[1];

        /* Make sure t0 is the smaller and t1 is the bigger table */
        if (t0->size > t1->size) {
            t0 = &d->ht[1];
            t1 = &d->ht[0];
        }

        m0 = t0->sizemask;
        m1 = t1->sizemask;

        /* Emit entries at cursor */
        // 遍历0号哈希表
        if (bucketfn) bucketfn(privdata, &t0->table[v & m0]);
        de = t0->table[v & m0];
        while (de) {
            next = de->next;
            fn(privdata, de);
            de = next;
        }

        /* Iterate over indices in larger table that are the expansion
         * of the index pointed to by the cursor in the smaller table */
        do {
            /* Emit entries at cursor */
            if (bucketfn) bucketfn(privdata, &t1->table[v & m1]);
            de = t1->table[v & m1];
            while (de) {
                next = de->next;
                fn(privdata, de);
                de = next;
            }

            /* Increment the reverse cursor not covered by the smaller mask.*/
            v |= ~m1;
            v = rev(v);
            v++;
            v = rev(v);

            /* Continue while bits covered by mask difference is non-zero */
        } while (v & (m0 ^ m1));
    }

    return v;
}

/* ------------------------- private functions ------------------------------ */

/* Expand the hash table if needed
  根据需要，初始化字典(的哈希表),或者对字典(的现有哈希表)进行扩展

 */
static int _dictExpandIfNeeded(dict *d)
{
    /* Incremental rehashing already in progress. Return. */
    // 渐进式rehash已经在进行，直接返回
    if (dictIsRehashing(d)) return DICT_OK;

    /* If the hash table is empty expand it to the initial size. */
    // 如果字典的0号哈希表为空，那么创建并返回初始化0号哈希表
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    /* If we reached the 1:1 ratio, and we are allowed to resize the hash
     * table (global setting) or we should avoid it but the ratio between
     * elements/buckets is over the "safe" threshold, we resize doubling
     * the number of buckets. */
    // 以下两个条件之一为真，对字典进行扩展
    // 1) 字典已经使用节点和字典大小之间比率近1：1,并且dict_can_resize为真
    // 2) 已使用节点数和字典大小间的比率超过dict_force_resize_ratio
    if (d->ht[0].used >= d->ht[0].size &&
        (dict_can_resize ||
         d->ht[0].used/d->ht[0].size > dict_force_resize_ratio))
    {   // 新的哈希表的大小至少是目前已使用节点的两倍
        return dictExpand(d, d->ht[0].used*2);
    }
    return DICT_OK;
}

/* Our hash table capability is a power of two 
 计算第一个大于等于size的2的N次方，用作哈希表的值
*/
static unsigned long _dictNextPower(unsigned long size)
{
    unsigned long i = DICT_HT_INITIAL_SIZE;

    if (size >= LONG_MAX) return LONG_MAX + 1LU;
    while(1) {
        if (i >= size)
            return i;
        i *= 2;
    }
}

/* Returns the index of a free slot that can be populated with
 * a hash entry for the given 'key'.
 * If the key already exists, -1 is returned
 * and the optional output parameter may be filled.
 *
 * Note that if we are in the process of rehashing the hash table, the
 * index is always returned in the context of the second (new) hash table.
 * 
 * 返回可以将 key(给了hash)插入哈希表索引的位置
 *  
 * hash  正常使用 h = dictHashKey(d, key) 计算
 * 如果key已经存在，返回-1，并将key的节点 放在 existing中
 * 
 *  */
static long _dictKeyIndex(dict *d, const void *key, uint64_t hash, dictEntry **existing)
{
    unsigned long idx, table;
    dictEntry *he;
    if (existing) *existing = NULL;

    /* Expand the hash table if needed */
    if (_dictExpandIfNeeded(d) == DICT_ERR)
        return -1;
    for (table = 0; table <= 1; table++) { 
        // 根据给定的hash 计算 索引值 
        // hash正常情况下是调用 dictHashKey(d, key) 这里通过外部参数传过来
        idx = hash & d->ht[table].sizemask;
        /* Search if this slot does not already contain the given key */
        he = d->ht[table].table[idx];
        while(he) {
            if (key==he->key || dictCompareKeys(d, key, he->key)) { 
                // 查到了key 
                if (existing) *existing = he;
                return -1;
            }
            he = he->next;
        }
        if (!dictIsRehashing(d)) break;
    }
    return idx;
}

/**
 * 清空字典的所有哈希表节点，并重置字典属性
 * **/
void dictEmpty(dict *d, void(callback)(void*)) {
    _dictClear(d,&d->ht[0],callback);
    _dictClear(d,&d->ht[1],callback);
    d->rehashidx = -1;
    d->iterators = 0;
}
/**
 * 开启自动 rehash 
 **/
void dictEnableResize(void) {
    dict_can_resize = 1;
}
/**
 * 关闭自动 rehash 
 **/
void dictDisableResize(void) {
    dict_can_resize = 0;
}
/***
 * 计算key的哈希值
 */
uint64_t dictGetHash(dict *d, const void *key) {
    return dictHashKey(d, key);
}

/* Finds the dictEntry reference by using pointer and pre-calculated hash.
 * oldkey is a dead pointer and should not be accessed.
 * the hash value should be provided using dictGetHash.
 * no string / key comparison is performed.
 * return value is the reference to the dictEntry if found, or NULL if not found. */
dictEntry **dictFindEntryRefByPtrAndHash(dict *d, const void *oldptr, uint64_t hash) {
    dictEntry *he, **heref;
    unsigned long idx, table;

    if (d->ht[0].used + d->ht[1].used == 0) return NULL; /* dict is empty */
    for (table = 0; table <= 1; table++) {
        idx = hash & d->ht[table].sizemask;
        heref = &d->ht[table].table[idx];
        he = *heref;
        while(he) {
            if (oldptr==he->key)
                return heref;
            heref = &he->next;
            he = *heref;
        }
        if (!dictIsRehashing(d)) return NULL;
    }
    return NULL;
}

/* ------------------------------- Debugging ---------------------------------*/

#define DICT_STATS_VECTLEN 50
size_t _dictGetStatsHt(char *buf, size_t bufsize, dictht *ht, int tableid) {
    unsigned long i, slots = 0, chainlen, maxchainlen = 0;
    unsigned long totchainlen = 0;
    unsigned long clvector[DICT_STATS_VECTLEN];
    size_t l = 0;

    if (ht->used == 0) {
        return snprintf(buf,bufsize,
            "No stats available for empty dictionaries\n");
    }

    /* Compute stats. */
    for (i = 0; i < DICT_STATS_VECTLEN; i++) clvector[i] = 0;
    for (i = 0; i < ht->size; i++) {
        dictEntry *he;

        if (ht->table[i] == NULL) {
            clvector[0]++;
            continue;
        }
        slots++;
        /* For each hash entry on this slot... */
        chainlen = 0;
        he = ht->table[i];
        while(he) {
            chainlen++;
            he = he->next;
        }
        clvector[(chainlen < DICT_STATS_VECTLEN) ? chainlen : (DICT_STATS_VECTLEN-1)]++;
        if (chainlen > maxchainlen) maxchainlen = chainlen;
        totchainlen += chainlen;
    }

    /* Generate human readable stats. */
    l += snprintf(buf+l,bufsize-l,
        "Hash table %d stats (%s):\n"
        " table size: %ld\n"
        " number of elements: %ld\n"
        " different slots: %ld\n"
        " max chain length: %ld\n"
        " avg chain length (counted): %.02f\n"
        " avg chain length (computed): %.02f\n"
        " Chain length distribution:\n",
        tableid, (tableid == 0) ? "main hash table" : "rehashing target",
        ht->size, ht->used, slots, maxchainlen,
        (float)totchainlen/slots, (float)ht->used/slots);

    for (i = 0; i < DICT_STATS_VECTLEN-1; i++) {
        if (clvector[i] == 0) continue;
        if (l >= bufsize) break;
        l += snprintf(buf+l,bufsize-l,
            "   %s%ld: %ld (%.02f%%)\n",
            (i == DICT_STATS_VECTLEN-1)?">= ":"",
            i, clvector[i], ((float)clvector[i]/ht->size)*100);
    }

    /* Unlike snprintf(), teturn the number of characters actually written. */
    if (bufsize) buf[bufsize-1] = '\0';
    return strlen(buf);
}

void dictGetStats(char *buf, size_t bufsize, dict *d) {
    size_t l;
    char *orig_buf = buf;
    size_t orig_bufsize = bufsize;

    l = _dictGetStatsHt(buf,bufsize,&d->ht[0],0);
    buf += l;
    bufsize -= l;
    if (dictIsRehashing(d) && bufsize > 0) {
        _dictGetStatsHt(buf,bufsize,&d->ht[1],1);
    }
    /* Make sure there is a NULL term at the end. */
    if (orig_bufsize) orig_buf[orig_bufsize-1] = '\0';
}

/* ------------------------------- Benchmark ---------------------------------*/

#ifdef DICT_BENCHMARK_MAIN 

#include "sds.h"

uint64_t hashCallback(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

int compareCallback(void *privdata, const void *key1, const void *key2) {
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

void freeCallback(void *privdata, void *val) {
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

dictType BenchmarkDictType = {
    hashCallback,
    NULL,
    NULL,
    compareCallback,
    freeCallback,
    NULL
};

#define start_benchmark() start = timeInMilliseconds()
#define end_benchmark(msg) do { \
    elapsed = timeInMilliseconds()-start; \
    printf(msg ": %ld items in %lld ms\n", count, elapsed); \
} while(0);

/* dict-benchmark [count] */
int main(int argc, char **argv) {
    long j;
    long long start, elapsed;
    dict *dict = dictCreate(&BenchmarkDictType,NULL);
    long count = 0;

    if (argc == 2) {
        count = strtol(argv[1],NULL,10);
    } else {
        count = 5000000;
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        int retval = dictAdd(dict,sdsfromlonglong(j),(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Inserting");
    assert((long)dictSize(dict) == count);

    /* Wait for rehashing. */
    while (dictIsRehashing(dict)) {
        dictRehashMilliseconds(dict,100);
    }

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Linear access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Linear access of existing elements (2nd round)");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(rand() % count);
        dictEntry *de = dictFind(dict,key);
        assert(de != NULL);
        sdsfree(key);
    }
    end_benchmark("Random access of existing elements");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(rand() % count);
        key[0] = 'X';
        dictEntry *de = dictFind(dict,key);
        assert(de == NULL);
        sdsfree(key);
    }
    end_benchmark("Accessing missing");

    start_benchmark();
    for (j = 0; j < count; j++) {
        sds key = sdsfromlonglong(j);
        int retval = dictDelete(dict,key);
        assert(retval == DICT_OK);
        key[0] += 17; /* Change first number to letter. */
        retval = dictAdd(dict,key,(void*)j);
        assert(retval == DICT_OK);
    }
    end_benchmark("Removing and adding");
}
#endif
