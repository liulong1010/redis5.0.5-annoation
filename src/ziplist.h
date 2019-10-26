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

#ifndef _ZIPLIST_H
#define _ZIPLIST_H

/**
 * 压缩表 是列表键和哈希键的底层实现之一
 * 
 * 当一个列表键只包含少量的列表项，并且每个列表要么就是小整数值，要么就是长度比较短的字符串，
 * 那么Redis就会使用压缩表来做列表键的底层实现
 * 
 * 压缩列表是Redis为了节约内存而开发的，是有一些列特殊编码的连续内存块组成的顺序数据结构
 * 
 * {zlbytes}{zltail}{zllen}{entry1}{entry2}{...}{entryN}{zlend}
 * zlbytes记录整个压缩列表占用的内存字节数，在对压缩列表进行内存重新分配，或计算zlend的位置使用
 * zltail 记录压缩列表表尾节点距离压缩列表的起始地址有多少字节；通过这个偏移量，可以直接定位到表尾地址
 * zllen 压缩表节点数
 * entryX 列表节点
 * zlend 特殊值0xFF，用于标记压缩列表的末端
 * 
 * 压缩表节点组成部分
 * {previous_entry_length}{encoding}{content}
 * previous_entry_length:  记录压缩了列表前一个节点长度
 * encoding 记录节点属性content属性保存数据类型以及长度
 * content  属性值
 * 
 * e1,e2,e3,e4
 *
 * 压缩列表 连续内存 会产生连续更新
 * 添加一个元素和删除一个元素 会引起连锁更新，但这种的操作出现几率并不高
 * **/
#define ZIPLIST_HEAD 0
#define ZIPLIST_TAIL 1

// 创建一个新的压缩列表
unsigned char *ziplistNew(void);
// 将连个压缩列表merge成一个新的压缩列表
unsigned char *ziplistMerge(unsigned char **first, unsigned char **second);
// 创建一个包含给定值的节点，并将这个新的节点添加到压缩列表的表头或表尾
unsigned char *ziplistPush(unsigned char *zl, unsigned char *s, unsigned int slen, int where);
// 返回压缩列表给定索引的节点
unsigned char *ziplistIndex(unsigned char *zl, int index);
// 返回给定节点的下一个节点
unsigned char *ziplistNext(unsigned char *zl, unsigned char *p);
// 返回给定节点的上一个节点
unsigned char *ziplistPrev(unsigned char *zl, unsigned char *p);
// 获取给定节点所保存的值
unsigned int ziplistGet(unsigned char *p, unsigned char **sval, unsigned int *slen, long long *lval);
// 将包含给定值的新节点插入给定节点之后
unsigned char *ziplistInsert(unsigned char *zl, unsigned char *p, unsigned char *s, unsigned int slen);
// 从压缩列表中删除给定的节点
unsigned char *ziplistDelete(unsigned char *zl, unsigned char **p);
// 删除压缩列表给定索引上的连续多个节点
unsigned char *ziplistDeleteRange(unsigned char *zl, int index, unsigned int num);
unsigned int ziplistCompare(unsigned char *p, unsigned char *s, unsigned int slen);
// 在压缩列表中查找并返回包含了给定值得节点
unsigned char *ziplistFind(unsigned char *p, unsigned char *vstr, unsigned int vlen, unsigned int skip);
// 返回压缩列表目前包含的节点数量
unsigned int ziplistLen(unsigned char *zl);
//  返回压缩列表目前占用的内存字节数
size_t ziplistBlobLen(unsigned char *zl);
// 打印给定压缩列表的信息
void ziplistRepr(unsigned char *zl);

#ifdef REDIS_TEST
int ziplistTest(int argc, char *argv[]);
#endif

#endif /* _ZIPLIST_H */
