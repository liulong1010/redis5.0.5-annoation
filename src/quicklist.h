/* quicklist.h - A generic doubly linked quicklist implementation
 *
 * Copyright (c) 2014, Matt Stancliff <matt@genges.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this quicklist of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this quicklist of conditions and the following disclaimer in the
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

#ifndef __QUICKLIST_H__
#define __QUICKLIST_H__

/**
 * 
 * quicklist redis3.2 中新加的数据结构。用在列表的底层实现
 * 
 *  quicklist是由ziplist组成的双向链表，链表中的每一个节点都以压缩列表ziplist的结构保存着数据，
 *  而ziplist有多个entry节点，保存着数据。相当与一个quicklist节点保存的是一片数据，而不再是一个数据
 * **/

/* Node, quicklist, and Iterator are the only data structures used currently. */

/* quicklistNode is a 32 byte struct describing a ziplist for a quicklist.
 * We use bit fields keep the quicklistNode at 32 bytes.
 * count: 16 bits, max 65536 (max zl bytes is 65k, so max count actually < 32k).
 * encoding: 2 bits, RAW=1, LZF=2.
 * container: 2 bits, NONE=1, ZIPLIST=2.
 * recompress: 1 bit, bool, true if node is temporarry decompressed for usage.
 * attempted_compress: 1 bit, boolean, used for verifying during testing.
 * extra: 10 bits, free for future use; pads out the remainder of 32 bits */
// quicklist节点结构
/**
 * list-max-ziplist-size -2 
 * 当数字为负数，表示以下含义：
 *  -1 每个quicklistNode节点的ziplist字节大小不能超过4kb。（建议）
 *  -2 每个quicklistNode节点的ziplist字节大小不能超过8kb。（默认配置）
 *  -3 每个quicklistNode节点的ziplist字节大小不能超过16kb。（一般不建议）
 *  -4 每个quicklistNode节点的ziplist字节大小不能超过32kb。（不建议）
 *  -5 每个quicklistNode节点的ziplist字节大小不能超过64kb。（正常工作量不建议）
 * 当数字为正数，表示：ziplist结构所最多包含的entry个数。最大值为 215215。
 * 
 * compress成员对应的配置：list-compress-depth 0 
 * 后面的数字有以下含义：
 *  0 表示不压缩。（默认）
 *  1 表示quicklist列表的两端各有1个节点不压缩，中间的节点压缩。
 *  2 表示quicklist列表的两端各有2个节点不压缩，中间的节点压缩。
 *  3 表示quicklist列表的两端各有3个节点不压缩，中间的节点压缩。
 * 
 * 
 * 
 * **/
typedef struct quicklistNode {
    // 指向上一个node节点
    struct quicklistNode *prev;
    // 指向下一个node节点
    struct quicklistNode *next;
    // 数据指针，如果当前节点的数据没有压缩，它指向一个ziplist结构；否则指向一个quicklistLZF结构
    // 不设置压缩数据参数recompress时指向一个ziplist结构
    // 设置压缩数据参数recompress指向quicklistLZF结构
    unsigned char *zl;
    // zl指向的ziplist的总大小(包括zlbytes,zltail,zllen,zlend和各个数据项)
    // 注意：如果zl被压缩了，sz值仍然是压缩前的ziplist大小
    unsigned int sz;             /* ziplist size in bytes */
    //表示ziplist里面包含的数据项个数 
    unsigned int count : 16;     /* count of items in ziplist */

    //表示是否采用了LZF压缩算法压缩quicklist节点，1表示压缩过，2表示没压缩，占2 bits长度
    unsigned int encoding : 2;   /* RAW==1 or LZF==2 */

    // 表示一个quicklistNode节点是否采用ziplist结构保存数据，
    // 2表示压缩了，1表示没压缩，默认是2，占2bits长度
    unsigned int container : 2;  /* NONE==1 or ZIPLIST==2 */

    // 标记quicklist节点的ziplist之前是否被解压缩过，占1bit长度
    // 如果recompress为1，则等待被再次压缩
    unsigned int recompress : 1; /* was this node previous compressed? */

    //测试时使用
    unsigned int attempted_compress : 1; /* node can't compress; too small */

    //额外扩展位，占10bits长度
    unsigned int extra : 10; /* more bits to steal for future usage */
} quicklistNode;

/* quicklistLZF is a 4+N byte struct holding 'sz' followed by 'compressed'.
 * 'sz' is byte length of 'compressed' field.
 * 'compressed' is LZF data with total (compressed) length 'sz'
 * NOTE: uncompressed length is stored in quicklistNode->sz.
 * When quicklistNode->zl is compressed, node->zl points to a quicklistLZF */
// 压缩过的ziplist结构—quicklistLZF
typedef struct quicklistLZF {
    // 表示被LZF算法压缩后的ziplist的大小
    unsigned int sz; /* LZF size in bytes*/

    // 保存压缩后的ziplist的数组，柔性数组
    char compressed[];
} quicklistLZF;

/* quicklist is a 40 byte struct (on 64-bit systems) describing a quicklist.
 * 'count' is the number of total entries.
 * 'len' is the number of quicklist nodes.
 * 'compress' is: -1 if compression disabled, otherwise it's the number
 *                of quicklistNodes to leave uncompressed at ends of quicklist.
 * 'fill' is the user-requested (or default) fill factor. */
typedef struct quicklist {
    // 指向头节点（左侧第一个节点）的指针
    quicklistNode *head;
    // 指向尾节点（右侧第一个节点）的指针
    quicklistNode *tail;
    // 所有ziplist数据项的个数总和
    unsigned long count;        /* total count of all entries in all ziplists */
    // quicklist节点的个数。
    unsigned long len;          /* number of quicklistNodes */
    // ziplist大小设置，存放list-max-ziplist-size参数的值
    int fill : 16;              /* fill factor for individual nodes */
    // 节点压缩深度设置，存放list-compress-depth参数的值
    unsigned int compress : 16; /* depth of end nodes not to compress;0=off */
} quicklist;

// quicklist的迭代器结构
typedef struct quicklistIter {
    // 指向所属的quicklist的指针
    const quicklist *quicklist;
    // 指向当前迭代的quicklist节点的指针
    quicklistNode *current;
    // 指向当前quicklist节点中迭代的ziplist
    unsigned char *zi;
    // 当前ziplist结构中的偏移量 
    long offset; /* offset in current ziplist */
    // 迭代方向
    int direction;
} quicklistIter;

// 管理quicklist中quicklistNode节点中ziplist信息的结构
typedef struct quicklistEntry {
    // 指向所属的quicklist的指针
    const quicklist *quicklist;
    // 指向所属的quicklistNode节点的指针
    quicklistNode *node;
    // 指向当前ziplist结构的指针
    unsigned char *zi;
    // 指向当前ziplist结构的字符串vlaue成员
    unsigned char *value;
    // 指向当前ziplist结构的整数value成员
    long long longval;
    // 保存当前ziplist结构的字节数大小
    unsigned int sz;
    // 保存相对ziplist的偏移量
    int offset;
} quicklistEntry;

#define QUICKLIST_HEAD 0
#define QUICKLIST_TAIL -1

/* quicklist node encodings */
#define QUICKLIST_NODE_ENCODING_RAW 1
#define QUICKLIST_NODE_ENCODING_LZF 2

/* quicklist compression disable */
#define QUICKLIST_NOCOMPRESS 0

/* quicklist container formats */
#define QUICKLIST_NODE_CONTAINER_NONE 1
#define QUICKLIST_NODE_CONTAINER_ZIPLIST 2

#define quicklistNodeIsCompressed(node)                                        \
    ((node)->encoding == QUICKLIST_NODE_ENCODING_LZF)

/* Prototypes */
// 创建一个空的quicklist
quicklist *quicklistCreate(void);
// 创建一个quicklist，并设置属性fill,compress
quicklist *quicklistNew(int fill, int compress);
// 设置quicklist的属性depth  compress值
void quicklistSetCompressDepth(quicklist *quicklist, int depth);
// 设置quicklist的属性值fill
void quicklistSetFill(quicklist *quicklist, int fill);
// 设置quicklist的属性值fill,depth(compress)值
void quicklistSetOptions(quicklist *quicklist, int fill, int depth);
// 释放quicklist,并释放所有的节点
void quicklistRelease(quicklist *quicklist);
// 将新的数据项push到头部
int quicklistPushHead(quicklist *quicklist, void *value, const size_t sz);
// 将新的数据项push到尾部
int quicklistPushTail(quicklist *quicklist, void *value, const size_t sz);
// 向quicklist中push一个新的节点 where是方向，0 head -1 tail 
void quicklistPush(quicklist *quicklist, void *value, const size_t sz,
                   int where);
// 在quick尾部追加指针zl, zl指向ziplist                   
void quicklistAppendZiplist(quicklist *quicklist, unsigned char *zl);
// 从zl中取出节点值，追加到quicklist中 
quicklist *quicklistAppendValuesFromZiplist(quicklist *quicklist,
                                            unsigned char *zl);
// 通过zl(ziplist)生成一个新的quicklist 
// 将ziplist数据转换成quicklist                                           
quicklist *quicklistCreateFromZiplist(int fill, int compress,
                                      unsigned char *zl);
// 在node节点后添加一个值value                                     
void quicklistInsertAfter(quicklist *quicklist, quicklistEntry *node,
                          void *value, const size_t sz);
// 在node节点前面添加一个值value                          
void quicklistInsertBefore(quicklist *quicklist, quicklistEntry *node,
                           void *value, const size_t sz);
// 删除节点entry                           
void quicklistDelEntry(quicklistIter *iter, quicklistEntry *entry);
// 替换索引中的节点值
int quicklistReplaceAtIndex(quicklist *quicklist, long index, void *data,
                            int sz);
// 删除指定范围的节点值 start-stop                            
int quicklistDelRange(quicklist *quicklist, const long start, const long stop);
// 获取一个quicklist的迭代器
quicklistIter *quicklistGetIterator(const quicklist *quicklist, int direction);
// 获取一个指向索引idx位置上的迭代器
quicklistIter *quicklistGetIteratorAtIdx(const quicklist *quicklist,
                                         int direction, const long long idx);
// 通过迭代器获取下一个节点                                         
int quicklistNext(quicklistIter *iter, quicklistEntry *node);
// 释放迭代器
void quicklistReleaseIterator(quicklistIter *iter);
// 拷贝一个quicklist
quicklist *quicklistDup(quicklist *orig);
// 返回在索引index上的节点，保存在entry上
int quicklistIndex(const quicklist *quicklist, const long long index,
                   quicklistEntry *entry);
void quicklistRewind(quicklist *quicklist, quicklistIter *li);
void quicklistRewindTail(quicklist *quicklist, quicklistIter *li);
// 翻转quicklist
void quicklistRotate(quicklist *quicklist);
// pop底层函数接口
int quicklistPopCustom(quicklist *quicklist, int where, unsigned char **data,
                       unsigned int *sz, long long *sval,
                       void *(*saver)(unsigned char *data, unsigned int sz));
// pop操作，where的pop的方向，
// 如果弹出节点的值的是字符串，存储在data,sz中     
// 如果弹出节点的值是整型值，存储在slong               
int quicklistPop(quicklist *quicklist, int where, unsigned char **data,
                 unsigned int *sz, long long *slong);
unsigned long quicklistCount(const quicklist *ql);
// 节点之间的比较函数
int quicklistCompare(unsigned char *p1, unsigned char *p2, int p2_len);
size_t quicklistGetLzf(const quicklistNode *node, void **data);

#ifdef REDIS_TEST
int quicklistTest(int argc, char *argv[]);
#endif

/* Directions for iterators */
#define AL_START_HEAD 0
#define AL_START_TAIL 1

#endif /* __QUICKLIST_H__ */
