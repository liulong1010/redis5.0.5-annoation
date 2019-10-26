/* adlist.h - A generic doubly linked list implementation
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

#ifndef __ADLIST_H__
#define __ADLIST_H__

/**
 * Redis链表实现特性：
 * 双端
 * 无环：表头节点prev和表尾节点的next都指向NULL
 * 链表计数器
 * **/

/* Node, List, and Iterator are the only data structures used currently. */

// 链表节点
typedef struct listNode {
    // 前置节点
    struct listNode *prev;
    // 后置节点
    struct listNode *next;
    // 节点的值
    void *value;
} listNode;

// 链表迭代器
typedef struct listIter {
    // 下一个节点
    listNode *next;
    // 迭代方向
    int direction;
} listIter;

// 双端链表
typedef struct list {
    // 表头节点
    listNode *head;
    // 表尾节点
    listNode *tail;
    // 节点值复制函数
    void *(*dup)(void *ptr);
    // 节点值释放函数
    void (*free)(void *ptr);
    // 节点值对比函数
    int (*match)(void *ptr, void *key);
    // 链表所baoh
    unsigned long len;
} list;

/* Functions implemented as macros */
// 获取链表的长度
#define listLength(l) ((l)->len)
// 获取链表的头节点
#define listFirst(l) ((l)->head)
// 获取链表的尾结点
#define listLast(l) ((l)->tail)
// 获取链表节点的前驱节点
#define listPrevNode(n) ((n)->prev)
// 获取链表节点的后继节点
#define listNextNode(n) ((n)->next)
// 获取节点的值
#define listNodeValue(n) ((n)->value)

// 设置链表的节点值复制函数
#define listSetDupMethod(l,m) ((l)->dup = (m))
// 设置链表的节点值释放函数
#define listSetFreeMethod(l,m) ((l)->free = (m))
// 设置链表的节点值的比较函数
#define listSetMatchMethod(l,m) ((l)->match = (m))

// 返回链表的节点值复制函数
#define listGetDupMethod(l) ((l)->dup)
// 返回链表节点值释放函数
#define listGetFree(l) ((l)->free)
// 返回链表节点值比较函数
#define listGetMatchMethod(l) ((l)->match)

/* Prototypes */
// 创建一个不包含任何节点的新链表
list *listCreate(void);
// 释放给定链表，以及链表中的所有节点
void listRelease(list *list);
// 将给定的链表置成空链表，不含节点，释放所有的节点
void listEmpty(list *list);
// 向链表头添加
list *listAddNodeHead(list *list, void *value);
// 向链表尾添加
list *listAddNodeTail(list *list, void *value);
// 向链表插入节点(之前|之后)
list *listInsertNode(list *list, listNode *old_node, void *value, int after);
// 删除链表的给定节点
void listDelNode(list *list, listNode *node);
// 返回指定方向的迭代器 direction: 0 从头开始迭代 1 从尾开始迭代
listIter *listGetIterator(list *list, int direction);
// 返回指向下一个节点的迭代器
listNode *listNext(listIter *iter);
// 释放链表迭代器内存空间
void listReleaseIterator(listIter *iter);
// 复制给定节点的副本
list *listDup(list *orig);
// 查找并返回给定节点值的节点
listNode *listSearchKey(list *list, void *key);
// 返回链表在给定索引上的节点
listNode *listIndex(list *list, long index);
// 将链表迭代器指向链表头
void listRewind(list *list, listIter *li);
// 将链表迭代器指向链表尾
void listRewindTail(list *list, listIter *li);
// 将链表进行导致 尾结点变成头结点
void listRotate(list *list);
// 两个链表join
void listJoin(list *l, list *o);

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __ADLIST_H__ */
