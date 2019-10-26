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

#ifndef __INTSET_H
#define __INTSET_H
#include <stdint.h>
/**
 * 整数集合 
 * 整数集合是集合键的底层实现之一，
 * 数组以有序，无重复的方式保存元素 在有需要的时候，程序会根据新添加的元素的类型，改变这个数组的类型
 * 数组升级操作为整数集合带来灵活性，并且尽可能地节约内存
 * 整数集合支持升级操作，不支持降级操作
 * 
 * **/
typedef struct intset {
    // 编码方式
    uint32_t encoding;
    // 集合包含的元素数量
    uint32_t length;
    // 保存元素的数组 
    // 各个项在数组中按值得大小从小到大有序地排列，并且数组中不包含任何重复项
    // 数组的真正类型取决于encoding属性值，如：INTSET_ENC_INT32，content类型是int32_t
    // eg:encoding=INTSET_ENC_INT16,length=5 contents大小：sizeof(int16_t)*5=16*5=80
    // 数组支持升级，不支持降级 按最最大元素类型做类型
    int8_t contents[];
} intset;

// 创建一个新的整数集合
intset *intsetNew(void);
// 将给定的元素添加到整数集合中 success 返回是否成功与否
intset *intsetAdd(intset *is, int64_t value, uint8_t *success);
// 从整数集合中移除给定的元素
intset *intsetRemove(intset *is, int64_t value, int *success);
// 检查给定的值是否存在于集合
uint8_t intsetFind(intset *is, int64_t value);
// 从整数集合中随机返回一个元素 
int64_t intsetRandom(intset *is);
// 从整数集合中取出指定索引的值
uint8_t intsetGet(intset *is, uint32_t pos, int64_t *value);
// 返回整数集合中元素个数
uint32_t intsetLen(const intset *is);
// 返回整数集合占用的内存字节数
size_t intsetBlobLen(intset *is);

#ifdef REDIS_TEST 
int intsetTest(int argc, char *argv[]);
#endif

#endif // __INTSET_H
