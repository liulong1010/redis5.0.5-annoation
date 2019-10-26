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

#include "server.h"
#include "lzf.h"    /* LZF compression library */
#include "zipmap.h"
#include "endianconv.h"
#include "stream.h"

#include <math.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/param.h>

#define rdbExitReportCorruptRDB(...) rdbCheckThenExit(__LINE__,__VA_ARGS__)

extern int rdbCheckMode;
void rdbCheckError(const char *fmt, ...);
void rdbCheckSetError(const char *fmt, ...);

// rdb检查后退出
void rdbCheckThenExit(int linenum, char *reason, ...) {
    va_list ap;
    char msg[1024];
    int len;

    len = snprintf(msg,sizeof(msg),
        "Internal error in RDB reading function at rdb.c:%d -> ", linenum);
    // 读取不定参数
    va_start(ap,reason);
    vsnprintf(msg+len,sizeof(msg)-len,reason,ap);
    va_end(ap);

    if (!rdbCheckMode) {
        serverLog(LL_WARNING, "%s", msg); // 写日志
        char *argv[2] = {"",server.rdb_filename};
        redis_check_rdb_main(2,argv,NULL); // rdb检查主函数
    } else {
        rdbCheckError("%s",msg);
    }
    exit(1);
}

/**
 * 将长度为len的字符数组p写入到rdb中
 * 写 raw格式到rdb文件中
 * 成功返回len,失败返回-1
 * **/
static int rdbWriteRaw(rio *rdb, void *p, size_t len) {
    // 调用rio的write接口写入
    if (rdb && rioWrite(rdb,p,len) == 0)
        return -1;
    return len;
}

/* This is just a wrapper for the low level function rioRead() that will
 * automatically abort if it is not possible to read the specified amount
 * of bytes. */
// 加载raw格式的字符串
void rdbLoadRaw(rio *rdb, void *buf, uint64_t len) {
    if (rioRead(rdb,buf,len) == 0) {
        rdbExitReportCorruptRDB(
            "Impossible to read %llu bytes in rdbLoadRaw()",
            (unsigned long long) len);
        return; /* Not reached. */
    }
}

/*
 * 将长度为 1 字节的字符 type 写入到 rdb 文件中。
 * 写类型到文件中
 */
int rdbSaveType(rio *rdb, unsigned char type) {
    return rdbWriteRaw(rdb,&type,1);
}

/* Load a "type" in RDB format, that is a one byte unsigned integer.
 * 从 rdb 中载入 1 字节长的 type 数据。
 * This function is not only used to load object types, but also special
 * "types" like the end-of-file type, the EXPIRE type, and so forth.
 * 
 *  */
// 返回rdb文件的加载类型
int rdbLoadType(rio *rdb) {
    unsigned char type;
    if (rioRead(rdb,&type,1) == 0) return -1;
    return type;
}

/* This is only used to load old databases stored with the RDB_OPCODE_EXPIRETIME
 * opcode. New versions of Redis store using the RDB_OPCODE_EXPIRETIME_MS
 * opcode. */
// 载入以秒为单位的过期时间，长度为4字节
time_t rdbLoadTime(rio *rdb) {
    int32_t t32;
    rdbLoadRaw(rdb,&t32,4);
    return (time_t)t32;
}

/*
 * 将长度为 8 字节的毫秒过期时间写入到 rdb 中。
 */
int rdbSaveMillisecondTime(rio *rdb, long long t) {
    int64_t t64 = (int64_t) t;
    memrev64ifbe(&t64); /* Store in little endian. */
    return rdbWriteRaw(rdb,&t64,8);
}

/* This function loads a time from the RDB file. It gets the version of the
 * RDB because, unfortunately, before Redis 5 (RDB version 9), the function
 * failed to convert data to/from little endian, so RDB files with keys having
 * expires could not be shared between big endian and little endian systems
 * (because the expire time will be totally wrong). The fix for this is just
 * to call memrev64ifbe(), however if we fix this for all the RDB versions,
 * this call will introduce an incompatibility for big endian systems:
 * after upgrading to Redis version 5 they will no longer be able to load their
 * own old RDB files. Because of that, we instead fix the function only for new
 * RDB versions, and load older RDB versions as we used to do in the past,
 * allowing big endian systems to load their own old RDB files. */
/*
 * 从 rdb 中载入 8 字节长的毫秒过期时间。
 */
long long rdbLoadMillisecondTime(rio *rdb, int rdbver) {
    int64_t t64;
    rdbLoadRaw(rdb,&t64,8);
    if (rdbver >= 9) /* Check the top comment of this function. */
        memrev64ifbe(&t64); /* Convert in big endian if the system is BE. */
    return (long long)t64;
}

/* Saves an encoded length. The first two bits in the first byte are used to
 * hold the encoding type. See the RDB_* definitions for more information
 * on the types of encoding.
 *  对 len 进行特殊编码之后写入到 rdb 。
 *  写入成功返回保存编码后的 len 所需的字节数
 *  */
int rdbSaveLen(rio *rdb, uint64_t len) {
    unsigned char buf[2];
    size_t nwritten;

    // 第一个字节的前两位用来编码类型
    if (len < (1<<6)) {
        /* Save a 6 bit len */
        buf[0] = (len&0xFF)|(RDB_6BITLEN<<6);
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        nwritten = 1;
    } else if (len < (1<<14)) {
        /* Save a 14 bit len */
        buf[0] = ((len>>8)&0xFF)|(RDB_14BITLEN<<6);
        buf[1] = len&0xFF;
        if (rdbWriteRaw(rdb,buf,2) == -1) return -1;
        nwritten = 2;
    } else if (len <= UINT32_MAX) {
        /* Save a 32 bit len */
        buf[0] = RDB_32BITLEN;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        uint32_t len32 = htonl(len);
        if (rdbWriteRaw(rdb,&len32,4) == -1) return -1;
        nwritten = 1+4;
    } else {
        /* Save a 64 bit len */
        buf[0] = RDB_64BITLEN;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
        len = htonu64(len);
        if (rdbWriteRaw(rdb,&len,8) == -1) return -1;
        nwritten = 1+8;
    }
    return nwritten;
}


/* Load an encoded length. If the loaded length is a normal length as stored
 * with rdbSaveLen(), the read length is set to '*lenptr'. If instead the
 * loaded length describes a special encoding that follows, then '*isencoded'
 * is set to 1 and the encoding format is stored at '*lenptr'.
 *
 * See the RDB_ENC_* definitions in rdb.h for more information on special
 * encodings.
 *
 * The function returns -1 on error, 0 on success. */
// 加载长度  isencoded 编码格式指针， lenptr 长度指针
int rdbLoadLenByRef(rio *rdb, int *isencoded, uint64_t *lenptr) {
    unsigned char buf[2];
    int type;

    if (isencoded) *isencoded = 0;
    if (rioRead(rdb,buf,1) == 0) return -1;
    type = (buf[0]&0xC0)>>6;
    if (type == RDB_ENCVAL) {
        /* Read a 6 bit encoding type. */
        if (isencoded) *isencoded = 1;
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_6BITLEN) {
        /* Read a 6 bit len. */
        *lenptr = buf[0]&0x3F;
    } else if (type == RDB_14BITLEN) {
        /* Read a 14 bit len. */
        if (rioRead(rdb,buf+1,1) == 0) return -1;
        *lenptr = ((buf[0]&0x3F)<<8)|buf[1];
    } else if (buf[0] == RDB_32BITLEN) {
        /* Read a 32 bit len. */
        uint32_t len;
        if (rioRead(rdb,&len,4) == 0) return -1;
        *lenptr = ntohl(len);
    } else if (buf[0] == RDB_64BITLEN) {
        /* Read a 64 bit len. */
        uint64_t len;
        if (rioRead(rdb,&len,8) == 0) return -1;
        *lenptr = ntohu64(len);
    } else {
        rdbExitReportCorruptRDB(
            "Unknown length encoding %d in rdbLoadLen()",type);
        return -1; /* Never reached. */
    }
    return 0;
}

/* This is like rdbLoadLenByRef() but directly returns the value read
 * from the RDB stream, signaling an error by returning RDB_LENERR
 * (since it is a too large count to be applicable in any Redis data
 * structure). */
// 加载长度  isencoded 编码格式 成功返回长度
uint64_t rdbLoadLen(rio *rdb, int *isencoded) {
    uint64_t len;

    if (rdbLoadLenByRef(rdb,isencoded,&len) == -1) return RDB_LENERR;
    return len;
}

/* Encodes the "value" argument as integer when it fits in the supported ranges
 * for encoded types. If the function successfully encodes the integer, the
 * representation is stored in the buffer pointer to by "enc" and the string
 * length is returned. Otherwise 0 is returned. 
 * 尝试使用特殊的整数编码来保存 value ，这要求它的值必须在给定范围之内
 * 
 * 如果可以编码的话，将编码后的值保存在 enc 指针中，
 * 并返回值在编码后所需的长度。
 * 如果不能编码的话，返回 0 。
 * 
 * */
int rdbEncodeInteger(long long value, unsigned char *enc) {
    if (value >= -(1<<7) && value <= (1<<7)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT8;
        enc[1] = value&0xFF;
        return 2;
    } else if (value >= -(1<<15) && value <= (1<<15)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT16;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        return 3;
    } else if (value >= -((long long)1<<31) && value <= ((long long)1<<31)-1) {
        enc[0] = (RDB_ENCVAL<<6)|RDB_ENC_INT32;
        enc[1] = value&0xFF;
        enc[2] = (value>>8)&0xFF;
        enc[3] = (value>>16)&0xFF;
        enc[4] = (value>>24)&0xFF;
        return 5;
    } else {
        return 0;
    }
}

/* Loads an integer-encoded object with the specified encoding type "enctype".
 * The returned value changes according to the flags, see
 * rdbGenerincLoadStringObject() for more info.
 *  */
// 加载整数对象 enctype 整数编码格式 
// 如果encoded参数被设置了的话，那么可能会返回一个整数编码的字符串对象，否则，字符串总是位编码的
// 根据flags标记，来决定返回的是字符对象还是字符串指针 
void *rdbLoadIntegerObject(rio *rdb, int enctype, int flags, size_t *lenptr) {
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    int encode = flags & RDB_LOAD_ENC;
    unsigned char enc[4];
    long long val;

    // 整数编码
    if (enctype == RDB_ENC_INT8) {
        if (rioRead(rdb,enc,1) == 0) return NULL;
        val = (signed char)enc[0];
    } else if (enctype == RDB_ENC_INT16) {
        uint16_t v;
        if (rioRead(rdb,enc,2) == 0) return NULL;
        v = enc[0]|(enc[1]<<8);
        val = (int16_t)v;
    } else if (enctype == RDB_ENC_INT32) {
        uint32_t v;
        if (rioRead(rdb,enc,4) == 0) return NULL;
        v = enc[0]|(enc[1]<<8)|(enc[2]<<16)|(enc[3]<<24);
        val = (int32_t)v;
    } else {
        val = 0; /* anti-warning */
        rdbExitReportCorruptRDB("Unknown RDB integer encoding type %d",enctype);
    }
    if (plain || sds) {
        char buf[LONG_STR_SIZE], *p;
        int len = ll2string(buf,sizeof(buf),val);
        if (lenptr) *lenptr = len;
        p = plain ? zmalloc(len) : sdsnewlen(SDS_NOINIT,len);
        memcpy(p,buf,len);
        return p;
    } else if (encode) { 
        // 整数编码的字符串
        return createStringObjectFromLongLongForValue(val);
    } else { 
         // 未编码
        return createObject(OBJ_STRING,sdsfromlonglong(val));
    }
}

/* String objects in the form "2391" "-100" without any space and with a
 * range of values that can fit in an 8, 16 or 32 bit signed value can be
 * encoded as integers to save space 
 * 
 * 那些保存像是 "2391" 、 "-100" 这样的字符串的字符串对象，
 * 可以将它们的值保存到 8 位、16 位或 32 位的带符号整数值中，
 * 从而节省一些内存。
 * 
 * 这个函数就是尝试将字符串编码成整数，
 * 如果成功的话，返回保存整数值所需的字节数，这个值必然大于 0 。
 *
 * 如果转换失败，那么返回 0 。
 * */
int rdbTryIntegerEncoding(char *s, size_t len, unsigned char *enc) {
    long long value;
    char *endptr, buf[32];

    /* Check if it's possible to encode this value as a number */
     // 尝试将值转换为整数
    value = strtoll(s, &endptr, 10);
    if (endptr[0] != '\0') return 0;
    // 尝试将转换后的整数转换回字符串
    ll2string(buf,32,value);

    /* If the number converted back into a string is not identical
     * then it's not possible to encode the string as integer */
    // 检查两次转换后的整数值能否还原回原来的字符串
    // 如果不行的话，那么转换失败
    if (strlen(buf) != len || memcmp(buf,s,len)) return 0;

    // 转换成功，对转换所得的整数进行特殊编码
    return rdbEncodeInteger(value,enc);
}

 // 保存压缩后的字符串到 rdb 。 data 字符串，compress_len压缩后长度,original_len原始长度
ssize_t rdbSaveLzfBlob(rio *rdb, void *data, size_t compress_len,
                       size_t original_len) {
    unsigned char byte;
    ssize_t n, nwritten = 0;

    /* Data compressed! Let's save it on disk */
    // 写入类型，说明这是一个 LZF 压缩字符串
    byte = (RDB_ENCVAL<<6)|RDB_ENC_LZF;
    if ((n = rdbWriteRaw(rdb,&byte,1)) == -1) goto writeerr;
    nwritten += n;

    // 写入字符串压缩后的长度
    if ((n = rdbSaveLen(rdb,compress_len)) == -1) goto writeerr;
    nwritten += n;

    // 写入字符串未压缩时的长度
    if ((n = rdbSaveLen(rdb,original_len)) == -1) goto writeerr;
    nwritten += n;

    // 写入压缩后的字符串
    if ((n = rdbWriteRaw(rdb,data,compress_len)) == -1) goto writeerr;
    nwritten += n;

    return nwritten;

writeerr:
    return -1;
}

/*
 * 尝试对输入字符串 s 进行压缩，
 * 如果压缩成功，那么将压缩后的字符串保存到 rdb 中。
 *
 * 函数在成功时返回保存压缩后的 s 所需的字节数，
 * 压缩失败或者内存不足时返回 0 ，
 * 写入失败时返回 -1 。
 */
ssize_t rdbSaveLzfStringObject(rio *rdb, unsigned char *s, size_t len) {
    size_t comprlen, outlen;
    void *out;

    /* We require at least four bytes compression for this to be worth it */
    // 压缩字符串
    if (len <= 4) return 0;
    outlen = len-4;
    if ((out = zmalloc(outlen+1)) == NULL) return 0;
    comprlen = lzf_compress(s, len, out, outlen);
    if (comprlen == 0) {
        zfree(out);
        return 0;
    }
     // 保存压缩后的字符串到 rdb 。
    ssize_t nwritten = rdbSaveLzfBlob(rdb, out, comprlen, len);
    zfree(out);
    return nwritten;
}

/* Load an LZF compressed string in RDB format. The returned value
 * changes according to 'flags'. For more info check the
 * rdbGenericLoadStringObject() function. */
/*
 * 从 rdb 中载入被 LZF 压缩的字符串，解压它，并创建相应的字符串对象。
 * 需要根据flags标识
 */
void *rdbLoadLzfStringObject(rio *rdb, int flags, size_t *lenptr) {
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    uint64_t len, clen;
    unsigned char *c = NULL;
    char *val = NULL;

    // 读入压缩后的缓存长度
    if ((clen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    // 读入字符串未压缩前的长度
    if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
    // 压缩缓存空间
    if ((c = zmalloc(clen)) == NULL) goto err;

    /* Allocate our target according to the uncompressed size. */
    // 分配空间
    if (plain) {
        val = zmalloc(len);
    } else {
        val = sdsnewlen(SDS_NOINIT,len);
    }
    if (lenptr) *lenptr = len;

    /* Load the compressed representation and uncompress it to target. */
     // 读入压缩后的缓存
    if (rioRead(rdb,c,clen) == 0) goto err;
    // 解压缓存，得出字符串
    if (lzf_decompress(c,clen,val,len) == 0) {
        if (rdbCheckMode) rdbCheckSetError("Invalid LZF compressed string");
        goto err;
    }
    zfree(c);

    if (plain || sds) {
        // 直接返回字符串
        return val;
    } else { 
         // 创建字符串对象
        return createObject(OBJ_STRING,val);
    }
err:
    zfree(c);
    if (plain)
        zfree(val);
    else
        sdsfree(val);
    return NULL;
}

/* Save a string object as [len][data] on disk. If the object is a string
 * representation of an integer value we try to save it in a special form
 * 
 * 以 [len][data] 的形式将字符串对象写入到 rdb 中。
 *
 * 如果对象是字符串表示的整数值，那么程序尝试以特殊的形式来保存它。
 *
 * 函数返回保存字符串所需的空间字节数
 * 
 *  */
ssize_t rdbSaveRawString(rio *rdb, unsigned char *s, size_t len) {
    int enclen;
    ssize_t n, nwritten = 0;

    /* Try integer encoding */
    // 尝试进行整数值编码
    if (len <= 11) {
        unsigned char buf[5];
        if ((enclen = rdbTryIntegerEncoding((char*)s,len,buf)) > 0) {
            // 整数转换成功，写入
            if (rdbWriteRaw(rdb,buf,enclen) == -1) return -1;
            // 返回字节数
            return enclen;
        }
    }

    /* Try LZF compression - under 20 bytes it's unable to compress even
     * aaaaaaaaaaaaaaaaaa so skip it
     * 
     * 如果字符串长度大于 20 ，并且服务器开启了 LZF 压缩，
     * 那么在保存字符串到数据库之前，先对字符串进行 LZF 压缩。
     * 
     *  */
    if (server.rdb_compression && len > 20) {
        n = rdbSaveLzfStringObject(rdb,s,len);
        if (n == -1) return -1;
        if (n > 0) return n;
        /* Return value of 0 means data can't be compressed, save the old way */
    }

    /* Store verbatim(一字不差，逐字的)))) */
    // 执行到这里，说明值 s 既不能编码为整数
    // 也不能被压缩
    // 那么直接将它写入到 rdb 中

    // 写入长度
    if ((n = rdbSaveLen(rdb,len)) == -1) return -1;
    nwritten += n;

    // 写入内容
    if (len > 0) {
        if (rdbWriteRaw(rdb,s,len) == -1) return -1;
        nwritten += len;
    }
    return nwritten;
}

/* Save a long long value as either an encoded string or a string.
 *
 * 将输入的 long long 类型的 value 转换成一个特殊编码的字符串，
 * 或者是一个普通的字符串表示的整数，
 * 然后将它写入到 rdb 中。
 *
 * 函数返回在 rdb 中保存 value 所需的字节数。
 */
ssize_t rdbSaveLongLongAsStringObject(rio *rdb, long long value) {
    unsigned char buf[32];
    ssize_t n, nwritten = 0;
    // 尝试以节省空间的方式编码整数值 value 
    int enclen = rdbEncodeInteger(value,buf);

    // 编码成功，直接写入编码后的缓存
    // 比如，值 1 可以编码为 11 00 0001
    if (enclen > 0) {
        return rdbWriteRaw(rdb,buf,enclen);
    } else {

    // 编码失败，将整数值转换成对应的字符串来保存
    // 比如，值 999999999 要编码成 "999999999" ，
    // 因为这个值没办法用节省空间的方式编码    

        /* Encode as string */
        // 转换成字符串表示
        enclen = ll2string((char*)buf,32,value);
        serverAssert(enclen < 32);
        // 写入字符串长度
        if ((n = rdbSaveLen(rdb,enclen)) == -1) return -1;
        nwritten += n;
         // 写入字符串
        if ((n = rdbWriteRaw(rdb,buf,enclen)) == -1) return -1;
        // 返回长度
        nwritten += n;
    }
    return nwritten;
}

/* Like rdbSaveRawString() gets a Redis object instead.
 *
 * 将给定的字符串对象 obj 保存到 rdb 中。
 *
 * 函数返回 rdb 保存字符串对象所需的字节数。
 *
 */
ssize_t rdbSaveStringObject(rio *rdb, robj *obj) {
    /* Avoid to decode the object, then encode it again, if the
     * object is already integer encoded. */
    // 尝试对 INT 编码的字符串进行特殊编码
    if (obj->encoding == OBJ_ENCODING_INT) {
        return rdbSaveLongLongAsStringObject(rdb,(long)obj->ptr);
    } else {
       // 保存 STRING 编码的字符串
        serverAssertWithInfo(NULL,obj,sdsEncodedObject(obj));
        return rdbSaveRawString(rdb,obj->ptr,sdslen(obj->ptr));
    }
}

/* Load a string object from an RDB file according to flags:
 *
 * RDB_LOAD_NONE (no flags): load an RDB object, unencoded.
 * RDB_LOAD_ENC: If the returned type is a Redis object, try to
 *               encode it in a special way to be more memory
 *               efficient. When this flag is passed the function
 *               no longer guarantees that obj->ptr is an SDS string.
 * RDB_LOAD_PLAIN: Return a plain string allocated with zmalloc()
 *                 instead of a Redis object with an sds in it.
 * RDB_LOAD_SDS: Return an SDS string instead of a Redis object.
 *
 * On I/O error NULL is returned.
 * 
 * 从rdb中载入一个字符串对象
 * 
 * flags 为 RDB_LOAD_NONE  未编码rdb对象
 * RDB_LOAD_ENC 指定编码的redis对象
 * RDB_LOAD_PLAIN 直接返回字符串
 * RDB_LOAD_SDS   直接返回一个sds字符串
 * 
 * lenptr 返回长度
 */
void *rdbGenericLoadStringObject(rio *rdb, int flags, size_t *lenptr) {
    int encode = flags & RDB_LOAD_ENC;
    int plain = flags & RDB_LOAD_PLAIN;
    int sds = flags & RDB_LOAD_SDS;
    int isencoded;
    uint64_t len;

    // 长度
    len = rdbLoadLen(rdb,&isencoded);

    // 这是一个特殊编码字符串
    if (isencoded) {
        switch(len) {

        // 整数编码    
        case RDB_ENC_INT8:
        case RDB_ENC_INT16:
        case RDB_ENC_INT32:
            return rdbLoadIntegerObject(rdb,len,flags,lenptr);
        // LZF 压缩    
        case RDB_ENC_LZF:
            return rdbLoadLzfStringObject(rdb,flags,lenptr);
        default:
            rdbExitReportCorruptRDB("Unknown RDB string encoding type %d",len);
        }
    }

    // 执行到这里，说明这个字符串即没有被压缩，也不是整数
    // 那么直接从 rdb 中读入它
    if (len == RDB_LENERR) return NULL;
    // 根据flags标识 返回响应的对象
    if (plain || sds) {
        void *buf = plain ? zmalloc(len) : sdsnewlen(SDS_NOINIT,len);
        if (lenptr) *lenptr = len;
        if (len && rioRead(rdb,buf,len) == 0) {
            if (plain)
                zfree(buf);
            else
                sdsfree(buf);
            return NULL;
        }
        return buf;
    } else {
        // 返回字符串redis对象
        robj *o = encode ? createStringObject(SDS_NOINIT,len) :
                           createRawStringObject(SDS_NOINIT,len);
        if (len && rioRead(rdb,o->ptr,len) == 0) {
            decrRefCount(o);
            return NULL;
        }
        return o;
    }
}

// 加载字符串对象(raw)
robj *rdbLoadStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
}
// 以编码方式加载字符串对象
robj *rdbLoadEncodedStringObject(rio *rdb) {
    return rdbGenericLoadStringObject(rdb,RDB_LOAD_ENC,NULL);
}

/* Save a double value. Doubles are saved as strings prefixed by an unsigned
 * 8 bit integer specifying the length of the representation.
 * This 8 bit integer has special values in order to specify the following
 * conditions:
 * 253: not a number
 * 254: + inf
 * 255: - inf
 * 
 * 以字符串形式来保存一个双精度浮点数。
 * 字符串的前面是一个 8 位长的无符号整数值，
 * 它指定了浮点数表示的长度。
 * 
 * 其中， 8 位整数中的以下值用作特殊值，来指示一些特殊情况：
 * 253: not a number
 *      输入不是数
 * 254: + inf
 *      输入为正无穷
 * 255: - inf
 *      输入为负无穷
 */
int rdbSaveDoubleValue(rio *rdb, double val) {
    unsigned char buf[128];
    int len;

    // 不是数
    if (isnan(val)) {
        buf[0] = 253;
        len = 1;
    // 无穷    
    } else if (!isfinite(val)) {
        len = 1;
        buf[0] = (val < 0) ? 255 : 254;
    } else {
    // 转换为整数    
#if (DBL_MANT_DIG >= 52) && (LLONG_MAX == 0x7fffffffffffffffLL)
        /* Check if the float is in a safe range to be casted into a
         * long long. We are assuming that long long is 64 bit here.
         * Also we are assuming that there are no implementations around where
         * double has precision < 52 bit.
         *
         * Under this assumptions we test if a double is inside an interval
         * where casting to long long is safe. Then using two castings we
         * make sure the decimal part is zero. If all this is true we use
         * integer printing function that is much faster. */
        double min = -4503599627370495; /* (2^52)-1 */
        double max = 4503599627370496; /* -(2^52) */
        if (val > min && val < max && val == ((double)((long long)val)))
            ll2string((char*)buf+1,sizeof(buf)-1,(long long)val);
        else
#endif
            snprintf((char*)buf+1,sizeof(buf)-1,"%.17g",val);
        buf[0] = strlen((char*)buf+1);
        len = buf[0]+1;
    }
    // 将字符串写入到 rdb
    return rdbWriteRaw(rdb,buf,len);
}

/* For information about double serialization check rdbSaveDoubleValue()
 * 载入字符串表示的双精度浮点数
 */
int rdbLoadDoubleValue(rio *rdb, double *val) {
    char buf[256];
    unsigned char len;

    // 载入字符串长度
    if (rioRead(rdb,&len,1) == 0) return -1;
    switch(len) {
    // 特殊值    
    case 255: *val = R_NegInf; return 0;
    case 254: *val = R_PosInf; return 0;
    case 253: *val = R_Nan; return 0;
    default:
        // 载入字符串 
        if (rioRead(rdb,buf,len) == 0) return -1;
        buf[len] = '\0';
        sscanf(buf, "%lg", val);
        return 0;
    }
}

/* Saves a double for RDB 8 or greater, where IE754 binary64 format is assumed.
 * We just make sure the integer is always stored in little endian, otherwise
 * the value is copied verbatim from memory to disk.
 *
 * Return -1 on error, the size of the serialized value on success. 
 * 
 * 以二进制的方式保存double
 * */
int rdbSaveBinaryDoubleValue(rio *rdb, double val) {
    memrev64ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Loads a double from RDB 8 or greater. See rdbSaveBinaryDoubleValue() for
 * more info. On error -1 is returned, otherwise 0.
 * 加载double 
 *  */
int rdbLoadBinaryDoubleValue(rio *rdb, double *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev64ifbe(val);
    return 0;
}

/* Like rdbSaveBinaryDoubleValue() but single precision. */
int rdbSaveBinaryFloatValue(rio *rdb, float val) {
    memrev32ifbe(&val);
    return rdbWriteRaw(rdb,&val,sizeof(val));
}

/* Like rdbLoadBinaryDoubleValue() but single precision. */
int rdbLoadBinaryFloatValue(rio *rdb, float *val) {
    if (rioRead(rdb,val,sizeof(*val)) == 0) return -1;
    memrev32ifbe(val);
    return 0;
}

/* Save the object type of object "o".
 * 将对象o的类型下入到rdb中
 */
int rdbSaveObjectType(rio *rdb, robj *o) {
    switch (o->type) {
    case OBJ_STRING:
        return rdbSaveType(rdb,RDB_TYPE_STRING);
    case OBJ_LIST:
        if (o->encoding == OBJ_ENCODING_QUICKLIST)
            return rdbSaveType(rdb,RDB_TYPE_LIST_QUICKLIST);
        else
            serverPanic("Unknown list encoding");
    case OBJ_SET:
        if (o->encoding == OBJ_ENCODING_INTSET)
            return rdbSaveType(rdb,RDB_TYPE_SET_INTSET);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_SET);
        else
            serverPanic("Unknown set encoding");
    case OBJ_ZSET:
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_ZIPLIST);
        else if (o->encoding == OBJ_ENCODING_SKIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_ZSET_2);
        else
            serverPanic("Unknown sorted set encoding");
    case OBJ_HASH:
        if (o->encoding == OBJ_ENCODING_ZIPLIST)
            return rdbSaveType(rdb,RDB_TYPE_HASH_ZIPLIST);
        else if (o->encoding == OBJ_ENCODING_HT)
            return rdbSaveType(rdb,RDB_TYPE_HASH);
        else
            serverPanic("Unknown hash encoding");
    case OBJ_STREAM:
        return rdbSaveType(rdb,RDB_TYPE_STREAM_LISTPACKS);
    case OBJ_MODULE:
        return rdbSaveType(rdb,RDB_TYPE_MODULE_2);
    default:
        serverPanic("Unknown object type");
    }
    return -1; /* avoid warning */
}

/* Use rdbLoadType() to load a TYPE in RDB format, but returns -1 if the
 * type is not specifically a valid Object Type.
 * 加载rdb数据对象类型 
 *  */
int rdbLoadObjectType(rio *rdb) {
    int type;
    if ((type = rdbLoadType(rdb)) == -1) return -1;
    if (!rdbIsObjectType(type)) return -1;
    return type;
}

/* This helper function serializes a consumer group Pending Entries List (PEL)
 * into the RDB file. The 'nacks' argument tells the function if also persist
 * the informations about the not acknowledged message, or if to persist
 * just the IDs: this is useful because for the global consumer group PEL
 * we serialized the NACKs as well, but when serializing the local consumer
 * PELs we just add the ID, that will be resolved inside the global PEL to
 * put a reference to the same structure. */
ssize_t rdbSaveStreamPEL(rio *rdb, rax *pel, int nacks) {
    ssize_t n, nwritten = 0;

    /* Number of entries in the PEL. */
    if ((n = rdbSaveLen(rdb,raxSize(pel))) == -1) return -1;
    nwritten += n;

    /* Save each entry. */
    raxIterator ri;
    raxStart(&ri,pel);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        /* We store IDs in raw form as 128 big big endian numbers, like
         * they are inside the radix tree key. */
        if ((n = rdbWriteRaw(rdb,ri.key,sizeof(streamID))) == -1) return -1;
        nwritten += n;

        if (nacks) {
            streamNACK *nack = ri.data;
            if ((n = rdbSaveMillisecondTime(rdb,nack->delivery_time)) == -1)
                return -1;
            nwritten += n;
            if ((n = rdbSaveLen(rdb,nack->delivery_count)) == -1) return -1;
            nwritten += n;
            /* We don't save the consumer name: we'll save the pending IDs
             * for each consumer in the consumer PEL, and resolve the consumer
             * at loading time. */
        }
    }
    raxStop(&ri);
    return nwritten;
}

/* Serialize the consumers of a stream consumer group into the RDB. Helper
 * function for the stream data type serialization. What we do here is to
 * persist the consumer metadata, and it's PEL, for each consumer. */
size_t rdbSaveStreamConsumers(rio *rdb, streamCG *cg) {
    ssize_t n, nwritten = 0;

    /* Number of consumers in this consumer group. */
    if ((n = rdbSaveLen(rdb,raxSize(cg->consumers))) == -1) return -1;
    nwritten += n;

    /* Save each consumer. */
    raxIterator ri;
    raxStart(&ri,cg->consumers);
    raxSeek(&ri,"^",NULL,0);
    while(raxNext(&ri)) {
        streamConsumer *consumer = ri.data;

        /* Consumer name. */
        if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) return -1;
        nwritten += n;

        /* Last seen time. */
        if ((n = rdbSaveMillisecondTime(rdb,consumer->seen_time)) == -1)
            return -1;
        nwritten += n;

        /* Consumer PEL, without the ACKs (see last parameter of the function
         * passed with value of 0), at loading time we'll lookup the ID
         * in the consumer group global PEL and will put a reference in the
         * consumer local PEL. */
        if ((n = rdbSaveStreamPEL(rdb,consumer->pel,0)) == -1)
            return -1;
        nwritten += n;
    }
    raxStop(&ri);
    return nwritten;
}

/* Save a Redis object.
 * Returns -1 on error, number of bytes written on success.
 * 
 * 将给定对象 o 保存到 rdb 中。
 * 保存成功返回 rdb 保存该对象所需的字节数 ，失败返回 0 。
 * 
 * 这个函数比较核心
 * 
 *  */
ssize_t rdbSaveObject(rio *rdb, robj *o, robj *key) {
    ssize_t n = 0, nwritten = 0;

    // 保存字符串对象
    if (o->type == OBJ_STRING) {
        /* Save a string value */
        if ((n = rdbSaveStringObject(rdb,o)) == -1) return -1;
        nwritten += n;
    // 保存列表对象    
    } else if (o->type == OBJ_LIST) {
        /* Save a list value */
        if (o->encoding == OBJ_ENCODING_QUICKLIST) {
            quicklist *ql = o->ptr;
            quicklistNode *node = ql->head;

            if ((n = rdbSaveLen(rdb,ql->len)) == -1) return -1;
            nwritten += n;
            
            // 保存quicklist的节点对象
            while(node) {
                // 判断节点是否进行压缩过，分别保存节点数据
                if (quicklistNodeIsCompressed(node)) {
                    void *data;
                    // 获取压缩后字符串的长度
                    size_t compress_len = quicklistGetLzf(node, &data);
                    // 将压缩后的节点进行保存 
                    if ((n = rdbSaveLzfBlob(rdb,data,compress_len,node->sz)) == -1) return -1;
                    nwritten += n;
                } else {
                    // 以字符串对象的形式保存整个 ZIPLIST 列表
                    if ((n = rdbSaveRawString(rdb,node->zl,node->sz)) == -1) return -1;
                    nwritten += n;
                }
                node = node->next;
            }
        } else {
            serverPanic("Unknown list encoding");
        }
    // 保存集合对象    
    } else if (o->type == OBJ_SET) {
        /* Save a set value */
        if (o->encoding == OBJ_ENCODING_HT) {
            dict *set = o->ptr;
            dictIterator *di = dictGetIterator(set);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize(set))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;
            // 遍历集合成员
            while((de = dictNext(di)) != NULL) {
                sds ele = dictGetKey(de);
                // 以字符串对象的方式保存成员
                if ((n = rdbSaveRawString(rdb,(unsigned char*)ele,sdslen(ele)))
                    == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else if (o->encoding == OBJ_ENCODING_INTSET) {
            size_t l = intsetBlobLen((intset*)o->ptr);
            // 以字符串对象的方式保存整个 INTSET 集合    
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else {
            serverPanic("Unknown set encoding");
        }
    // 保存有序集对象    
    } else if (o->type == OBJ_ZSET) {
        /* Save a sorted set value */
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);
            // 以字符串对象的形式保存整个 ZIPLIST 有序集
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;
        } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = o->ptr;
            zskiplist *zsl = zs->zsl;

            if ((n = rdbSaveLen(rdb,zsl->length)) == -1) return -1;
            nwritten += n;

            /* We save the skiplist elements from the greatest to the smallest
             * (that's trivial since the elements are already ordered in the
             * skiplist): this improves the load process, since the next loaded
             * element will always be the smaller, so adding to the skiplist
             * will always immediately stop at the head, making the insertion
             * O(1) instead of O(log(N)). */
            // 遍历有序集
            zskiplistNode *zn = zsl->tail;
            while (zn != NULL) {
                // 以字符串对象的形式保存集合成员
                if ((n = rdbSaveRawString(rdb,
                    (unsigned char*)zn->ele,sdslen(zn->ele))) == -1)
                {
                    return -1;
                }
                nwritten += n;
                // 成员分值（一个双精度浮点数）会被转换成字符串
                // 然后保存到 rdb 中
                if ((n = rdbSaveBinaryDoubleValue(rdb,zn->score)) == -1)
                    return -1;
                nwritten += n;
                zn = zn->backward;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    // 保存哈希表    
    } else if (o->type == OBJ_HASH) {
        /* Save a hash value */
        if (o->encoding == OBJ_ENCODING_ZIPLIST) {
            size_t l = ziplistBlobLen((unsigned char*)o->ptr);
            // 以字符串对象的形式保存整个 ZIPLIST 哈希表
            if ((n = rdbSaveRawString(rdb,o->ptr,l)) == -1) return -1;
            nwritten += n;

        } else if (o->encoding == OBJ_ENCODING_HT) {
            dictIterator *di = dictGetIterator(o->ptr);
            dictEntry *de;

            if ((n = rdbSaveLen(rdb,dictSize((dict*)o->ptr))) == -1) {
                dictReleaseIterator(di);
                return -1;
            }
            nwritten += n;

            // 迭代字典
            while((de = dictNext(di)) != NULL) {
                sds field = dictGetKey(de);
                sds value = dictGetVal(de);
                // 键和值都以字符串对象的形式来保存
                if ((n = rdbSaveRawString(rdb,(unsigned char*)field,
                        sdslen(field))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
                if ((n = rdbSaveRawString(rdb,(unsigned char*)value,
                        sdslen(value))) == -1)
                {
                    dictReleaseIterator(di);
                    return -1;
                }
                nwritten += n;
            }
            dictReleaseIterator(di);
        } else {
            serverPanic("Unknown hash encoding");
        }
    // 保存stream对象    
    } else if (o->type == OBJ_STREAM) {
        /* Store how many listpacks we have inside the radix tree. */
        stream *s = o->ptr;
        rax *rax = s->rax;
        if ((n = rdbSaveLen(rdb,raxSize(rax))) == -1) return -1;
        nwritten += n;

        /* Serialize all the listpacks inside the radix tree as they are,
         * when loading back, we'll use the first entry of each listpack
         * to insert it back into the radix tree. */
        raxIterator ri;
        raxStart(&ri,rax);
        raxSeek(&ri,"^",NULL,0);
        while (raxNext(&ri)) {
            unsigned char *lp = ri.data;
            size_t lp_bytes = lpBytes(lp);
            if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1) return -1;
            nwritten += n;
            if ((n = rdbSaveRawString(rdb,lp,lp_bytes)) == -1) return -1;
            nwritten += n;
        }
        raxStop(&ri);

        /* Save the number of elements inside the stream. We cannot obtain
         * this easily later, since our macro nodes should be checked for
         * number of items: not a great CPU / space tradeoff. */
        if ((n = rdbSaveLen(rdb,s->length)) == -1) return -1;
        nwritten += n;
        /* Save the last entry ID. */
        if ((n = rdbSaveLen(rdb,s->last_id.ms)) == -1) return -1;
        nwritten += n;
        if ((n = rdbSaveLen(rdb,s->last_id.seq)) == -1) return -1;
        nwritten += n;

        /* The consumer groups and their clients are part of the stream
         * type, so serialize every consumer group. */

        /* Save the number of groups. */
        size_t num_cgroups = s->cgroups ? raxSize(s->cgroups) : 0;
        if ((n = rdbSaveLen(rdb,num_cgroups)) == -1) return -1;
        nwritten += n;

        if (num_cgroups) {
            /* Serialize each consumer group. */
            raxStart(&ri,s->cgroups);
            raxSeek(&ri,"^",NULL,0);
            while(raxNext(&ri)) {
                streamCG *cg = ri.data;

                /* Save the group name. */
                if ((n = rdbSaveRawString(rdb,ri.key,ri.key_len)) == -1)
                    return -1;
                nwritten += n;

                /* Last ID. */
                if ((n = rdbSaveLen(rdb,cg->last_id.ms)) == -1) return -1;
                nwritten += n;
                if ((n = rdbSaveLen(rdb,cg->last_id.seq)) == -1) return -1;
                nwritten += n;

                /* Save the global PEL. */
                if ((n = rdbSaveStreamPEL(rdb,cg->pel,1)) == -1) return -1;
                nwritten += n;

                /* Save the consumers of this group. */
                if ((n = rdbSaveStreamConsumers(rdb,cg)) == -1) return -1;
                nwritten += n;
            }
            raxStop(&ri);
        }
    // 保存module类型对象    
    } else if (o->type == OBJ_MODULE) {
        /* Save a module-specific value. */
        RedisModuleIO io;
        moduleValue *mv = o->ptr;
        moduleType *mt = mv->type;
        moduleInitIOContext(io,mt,rdb,key);

        /* Write the "module" identifier as prefix, so that we'll be able
         * to call the right module during loading. */
        int retval = rdbSaveLen(rdb,mt->id);
        if (retval == -1) return -1;
        io.bytes += retval;

        /* Then write the module-specific representation + EOF marker. */
        mt->rdb_save(&io,mv->value);
        retval = rdbSaveLen(rdb,RDB_MODULE_OPCODE_EOF);
        if (retval == -1) return -1;
        io.bytes += retval;

        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }
        return io.error ? -1 : (ssize_t)io.bytes;
    } else {
        serverPanic("Unknown object type");
    }
    return nwritten;
}

/* Return the length the object will have on disk if saved with
 * the rdbSaveObject() function. Currently we use a trick to get
 * this length with very little changes to the code. In the future
 * we could switch to a faster solution. */
// 这个没有保存在rdb上，返回应该保存在rdb的对象长度
size_t rdbSavedObjectLen(robj *o) {
    ssize_t len = rdbSaveObject(NULL,o,NULL);
    serverAssertWithInfo(NULL,o,len != -1);
    return len;
}

/* Save a key-value pair, with expire time, type, key, value.
 * On error -1 is returned.
 * On success if the key was actually saved 1 is returned, otherwise 0
 * is returned (the key was already expired).
 * 将键值对的键、值、过期时间和类型写入到 RDB 中。
 *  出错返回 -1 。
 *  成功保存返回 1 ，当键已经过期时，返回 0 。
 *  */
int rdbSaveKeyValuePair(rio *rdb, robj *key, robj *val, long long expiretime) {

    // 获取过期策略
    int savelru = server.maxmemory_policy & MAXMEMORY_FLAG_LRU;
    int savelfu = server.maxmemory_policy & MAXMEMORY_FLAG_LFU;

    /* Save the expire time */
    // 保存过期时间
    if (expiretime != -1) {
        if (rdbSaveType(rdb,RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;
    }

    /* Save the LRU info. */
    // 保存LRU过期信息
    if (savelru) {
        // 计算还有多长时间过期
        uint64_t idletime = estimateObjectIdleTime(val);
        idletime /= 1000; /* Using seconds is enough and requires less space.*/
        if (rdbSaveType(rdb,RDB_OPCODE_IDLE) == -1) return -1;
        if (rdbSaveLen(rdb,idletime) == -1) return -1;
    }

    /* Save the LFU info. */
    // 保存LFU过期信息
    if (savelfu) {
        uint8_t buf[1];
        buf[0] = LFUDecrAndReturn(val);
        /* We can encode this in exactly two bytes: the opcode and an 8
         * bit counter, since the frequency is logarithmic with a 0-255 range.
         * Note that we do not store the halving time because to reset it
         * a single time when loading does not affect the frequency much. */
        if (rdbSaveType(rdb,RDB_OPCODE_FREQ) == -1) return -1;
        if (rdbWriteRaw(rdb,buf,1) == -1) return -1;
    }

    /* Save type, key, value */
    // 保存类型，键，值
    if (rdbSaveObjectType(rdb,val) == -1) return -1;
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    if (rdbSaveObject(rdb,val,key) == -1) return -1;
    return 1;
}

/* Save an AUX(辅助))) field. */
ssize_t rdbSaveAuxField(rio *rdb, void *key, size_t keylen, void *val, size_t vallen) {
    ssize_t ret, len = 0;
    if ((ret = rdbSaveType(rdb,RDB_OPCODE_AUX)) == -1) return -1;
    len += ret;
    if ((ret = rdbSaveRawString(rdb,key,keylen)) == -1) return -1;
    len += ret;
    if ((ret = rdbSaveRawString(rdb,val,vallen)) == -1) return -1;
    len += ret;
    return len;
}

/* Wrapper for rdbSaveAuxField() used when key/val length can be obtained
 * with strlen(). */
ssize_t rdbSaveAuxFieldStrStr(rio *rdb, char *key, char *val) {
    return rdbSaveAuxField(rdb,key,strlen(key),val,strlen(val));
}

/* Wrapper for strlen(key) + integer type (up to long long range). */
ssize_t rdbSaveAuxFieldStrInt(rio *rdb, char *key, long long val) {
    char buf[LONG_STR_SIZE];
    int vlen = ll2string(buf,sizeof(buf),val);
    return rdbSaveAuxField(rdb,key,strlen(key),buf,vlen);
}

/* Save a few default AUX fields with information about the RDB generated. */
int rdbSaveInfoAuxFields(rio *rdb, int flags, rdbSaveInfo *rsi) {
    int redis_bits = (sizeof(void*) == 8) ? 64 : 32;
    int aof_preamble = (flags & RDB_SAVE_AOF_PREAMBLE) != 0;

    /* Add a few fields about the state when the RDB was created. */
    if (rdbSaveAuxFieldStrStr(rdb,"redis-ver",REDIS_VERSION) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"redis-bits",redis_bits) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"ctime",time(NULL)) == -1) return -1;
    if (rdbSaveAuxFieldStrInt(rdb,"used-mem",zmalloc_used_memory()) == -1) return -1;

    /* Handle saving options that generate aux fields. */
    if (rsi) {
        if (rdbSaveAuxFieldStrInt(rdb,"repl-stream-db",rsi->repl_stream_db)
            == -1) return -1;
        if (rdbSaveAuxFieldStrStr(rdb,"repl-id",server.replid)
            == -1) return -1;
        if (rdbSaveAuxFieldStrInt(rdb,"repl-offset",server.master_repl_offset)
            == -1) return -1;
    }
    if (rdbSaveAuxFieldStrInt(rdb,"aof-preamble",aof_preamble) == -1) return -1;
    return 1;
}

/* Produces a dump of the database in RDB format sending it to the specified
 * Redis I/O channel. On success C_OK is returned, otherwise C_ERR
 * is returned and part of the output, or all the output, can be
 * missing because of I/O errors.
 * 
 * 生成RDB格式数据库文件通过 Redis IO 通过 保存到磁盘中 
 * 
 * 成功返回 C_OK 
 * 
 * RDB 文件结构
 *  REDIS db_version databases EOF check_sum
 * 
 * When the function returns C_ERR and if 'error' is not NULL, the
 * integer pointed by 'error' is set to the value of errno just after the I/O
 * error. */
// 这个函数是真正写入RDB函数 
int rdbSaveRio(rio *rdb, int *error, int flags, rdbSaveInfo *rsi) {
    //  字典迭代器
    dictIterator *di = NULL;
    // 字典的一个实体信息
    dictEntry *de;
    // 魔法数
    char magic[10];
    int j;
    // 校验和
    uint64_t cksum;
    size_t processed = 0;

    // 更新生产校验和的函数
    if (server.rdb_checksum)
        rdb->update_cksum = rioGenericUpdateChecksum; 
     //REDIS0009 (9个字节) 即REDIS db_version    
    snprintf(magic,sizeof(magic),"REDIS%04d",RDB_VERSION);
    // 将魔幻数(REDIS db_version) 以字符串方式写入文件
    if (rdbWriteRaw(rdb,magic,9) == -1) goto werr;
    // 写入其他信息
    if (rdbSaveInfoAuxFields(rdb,flags,rsi) == -1) goto werr;

    // 遍历每个数据 
    for (j = 0; j < server.dbnum; j++) {
        // 指向数据库
        redisDb *db = server.db+j;
        // 数据库字典
        dict *d = db->dict;
        // 跳空数据库
        if (dictSize(d) == 0) continue;
        // 迭代器
        di = dictGetSafeIterator(d);

        /* Write the SELECT DB opcode */
        // 选择的db标志 保存SELECTDB常量和dbid
        if (rdbSaveType(rdb,RDB_OPCODE_SELECTDB) == -1) goto werr;
        // 第几个db
        if (rdbSaveLen(rdb,j) == -1) goto werr;

        /* Write the RESIZE DB opcode. We trim the size to UINT32_MAX, which
         * is currently the largest type we are able to represent in RDB sizes.
         * However this does not limit the actual size of the DB to load since
         * these sizes are just hints to resize the hash tables. */
        uint64_t db_size, expires_size;
        db_size = dictSize(db->dict);
        expires_size = dictSize(db->expires);
        // 保存RESIZEDB常量，数据库size和设置expire的key-value的size
        if (rdbSaveType(rdb,RDB_OPCODE_RESIZEDB) == -1) goto werr;
        if (rdbSaveLen(rdb,db_size) == -1) goto werr;
        if (rdbSaveLen(rdb,expires_size) == -1) goto werr;

        /* Iterate this DB writing every entry */
        // 遍历db->dict,将数据中key-value保存到rdb文件中
        while((de = dictNext(di)) != NULL) {
            // 获取key,value 对象
            sds keystr = dictGetKey(de);
            robj key, *o = dictGetVal(de);
            long long expire;

            initStaticStringObject(key,keystr);
            // 获取key的expire时间
            expire = getExpire(db,&key);
            // 保存key-value和expire
            if (rdbSaveKeyValuePair(rdb,&key,o,expire) == -1) goto werr;

            /* When this RDB is produced as part of an AOF rewrite, move
             * accumulated diff from parent to child while rewriting in
             * order to have a smaller final write. */
            // aof重写
            if (flags & RDB_SAVE_AOF_PREAMBLE &&
                rdb->processed_bytes > processed+AOF_READ_DIFF_INTERVAL_BYTES)
            {
                processed = rdb->processed_bytes;
                aofReadDiffFromParent();
            }
        }
        dictReleaseIterator(di);
        di = NULL; /* So that we don't release it again on error. */
    }

    /* If we are storing the replication information on disk, persist
     * the script cache as well: on successful PSYNC after a restart, we need
     * to be able to process any EVALSHA inside the replication backlog the
     * master will send us. */
    // 保存lua脚本的信息
    if (rsi && dictSize(server.lua_scripts)) {
        di = dictGetIterator(server.lua_scripts);
        while((de = dictNext(di)) != NULL) {
            robj *body = dictGetVal(de);
            if (rdbSaveAuxField(rdb,"lua",3,body->ptr,sdslen(body->ptr)) == -1)
                goto werr;
        }
        dictReleaseIterator(di);
        di = NULL; /* So that we don't release it again on error. */
    }

    /* EOF opcode */
    // 写入RDB文件的结束标志
    if (rdbSaveType(rdb,RDB_OPCODE_EOF) == -1) goto werr;

    /* CRC64 checksum. It will be zero if checksum computation is disabled, the
     * loading code skips the check in this case. */
    // 写入校验和
    cksum = rdb->cksum;
    memrev64ifbe(&cksum);
    if (rioWrite(rdb,&cksum,8) == 0) goto werr;
    return C_OK;

werr:
    if (error) *error = errno;
    if (di) dictReleaseIterator(di);
    return C_ERR;
}

/* This is just a wrapper to rdbSaveRio() that additionally adds a prefix
 * and a suffix to the generated RDB dump. The prefix is:
 *
 * $EOF:<40 bytes unguessable hex string>\r\n
 *
 * While the suffix is the 40 bytes hex string we announced in the prefix.
 * This way processes receiving the payload can understand when it ends
 * without doing any processing of the content. 
 * 标志着数据库内容的结尾 EOF
 * */
int rdbSaveRioWithEOFMark(rio *rdb, int *error, rdbSaveInfo *rsi) {
    char eofmark[RDB_EOF_MARK_SIZE];

    getRandomHexChars(eofmark,RDB_EOF_MARK_SIZE);
    if (error) *error = 0;
    if (rioWrite(rdb,"$EOF:",5) == 0) goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    if (rioWrite(rdb,"\r\n",2) == 0) goto werr;
    if (rdbSaveRio(rdb,error,RDB_SAVE_NONE,rsi) == C_ERR) goto werr;
    if (rioWrite(rdb,eofmark,RDB_EOF_MARK_SIZE) == 0) goto werr;
    return C_OK;

werr: /* Write error. */
    /* Set 'error' only if not already set by rdbSaveRio() call. */
    if (error && *error == 0) *error = errno;
    return C_ERR;
}

/* Save the DB on disk. Return C_ERR on error, C_OK on success. */
// 生成rdb文件持久化到磁盘 成功C_OK,失败返回C_ERR 核心函数入口
int rdbSave(char *filename, rdbSaveInfo *rsi) {
    char tmpfile[256];
    // 存储当前错误信息
    char cwd[MAXPATHLEN]; /* Current working dir path for error messages. */
    FILE *fp;
    rio rdb;
    int error = 0;

    // 创建临时文件 temp-{pid}.rdb文件
    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());
    fp = fopen(tmpfile,"w");
    if (!fp) {
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Failed opening the RDB file %s (in server root dir %s) "
            "for saving: %s",
            filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
        return C_ERR;
    }

    // 初始化rio(Redis IO)
    rioInitWithFile(&rdb,fp);

    // 设置rio中的autosync属性(autosync定义了多少字节进行一次fsync操作)
    if (server.rdb_save_incremental_fsync)
        rioSetAutoSync(&rdb,REDIS_AUTOSYNC_BYTES);

    // 调用rdbSaveRIO将数据写入到文件中
    if (rdbSaveRio(&rdb,&error,RDB_SAVE_NONE,rsi) == C_ERR) {
        errno = error;
        goto werr;
    }

    /* Make sure data will not remain on the OS's output buffers */
    // 保证缓存里面的数据全部写入到fp中
    if (fflush(fp) == EOF) goto werr;
    if (fsync(fileno(fp)) == -1) goto werr;
    if (fclose(fp) == EOF) goto werr;

    /* Use RENAME to make sure the DB file is changed atomically only
     * if the generate DB file is ok. */
    // 重命名rdb文件
    if (rename(tmpfile,filename) == -1) {
        char *cwdp = getcwd(cwd,MAXPATHLEN);
        serverLog(LL_WARNING,
            "Error moving temp DB file %s on the final "
            "destination %s (in server root dir %s): %s",
            tmpfile,
            filename,
            cwdp ? cwdp : "unknown",
            strerror(errno));
        unlink(tmpfile);
        return C_ERR;
    }

    // 设置server属性
    serverLog(LL_NOTICE,"DB saved on disk");
    // 将数据库脏状态清零
    server.dirty = 0;
    // 记录最后一次save的时间
    server.lastsave = time(NULL);
    // 记录最后一次 SAVE的状态
    server.lastbgsave_status = C_OK;
    return C_OK;

werr:
    serverLog(LL_WARNING,"Write error saving DB on disk: %s", strerror(errno));
    // 关闭文件
    fclose(fp);
    // 删除文件
    unlink(tmpfile);
    return C_ERR;
}

// 执行bgsave命令 后台保存rdb文件
int rdbSaveBackground(char *filename, rdbSaveInfo *rsi) {
    pid_t childpid;
    long long start;

    // 如果BGSAVE已经在执行，出错
    if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) return C_ERR;

    // 记录BGSAVE执行前的数据库被修改次数
    server.dirty_before_bgsave = server.dirty;
    // 记录bgsave时间
    server.lastbgsave_try = time(NULL);

    // ? 这个应该是打开父子进程的通信管多pipe line
    openChildInfoPipe();

    start = ustime();
    // 创建子进程 进行rdb文件
    if ((childpid = fork()) == 0) {
        int retval;

        /* Child */
        // 关闭网连接fd ？ 为什么子进程要关闭网络连接
        closeListeningSockets(0);
        // 设置进程的标题，方便识别
        redisSetProcTitle("redis-rdb-bgsave");
        // 执行rdb保存操作
        retval = rdbSave(filename,rsi);

        // 打印 copy-on-write 时 使用的内存数
        if (retval == C_OK) {
            size_t private_dirty = zmalloc_get_private_dirty(-1);

            if (private_dirty) {
                serverLog(LL_NOTICE,
                    "RDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }

            server.child_info_data.cow_size = private_dirty;
            sendChildInfo(CHILD_INFO_TYPE_RDB);
        }
        // 向父进程发送信号
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent */
        // 计算fork()执行的时间
        server.stat_fork_time = ustime()-start;
        server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
    
        latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);
        if (childpid == -1) {
            closeChildInfoPipe();
            server.lastbgsave_status = C_ERR;
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));
            return C_ERR;
        }
        serverLog(LL_NOTICE,"Background saving started by pid %d",childpid);
        // 记录 save 开始时间
        server.rdb_save_time_start = time(NULL);
        // 记录执行bgsave的子进程id
        server.rdb_child_pid = childpid;
        // 计算 bgsave 的类型
        server.rdb_child_type = RDB_CHILD_TYPE_DISK;
        // 更新 dict rehash策略
        updateDictResizePolicy();
        return C_OK;
    }
    return C_OK; /* unreached */
}

// 删除rdb的临时文件
// BGSAVE 执行被中断是使用
void rdbRemoveTempFile(pid_t childpid) {
    char tmpfile[256];

    snprintf(tmpfile,sizeof(tmpfile),"temp-%d.rdb", (int) childpid);
    unlink(tmpfile);
}

/* This function is called by rdbLoadObject() when the code is in RDB-check
 * mode and we find a module value of type 2 that can be parsed without
 * the need of the actual module. The value is parsed for errors, finally
 * a dummy redis object is returned just to conform to the API. */
robj *rdbLoadCheckModuleValue(rio *rdb, char *modulename) {
    uint64_t opcode;
    while((opcode = rdbLoadLen(rdb,NULL)) != RDB_MODULE_OPCODE_EOF) {
        if (opcode == RDB_MODULE_OPCODE_SINT ||
            opcode == RDB_MODULE_OPCODE_UINT)
        {
            uint64_t len;
            if (rdbLoadLenByRef(rdb,NULL,&len) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading integer from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_STRING) {
            robj *o = rdbGenericLoadStringObject(rdb,RDB_LOAD_NONE,NULL);
            if (o == NULL) {
                rdbExitReportCorruptRDB(
                    "Error reading string from module %s value", modulename);
            }
            decrRefCount(o);
        } else if (opcode == RDB_MODULE_OPCODE_FLOAT) {
            float val;
            if (rdbLoadBinaryFloatValue(rdb,&val) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading float from module %s value", modulename);
            }
        } else if (opcode == RDB_MODULE_OPCODE_DOUBLE) {
            double val;
            if (rdbLoadBinaryDoubleValue(rdb,&val) == -1) {
                rdbExitReportCorruptRDB(
                    "Error reading double from module %s value", modulename);
            }
        }
    }
    return createStringObject("module-dummy-value",18);
}

/* Load a Redis object of the specified type from the specified file.
 * On success a newly allocated object is returned, otherwise NULL.
 * 
 * 从rdb文件中载入指定类型的对象
 * 
 * 读入成功返回一个新对象，失败返回NULL
 *  */

robj *rdbLoadObject(int rdbtype, rio *rdb, robj *key) {
    robj *o = NULL, *ele, *dec;
    uint64_t len;
    unsigned int i;

    // 字符串对象
    if (rdbtype == RDB_TYPE_STRING) {
        /* Read string value */
        if ((o = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
        o = tryObjectEncoding(o);

    //  列表对象   
    } else if (rdbtype == RDB_TYPE_LIST) {
        // 读入列表的节点数
        /* Read list value */
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;

        // 创建一个新的quicklist对象，并设置响应的属性
        o = createQuicklistObject();
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);

        /* Load every single element of the list */
        // 载入所有的列表项
        while(len--) {
            if ((ele = rdbLoadEncodedStringObject(rdb)) == NULL) return NULL;
            dec = getDecodedObject(ele);
            size_t len = sdslen(dec->ptr);
            quicklistPushTail(o->ptr, dec->ptr, len);
            decrRefCount(dec);
            decrRefCount(ele);
        }

    // 载入集合对象    
    } else if (rdbtype == RDB_TYPE_SET) {
        /* Read Set value */
        // 载入集合的元素的数量
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;

        /* Use a regular set when there are too many entries. */
        if (len > server.set_max_intset_entries) {
            // hashSet 哈希表集合
            o = createSetObject();
            /* It's faster to expand the dict to the right size asap in order
             * to avoid rehashing */
            if (len > DICT_HT_INITIAL_SIZE)
                dictExpand(o->ptr,len);
        } else {
            // 整数集合
            o = createIntsetObject();
        }

        /* Load every single element of the set */
        // 载入所有的集合元素
        for (i = 0; i < len; i++) {
            long long llval;
            sds sdsele;

            // 载入元素
            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;

            if (o->encoding == OBJ_ENCODING_INTSET) {
                // intset 集合
                /* Fetch integer value from element. */
                if (isSdsRepresentableAsLongLong(sdsele,&llval) == C_OK) {
                    o->ptr = intsetAdd(o->ptr,llval,NULL);
                } else {
                    setTypeConvert(o,OBJ_ENCODING_HT);
                    dictExpand(o->ptr,len);
                }
            }

            /* This will also be called when the set was just converted
             * to a regular hash table encoded set. */
            if (o->encoding == OBJ_ENCODING_HT) {
                // HT 编码集合
                dictAdd((dict*)o->ptr,sdsele,NULL);
            } else {
                sdsfree(sdsele);
            }
        }
    // 载入有序集合    
    } else if (rdbtype == RDB_TYPE_ZSET_2 || rdbtype == RDB_TYPE_ZSET) {
        /* Read list/set value. */
        uint64_t zsetlen;
        size_t maxelelen = 0;
        zset *zs;

        // 载入有序集合的元素数量
        if ((zsetlen = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        // 创建一个有序集合对象
        o = createZsetObject();
        zs = o->ptr;

        if (zsetlen > DICT_HT_INITIAL_SIZE)
            dictExpand(zs->dict,zsetlen);

        /* Load every single element of the sorted set. */
        // 载入每个元素
        while(zsetlen--) {
            sds sdsele;
            double score;
            zskiplistNode *znode;

            if ((sdsele = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;

            if (rdbtype == RDB_TYPE_ZSET_2) {
                if (rdbLoadBinaryDoubleValue(rdb,&score) == -1) return NULL;
            } else {
                if (rdbLoadDoubleValue(rdb,&score) == -1) return NULL;
            }

            /* Don't care about integer-encoded strings. */
            if (sdslen(sdsele) > maxelelen) maxelelen = sdslen(sdsele);

            znode = zslInsert(zs->zsl,score,sdsele);
            dictAdd(zs->dict,sdsele,&znode->score);
        }

        /* Convert *after* loading, since sorted sets are not stored ordered. */
        // 如果有序集合符合条件的话，将它转换为ZIPLIST编码
        if (zsetLength(o) <= server.zset_max_ziplist_entries &&
            maxelelen <= server.zset_max_ziplist_value)
                zsetConvert(o,OBJ_ENCODING_ZIPLIST);


    // 载入哈希对象
    } else if (rdbtype == RDB_TYPE_HASH) {
        uint64_t len;
        int ret;
        sds field, value;

        // 加入哈希表节点数
        len = rdbLoadLen(rdb, NULL);
        if (len == RDB_LENERR) return NULL;

        // 创建哈希表
        o = createHashObject();

        /* Too many entries? Use a hash table.
         * 根据节点数据量 选择使用 ZIPLIST编码还是HT编码
         */
        if (len > server.hash_max_ziplist_entries)
            hashTypeConvert(o, OBJ_ENCODING_HT);

        /* Load every field and value into the ziplist 
         * 
         * 将每个元素载入 ZIPLIST编码   
         */
        while (o->encoding == OBJ_ENCODING_ZIPLIST && len > 0) {
            len--;
            /* Load raw strings */
            // 载入key value
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;

            /* Add pair to ziplist */
            // 将key value 保存在ziplist末尾
            o->ptr = ziplistPush(o->ptr, (unsigned char*)field,
                    sdslen(field), ZIPLIST_TAIL);
            o->ptr = ziplistPush(o->ptr, (unsigned char*)value,
                    sdslen(value), ZIPLIST_TAIL);

            /* Convert to hash table if size threshold is exceeded */
            // 如果元素过的，将编码转换成 HT 
            if (sdslen(field) > server.hash_max_ziplist_value ||
                sdslen(value) > server.hash_max_ziplist_value)
            {
                sdsfree(field);
                sdsfree(value);
                hashTypeConvert(o, OBJ_ENCODING_HT);
                break;
            }
            sdsfree(field);
            sdsfree(value);
        }

        if (o->encoding == OBJ_ENCODING_HT && len > DICT_HT_INITIAL_SIZE)
            dictExpand(o->ptr,len);

        /* Load remaining fields and values into the hash table
         * 载入每个元素 HT 编码
         */
        while (o->encoding == OBJ_ENCODING_HT && len > 0) {
            len--;
            /* Load encoded strings */
            // 获取 key value
            if ((field = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;
            if ((value = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL))
                == NULL) return NULL;

            /* Add pair to hash table */
            // 添加到哈希表中
            ret = dictAdd((dict*)o->ptr, field, value);
            if (ret == DICT_ERR) {
                rdbExitReportCorruptRDB("Duplicate keys detected");
            }
        }

        /* All pairs should be read by now */
        serverAssert(len == 0);

    // quicklist对象    
    } else if (rdbtype == RDB_TYPE_LIST_QUICKLIST) {
        if ((len = rdbLoadLen(rdb,NULL)) == RDB_LENERR) return NULL;
        o = createQuicklistObject();
        quicklistSetOptions(o->ptr, server.list_max_ziplist_size,
                            server.list_compress_depth);

        while (len--) {
            unsigned char *zl =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
            if (zl == NULL) return NULL;
            quicklistAppendZiplist(o->ptr, zl);
        }

    // 载入下面RDB对象     
    } else if (rdbtype == RDB_TYPE_HASH_ZIPMAP  ||
               rdbtype == RDB_TYPE_LIST_ZIPLIST ||
               rdbtype == RDB_TYPE_SET_INTSET   ||
               rdbtype == RDB_TYPE_ZSET_ZIPLIST ||
               rdbtype == RDB_TYPE_HASH_ZIPLIST)
    {
        unsigned char *encoded =
            rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
        if (encoded == NULL) return NULL;
        o = createObject(OBJ_STRING,encoded); /* Obj type fixed below. */

        /* Fix the object encoding, and make sure to convert the encoded
         * data type into the base type if accordingly to the current
         * configuration there are too many elements in the encoded data
         * type. Note that we only check the length and not max element
         * size as this is an O(N) scan. Eventually everything will get
         * converted. 
         * 
         * 根据读取的类型，将值恢复成原来的编码对象。
         *
         * 在创建编码对象的过程中，程序会检查对象的元素长度，
         * 如果长度超过指定值的话，就会将内存编码对象转换成普通数据结构对象。
         * 
         * */
        switch(rdbtype) {
            // ZIPMAP 编码的哈希表
            case RDB_TYPE_HASH_ZIPMAP:
                /* Convert to ziplist encoded hash. This must be deprecated
                 * when loading dumps created by Redis 2.4 gets deprecated. */
                {
                    unsigned char *zl = ziplistNew();
                    unsigned char *zi = zipmapRewind(o->ptr);
                    unsigned char *fstr, *vstr;
                    unsigned int flen, vlen;
                    unsigned int maxlen = 0;

                    while ((zi = zipmapNext(zi, &fstr, &flen, &vstr, &vlen)) != NULL) {
                        if (flen > maxlen) maxlen = flen;
                        if (vlen > maxlen) maxlen = vlen;
                        zl = ziplistPush(zl, fstr, flen, ZIPLIST_TAIL);
                        zl = ziplistPush(zl, vstr, vlen, ZIPLIST_TAIL);
                    }

                    zfree(o->ptr);
                    o->ptr = zl;
                    o->type = OBJ_HASH;
                    o->encoding = OBJ_ENCODING_ZIPLIST;

                    if (hashTypeLength(o) > server.hash_max_ziplist_entries ||
                        maxlen > server.hash_max_ziplist_value)
                    {
                        hashTypeConvert(o, OBJ_ENCODING_HT);
                    }
                }
                break;

            // ZIPLIST 编码的列表    
            case RDB_TYPE_LIST_ZIPLIST:
                o->type = OBJ_LIST;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                listTypeConvert(o,OBJ_ENCODING_QUICKLIST);
                break;
            // INTSET 编码集合    
            case RDB_TYPE_SET_INTSET:
                o->type = OBJ_SET;
                o->encoding = OBJ_ENCODING_INTSET;
                if (intsetLen(o->ptr) > server.set_max_intset_entries)
                    setTypeConvert(o,OBJ_ENCODING_HT);
                break;
            // ZIPLIST 编码有序集合    
            case RDB_TYPE_ZSET_ZIPLIST:
                o->type = OBJ_ZSET;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (zsetLength(o) > server.zset_max_ziplist_entries)
                    zsetConvert(o,OBJ_ENCODING_SKIPLIST);
                break;
            // ZIPLIST 编码的 HASH    
            case RDB_TYPE_HASH_ZIPLIST:
                o->type = OBJ_HASH;
                o->encoding = OBJ_ENCODING_ZIPLIST;
                if (hashTypeLength(o) > server.hash_max_ziplist_entries)
                    hashTypeConvert(o, OBJ_ENCODING_HT);
                break;
            default:
                rdbExitReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
                break;
        }

    // 载入stream对象    
    } else if (rdbtype == RDB_TYPE_STREAM_LISTPACKS) {
        o = createStreamObject();
        stream *s = o->ptr;
        uint64_t listpacks = rdbLoadLen(rdb,NULL);

        while(listpacks--) {
            /* Get the master ID, the one we'll use as key of the radix tree
             * node: the entries inside the listpack itself are delta-encoded
             * relatively to this ID. */
            sds nodekey = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (nodekey == NULL) {
                rdbExitReportCorruptRDB("Stream master ID loading failed: invalid encoding or I/O error.");
            }
            if (sdslen(nodekey) != sizeof(streamID)) {
                rdbExitReportCorruptRDB("Stream node key entry is not the "
                                        "size of a stream ID");
            }

            /* Load the listpack. */
            unsigned char *lp =
                rdbGenericLoadStringObject(rdb,RDB_LOAD_PLAIN,NULL);
            if (lp == NULL) return NULL;
            unsigned char *first = lpFirst(lp);
            if (first == NULL) {
                /* Serialized listpacks should never be empty, since on
                 * deletion we should remove the radix tree key if the
                 * resulting listpack is empty. */
                rdbExitReportCorruptRDB("Empty listpack inside stream");
            }

            /* Insert the key in the radix tree. */
            int retval = raxInsert(s->rax,
                (unsigned char*)nodekey,sizeof(streamID),lp,NULL);
            sdsfree(nodekey);
            if (!retval)
                rdbExitReportCorruptRDB("Listpack re-added with existing key");
        }
        /* Load total number of items inside the stream. */
        s->length = rdbLoadLen(rdb,NULL);
        /* Load the last entry ID. */
        s->last_id.ms = rdbLoadLen(rdb,NULL);
        s->last_id.seq = rdbLoadLen(rdb,NULL);

        /* Consumer groups loading */
        size_t cgroups_count = rdbLoadLen(rdb,NULL);
        while(cgroups_count--) {
            /* Get the consumer group name and ID. We can then create the
             * consumer group ASAP and populate its structure as
             * we read more data. */
            streamID cg_id;
            sds cgname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
            if (cgname == NULL) {
                rdbExitReportCorruptRDB(
                    "Error reading the consumer group name from Stream");
            }
            cg_id.ms = rdbLoadLen(rdb,NULL);
            cg_id.seq = rdbLoadLen(rdb,NULL);
            streamCG *cgroup = streamCreateCG(s,cgname,sdslen(cgname),&cg_id);
            if (cgroup == NULL)
                rdbExitReportCorruptRDB("Duplicated consumer group name %s",
                                         cgname);
            sdsfree(cgname);

            /* Load the global PEL for this consumer group, however we'll
             * not yet populate the NACK structures with the message
             * owner, since consumers for this group and their messages will
             * be read as a next step. So for now leave them not resolved
             * and later populate it. */
            size_t pel_size = rdbLoadLen(rdb,NULL);
            while(pel_size--) {
                unsigned char rawid[sizeof(streamID)];
                rdbLoadRaw(rdb,rawid,sizeof(rawid));
                streamNACK *nack = streamCreateNACK(NULL);
                nack->delivery_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);
                nack->delivery_count = rdbLoadLen(rdb,NULL);
                if (!raxInsert(cgroup->pel,rawid,sizeof(rawid),nack,NULL))
                    rdbExitReportCorruptRDB("Duplicated gobal PEL entry "
                                            "loading stream consumer group");
            }

            /* Now that we loaded our global PEL, we need to load the
             * consumers and their local PELs. */
            size_t consumers_num = rdbLoadLen(rdb,NULL);
            while(consumers_num--) {
                sds cname = rdbGenericLoadStringObject(rdb,RDB_LOAD_SDS,NULL);
                if (cname == NULL) {
                    rdbExitReportCorruptRDB(
                        "Error reading the consumer name from Stream group");
                }
                streamConsumer *consumer = streamLookupConsumer(cgroup,cname,
                                           1);
                sdsfree(cname);
                consumer->seen_time = rdbLoadMillisecondTime(rdb,RDB_VERSION);

                /* Load the PEL about entries owned by this specific
                 * consumer. */
                pel_size = rdbLoadLen(rdb,NULL);
                while(pel_size--) {
                    unsigned char rawid[sizeof(streamID)];
                    rdbLoadRaw(rdb,rawid,sizeof(rawid));
                    streamNACK *nack = raxFind(cgroup->pel,rawid,sizeof(rawid));
                    if (nack == raxNotFound)
                        rdbExitReportCorruptRDB("Consumer entry not found in "
                                                "group global PEL");

                    /* Set the NACK consumer, that was left to NULL when
                     * loading the global PEL. Then set the same shared
                     * NACK structure also in the consumer-specific PEL. */
                    nack->consumer = consumer;
                    if (!raxInsert(consumer->pel,rawid,sizeof(rawid),nack,NULL))
                        rdbExitReportCorruptRDB("Duplicated consumer PEL entry "
                                                " loading a stream consumer "
                                                "group");
                }
            }
        }

    // 载入moduel对象    
    } else if (rdbtype == RDB_TYPE_MODULE || rdbtype == RDB_TYPE_MODULE_2) {
        uint64_t moduleid = rdbLoadLen(rdb,NULL);
        moduleType *mt = moduleTypeLookupModuleByID(moduleid);
        char name[10];

        if (rdbCheckMode && rdbtype == RDB_TYPE_MODULE_2) {
            moduleTypeNameByID(name,moduleid);
            return rdbLoadCheckModuleValue(rdb,name);
        }

        if (mt == NULL) {
            moduleTypeNameByID(name,moduleid);
            serverLog(LL_WARNING,"The RDB file contains module data I can't load: no matching module '%s'", name);
            exit(1);
        }
        RedisModuleIO io;
        moduleInitIOContext(io,mt,rdb,key);
        io.ver = (rdbtype == RDB_TYPE_MODULE) ? 1 : 2;
        /* Call the rdb_load method of the module providing the 10 bit
         * encoding version in the lower 10 bits of the module ID. */
        void *ptr = mt->rdb_load(&io,moduleid&1023);
        if (io.ctx) {
            moduleFreeContext(io.ctx);
            zfree(io.ctx);
        }

        /* Module v2 serialization has an EOF mark at the end. */
        if (io.ver == 2) {
            uint64_t eof = rdbLoadLen(rdb,NULL);
            if (eof != RDB_MODULE_OPCODE_EOF) {
                serverLog(LL_WARNING,"The RDB file contains module data for the module '%s' that is not terminated by the proper module value EOF marker", name);
                exit(1);
            }
        }

        if (ptr == NULL) {
            moduleTypeNameByID(name,moduleid);
            serverLog(LL_WARNING,"The RDB file contains module data for the module type '%s', that the responsible module is not able to load. Check for modules log above for additional clues.", name);
            exit(1);
        }
        o = createModuleObject(mt,ptr);
    } else {
        rdbExitReportCorruptRDB("Unknown RDB encoding type %d",rdbtype);
    }
    return o;
}

/* Mark that we are loading in the global state and setup the fields
 * needed to provide loading stats.
 * 
 * 在全局状态中标记程序正在载入rdb文件
 * 
 * 并设置相应的载入状态
 *  */
void startLoading(FILE *fp) {
    struct stat sb;

    /* Load the DB */
    // 正在载入
    server.loading = 1;
    // 开始载入的时间
    server.loading_start_time = time(NULL);
    // 已经载入的字节大小
    server.loading_loaded_bytes = 0;
    if (fstat(fileno(fp), &sb) == -1) {
        server.loading_total_bytes = 0;
    } else {
        server.loading_total_bytes = sb.st_size;
    }
}

/* Refresh the loading progress info 
* 刷新载入进度信息
*/
void loadingProgress(off_t pos) {
    server.loading_loaded_bytes = pos;
    if (server.stat_peak_memory < zmalloc_used_memory())
        server.stat_peak_memory = zmalloc_used_memory();
}

/* Loading finished 
 * 设置完全载入标记
 */
void stopLoading(void) {
    server.loading = 0;
}

/* Track loading progress in order to serve client's from time to time
   and if needed calculate rdb checksum  */
 // 记录载入进度信息，以便让客户端进行查询
 // 这也会在计算RDB校验和时间用到  正在载入的回调
void rdbLoadProgressCallback(rio *r, const void *buf, size_t len) {
    if (server.rdb_checksum)
        rioGenericUpdateChecksum(r, buf, len);
    if (server.loading_process_events_interval_bytes &&
        (r->processed_bytes + len)/server.loading_process_events_interval_bytes > r->processed_bytes/server.loading_process_events_interval_bytes)
    {
        /* The DB can take some non trivial amount of time to load. Update
         * our cached time since it is used to create and update the last
         * interaction time with clients and for other important things. */
        updateCachedTime();
        if (server.masterhost && server.repl_state == REPL_STATE_TRANSFER)
            replicationSendNewlineToMaster();
        loadingProgress(r->processed_bytes);
        processEventsWhileBlocked();
    }
}

/* Load an RDB file from the rio stream 'rdb'. On success C_OK is returned,
 * otherwise C_ERR is returned and 'errno' is set accordingly. 
 * 载入rdb文件
 * */
int rdbLoadRio(rio *rdb, rdbSaveInfo *rsi, int loading_aof) {
    uint64_t dbid;
    int type, rdbver;
    redisDb *db = server.db+0;
    char buf[1024];

    // 设置chsum函数回调
    rdb->update_cksum = rdbLoadProgressCallback;
    rdb->max_processing_chunk = server.loading_process_events_interval_bytes;
    
    //  检查版本号
    if (rioRead(rdb,buf,9) == 0) goto eoferr;
    buf[9] = '\0';
    if (memcmp(buf,"REDIS",5) != 0) {
        serverLog(LL_WARNING,"Wrong signature trying to load DB from file");
        errno = EINVAL;
        return C_ERR;
    }
    rdbver = atoi(buf+5);
    if (rdbver < 1 || rdbver > RDB_VERSION) {
        serverLog(LL_WARNING,"Can't handle RDB format version %d",rdbver);
        errno = EINVAL;
        return C_ERR;
    }

    /* Key-specific attributes, set by opcodes before the key type. */
    long long lru_idle = -1, lfu_freq = -1, expiretime = -1, now = mstime();
    long long lru_clock = LRU_CLOCK();
    
    // 解析rdb文件，rdb文件存储方式 type+object
    while(1) {
        robj *key, *val;

        // 根据type 进行object解析
        /* Read type. */
        if ((type = rdbLoadType(rdb)) == -1) goto eoferr;

        /* Handle special types. */
        if (type == RDB_OPCODE_EXPIRETIME) {
            /* EXPIRETIME: load an expire associated with the next key
             * to load. Note that after loading an expire we need to
             * load the actual type, and continue. */
            // 读取key-value的过期时间，时间单位为秒
            expiretime = rdbLoadTime(rdb);
            expiretime *= 1000;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EXPIRETIME_MS) {
            /* EXPIRETIME_MS: milliseconds precision expire times introduced
             * with RDB v3. Like EXPIRETIME but no with more precision. */
            // 读取key-value的过期时间，时间单位为毫秒 
            expiretime = rdbLoadMillisecondTime(rdb,rdbver);
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_FREQ) {
            // 读取LFU过期信息
            /* FREQ: LFU frequency. */
            uint8_t byte;
            if (rioRead(rdb,&byte,1) == 0) goto eoferr;
            lfu_freq = byte;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_IDLE) {
            // 读取LRU过期信息
            /* IDLE: LRU idle time. */
            uint64_t qword;
            if ((qword = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            lru_idle = qword;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_EOF) {
            // 解析到EOF,载入完毕
            /* EOF: End of file, exit the main loop. */
            break;
        } else if (type == RDB_OPCODE_SELECTDB) {
            // 解析数据库id
            /* SELECTDB: Select the specified database. */
            if ((dbid = rdbLoadLen(rdb,NULL)) == RDB_LENERR) goto eoferr;
            if (dbid >= (unsigned)server.dbnum) {
                serverLog(LL_WARNING,
                    "FATAL: Data file was created with a Redis "
                    "server configured to handle more than %d "
                    "databases. Exiting\n", server.dbnum);
                exit(1);
            }
            // 切换数据库
            db = server.db+dbid;
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_RESIZEDB) {
            // 解析dict和expires的size，并创建对应的大小的字典
            /* RESIZEDB: Hint about the size of the keys in the currently
             * selected data base, in order to avoid useless rehashing. */
            uint64_t db_size, expires_size;
            if ((db_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            if ((expires_size = rdbLoadLen(rdb,NULL)) == RDB_LENERR)
                goto eoferr;
            dictExpand(db->dict,db_size);
            dictExpand(db->expires,expires_size);
            continue; /* Read next opcode. */
        } else if (type == RDB_OPCODE_AUX) {
            // 载入aux 辅助信息 存储载rsi中
            /* AUX: generic string-string fields. Use to add state to RDB
             * which is backward compatible. Implementations of RDB loading
             * are requierd to skip AUX fields they don't understand.
             *
             * An AUX field is composed of two strings: key and value. */
            robj *auxkey, *auxval;
            if ((auxkey = rdbLoadStringObject(rdb)) == NULL) goto eoferr;
            if ((auxval = rdbLoadStringObject(rdb)) == NULL) goto eoferr;

            if (((char*)auxkey->ptr)[0] == '%') {
                /* All the fields with a name staring with '%' are considered
                 * information fields and are logged at startup with a log
                 * level of NOTICE. */
                serverLog(LL_NOTICE,"RDB '%s': %s",
                    (char*)auxkey->ptr,
                    (char*)auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-stream-db")) {
                if (rsi) rsi->repl_stream_db = atoi(auxval->ptr);
            } else if (!strcasecmp(auxkey->ptr,"repl-id")) {
                if (rsi && sdslen(auxval->ptr) == CONFIG_RUN_ID_SIZE) {
                    memcpy(rsi->repl_id,auxval->ptr,CONFIG_RUN_ID_SIZE+1);
                    rsi->repl_id_is_set = 1;
                }
            } else if (!strcasecmp(auxkey->ptr,"repl-offset")) {
                if (rsi) rsi->repl_offset = strtoll(auxval->ptr,NULL,10);
            } else if (!strcasecmp(auxkey->ptr,"lua")) {
                /* Load the script back in memory. */
                if (luaCreateFunction(NULL,server.lua,auxval) == NULL) {
                    rdbExitReportCorruptRDB(
                        "Can't load Lua script from RDB file! "
                        "BODY: %s", auxval->ptr);
                }
            } else {
                /* We ignore fields we don't understand, as by AUX field
                 * contract. */
                serverLog(LL_DEBUG,"Unrecognized RDB AUX field: '%s'",
                    (char*)auxkey->ptr);
            }

            decrRefCount(auxkey);
            decrRefCount(auxval);
            continue; /* Read type again. */
        } else if (type == RDB_OPCODE_MODULE_AUX) {
           // 载入 module aux 信息 
            /* This is just for compatibility with the future: we have plans
             * to add the ability for modules to store anything in the RDB
             * file, like data that is not related to the Redis key space.
             * Such data will potentially be stored both before and after the
             * RDB keys-values section. For this reason since RDB version 9,
             * we have the ability to read a MODULE_AUX opcode followed by an
             * identifier of the module, and a serialized value in "MODULE V2"
             * format. */
            uint64_t moduleid = rdbLoadLen(rdb,NULL);
            moduleType *mt = moduleTypeLookupModuleByID(moduleid);
            char name[10];
            moduleTypeNameByID(name,moduleid);

            if (!rdbCheckMode && mt == NULL) {
                /* Unknown module. */
                serverLog(LL_WARNING,"The RDB file contains AUX module data I can't load: no matching module '%s'", name);
                exit(1);
            } else if (!rdbCheckMode && mt != NULL) {
                /* This version of Redis actually does not know what to do
                 * with modules AUX data... */
                serverLog(LL_WARNING,"The RDB file contains AUX module data I can't load for the module '%s'. Probably you want to use a newer version of Redis which implements aux data callbacks", name);
                exit(1);
            } else {
                /* RDB check mode. */
                robj *aux = rdbLoadCheckModuleValue(rdb,name);
                decrRefCount(aux);
            }
        }

        // 解析key-value
        /* Read key */
        if ((key = rdbLoadStringObject(rdb)) == NULL) goto eoferr;
        /* Read value */
        if ((val = rdbLoadObject(type,rdb,key)) == NULL) goto eoferr;
        /* Check if the key already expired. This function is used when loading
         * an RDB file from disk, either at startup, or when an RDB was
         * received from the master. In the latter case, the master is
         * responsible for key expiry. If we would expire keys here, the
         * snapshot taken by the master may not be reflected on the slave.
         * 
         * 如果服务器为主节点的话，
         * 那么在键已经过期的时候，不再将它们关联到数据库中去
         * 
         *  */
        if (server.masterhost == NULL && !loading_aof && expiretime != -1 && expiretime < now) {
            decrRefCount(key);
            decrRefCount(val);
        } else {
            // 添加key-value到数据库中
            /* Add the new object in the hash table */
            dbAdd(db,key,val);

            /* Set the expire time if needed */
            // 设置过期时间
            if (expiretime != -1) setExpire(NULL,db,key,expiretime);
            
            /* Set usage information (for eviction). */
            // 设置过期策略值
            objectSetLRUOrLFU(val,lfu_freq,lru_idle,lru_clock);

            /* Decrement the key refcount since dbAdd() will take its
             * own reference. */
            decrRefCount(key);
        }

        /* Reset the state that is key-specified and is populated by
         * opcodes before the key, so that we start from scratch again. */
        expiretime = -1;
        lfu_freq = -1;
        lru_idle = -1;
    }
    /* Verify the checksum if RDB version is >= 5 */
    // 如果 RDB 版本 >= 5 ，那么比对校验和
    if (rdbver >= 5) {
        uint64_t cksum, expected = rdb->cksum;

        // 读入文件的校验和    
        if (rioRead(rdb,&cksum,8) == 0) goto eoferr;
        
        if (server.rdb_checksum) {
            memrev64ifbe(&cksum);
            // 比对校验和
            if (cksum == 0) {
                serverLog(LL_WARNING,"RDB file was saved with checksum disabled: no check performed.");
            } else if (cksum != expected) {
                serverLog(LL_WARNING,"Wrong RDB checksum. Aborting now.");
                rdbExitReportCorruptRDB("RDB CRC error");
            }
        }
    }
    return C_OK;

eoferr: /* unexpected end of file is handled here with a fatal exit */
    serverLog(LL_WARNING,"Short read or OOM loading DB. Unrecoverable error, aborting now.");
    rdbExitReportCorruptRDB("Unexpected EOF reading RDB file");
    return C_ERR; /* Just to avoid warning */
}

/* Like rdbLoadRio() but takes a filename instead of a rio stream. The
 * filename is open for reading and a rio stream object created in order
 * to do the actual loading. Moreover the ETA displayed in the INFO
 * output is initialized and finalized.
 *
 * If you pass an 'rsi' structure initialied with RDB_SAVE_OPTION_INIT, the
 * loading code will fiil the information fields in the structure. */
// 将rdb文件保持的数据载入到数据库中
int rdbLoad(char *filename, rdbSaveInfo *rsi) {
    FILE *fp;
    rio rdb;
    int retval;

    // 打开rdb文件
    if ((fp = fopen(filename,"r")) == NULL) return C_ERR;
    // 设置载入标识
    startLoading(fp);
    // 初始化rio
    rioInitWithFile(&rdb,fp);
    // 载入rdb文件
    retval = rdbLoadRio(&rdb,rsi,0);
    // 关闭文件句柄
    fclose(fp);
    // 服务器从载入状态中退出
    stopLoading();
    return retval;
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of actual BGSAVEs. */
// 处理 BGSAVE 完成时发送的信号
void backgroundSaveDoneHandlerDisk(int exitcode, int bysignal) {
    // BGSAVE 成功
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background saving terminated with success");
        server.dirty = server.dirty - server.dirty_before_bgsave;
        server.lastsave = time(NULL);
        server.lastbgsave_status = C_OK;
    // BGSAVE 出错    
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background saving error");
        server.lastbgsave_status = C_ERR;

    // BGSAVE 被中断    
    } else {
        mstime_t latency;

        serverLog(LL_WARNING,
            "Background saving terminated by signal %d", bysignal);
        latencyStartMonitor(latency);
        // 移除临时文件
        rdbRemoveTempFile(server.rdb_child_pid);
        latencyEndMonitor(latency);
        latencyAddSampleIfNeeded("rdb-unlink-temp-file",latency);
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * tirggering an error condition. */
        if (bysignal != SIGUSR1)
            server.lastbgsave_status = C_ERR;
    }
    // 更新服务器状态
    server.rdb_child_pid = -1;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_last = time(NULL)-server.rdb_save_time_start;
    server.rdb_save_time_start = -1;
    /* Possibly there are slaves waiting for a BGSAVE in order to be served
     * (the first stage of SYNC is a bulk transfer of dump.rdb) */
    // 处理正在等待 BGSAVE 完成的那些 slave
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, RDB_CHILD_TYPE_DISK);
}

/* A background saving child (BGSAVE) terminated its work. Handle this.
 * This function covers the case of RDB -> Salves socket transfers for
 * diskless replication. 
 * 
 * bgsave 保存完成时 发送的信号 用于 主从 通过 rdb数据拷贝 
 *  主需要将 rdb 通过sock发送savle节点上
 * */
void backgroundSaveDoneHandlerSocket(int exitcode, int bysignal) {
    uint64_t *ok_slaves;

    // BGSAVE 成功
    if (!bysignal && exitcode == 0) {
        serverLog(LL_NOTICE,
            "Background RDB transfer terminated with success");
    // BGSAVE 出错        
    } else if (!bysignal && exitcode != 0) {
        serverLog(LL_WARNING, "Background transfer error");
    } else {
     // BGSAVE 被中断   
        serverLog(LL_WARNING,
            "Background transfer terminated by signal %d", bysignal);
    }

    // 更新状态
    server.rdb_child_pid = -1;
    server.rdb_child_type = RDB_CHILD_TYPE_NONE;
    server.rdb_save_time_start = -1;

    /* If the child returns an OK exit code, read the set of slave client
     * IDs and the associated status code. We'll terminate all the slaves
     * in error state.
     *
     * If the process returned an error, consider the list of slaves that
     * can continue to be empty, so that it's just a special case of the
     * normal code path. */
    ok_slaves = zmalloc(sizeof(uint64_t)); /* Make space for the count. */
    ok_slaves[0] = 0;
    // bgsave成功
    if (!bysignal && exitcode == 0) {
        int readlen = sizeof(uint64_t);

        if (read(server.rdb_pipe_read_result_from_child, ok_slaves, readlen) ==
                 readlen)
        {
            readlen = ok_slaves[0]*sizeof(uint64_t)*2;

            /* Make space for enough elements as specified by the first
             * uint64_t element in the array. */
            ok_slaves = zrealloc(ok_slaves,sizeof(uint64_t)+readlen);
            if (readlen &&
                read(server.rdb_pipe_read_result_from_child, ok_slaves+1,
                     readlen) != readlen)
            {
                ok_slaves[0] = 0;
            }
        }
    }

    close(server.rdb_pipe_read_result_from_child);
    close(server.rdb_pipe_write_result_to_parent);

    /* We can continue the replication process with all the slaves that
     * correctly received the full payload. Others are terminated. */
    listNode *ln;
    listIter li;

    // 将rdb 发送 到slaves
    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_END) {
            uint64_t j;
            int errorcode = 0;

            /* Search for the slave ID in the reply. In order for a slave to
             * continue the replication process, we need to find it in the list,
             * and it must have an error code set to 0 (which means success). */
            for (j = 0; j < ok_slaves[0]; j++) {
                if (slave->id == ok_slaves[2*j+1]) {
                    errorcode = ok_slaves[2*j+2];
                    break; /* Found in slaves list. */
                }
            }
            if (j == ok_slaves[0] || errorcode != 0) {
                serverLog(LL_WARNING,
                "Closing slave %s: child->slave RDB transfer failed: %s",
                    replicationGetSlaveName(slave),
                    (errorcode == 0) ? "RDB transfer child aborted"
                                     : strerror(errorcode));
                freeClient(slave);
            } else {
                serverLog(LL_WARNING,
                "Slave %s correctly received the streamed RDB file.",
                    replicationGetSlaveName(slave));
                /* Restore the socket as non-blocking. */
                anetNonBlock(NULL,slave->fd);
                anetSendTimeout(NULL,slave->fd,0);
            }
        }
    }
    zfree(ok_slaves);

    // 处理正在等待 BGSAVE 完成的那些 slave rdb数据同步
    updateSlavesWaitingBgsave((!bysignal && exitcode == 0) ? C_OK : C_ERR, RDB_CHILD_TYPE_SOCKET);
}

/* When a background RDB saving/transfer terminates, call the right handler.
 * 
 * rdb 保存完后的回调
 * 
 */
void backgroundSaveDoneHandler(int exitcode, int bysignal) {
    switch(server.rdb_child_type) {
    // master本地rdb bgsave操作    
    case RDB_CHILD_TYPE_DISK:
        backgroundSaveDoneHandlerDisk(exitcode,bysignal);
        break;
    // 主从 rdb 数据同步 bgsave操作    
    case RDB_CHILD_TYPE_SOCKET:
        backgroundSaveDoneHandlerSocket(exitcode,bysignal);
        break;
    default:
        serverPanic("Unknown RDB child type.");
        break;
    }
}

/* Spawn an RDB child that writes the RDB to the sockets of the slaves
 * that are currently in SLAVE_STATE_WAIT_BGSAVE_START state. */
// 子进程中 通过socket 将master的bgsave的rdb文件发送到 副本集中 slave启动时用作的数据同步
int rdbSaveToSlavesSockets(rdbSaveInfo *rsi) {
    int *fds;
    uint64_t *clientids;
    int numfds;
    listNode *ln;
    listIter li;
    pid_t childpid;
    long long start;
    int pipefds[2];

    if (server.aof_child_pid != -1 || server.rdb_child_pid != -1) return C_ERR;

    /* Before to fork, create a pipe that will be used in order to
     * send back to the parent the IDs of the slaves that successfully
     * received all the writes. */
    // 创建一个管道 半双工 只能用于父子进程或者兄弟进程之间通信
    if (pipe(pipefds) == -1) return C_ERR;
    server.rdb_pipe_read_result_from_child = pipefds[0];
    server.rdb_pipe_write_result_to_parent = pipefds[1];

    /* Collect the file descriptors of the slaves we want to transfer
     * the RDB to, which are i WAIT_BGSAVE_START state. */
    fds = zmalloc(sizeof(int)*listLength(server.slaves));
    /* We also allocate an array of corresponding client IDs. This will
     * be useful for the child process in order to build the report
     * (sent via unix pipe) that will be sent to the parent. */
    clientids = zmalloc(sizeof(uint64_t)*listLength(server.slaves));
    numfds = 0;

    listRewind(server.slaves,&li);
    while((ln = listNext(&li))) {
        client *slave = ln->value;

        if (slave->replstate == SLAVE_STATE_WAIT_BGSAVE_START) {
            clientids[numfds] = slave->id;
            fds[numfds++] = slave->fd;
            replicationSetupSlaveForFullResync(slave,getPsyncInitialOffset());
            /* Put the socket in blocking mode to simplify RDB transfer.
             * We'll restore it when the children returns (since duped socket
             * will share the O_NONBLOCK attribute with the parent). */
            anetBlock(NULL,slave->fd);
            anetSendTimeout(NULL,slave->fd,server.repl_timeout*1000);
        }
    }

    /* Create the child process. */
    openChildInfoPipe();
    start = ustime();
    if ((childpid = fork()) == 0) {
        // 子进程 通过socket 发送到slave中
        /* Child */
        int retval;
        rio slave_sockets;

        rioInitWithFdset(&slave_sockets,fds,numfds);
        zfree(fds);

        closeListeningSockets(0);
        redisSetProcTitle("redis-rdb-to-slaves");

        retval = rdbSaveRioWithEOFMark(&slave_sockets,NULL,rsi);
        if (retval == C_OK && rioFlush(&slave_sockets) == 0)
            retval = C_ERR;

        if (retval == C_OK) {
            size_t private_dirty = zmalloc_get_private_dirty(-1);

            if (private_dirty) {
                serverLog(LL_NOTICE,
                    "RDB: %zu MB of memory used by copy-on-write",
                    private_dirty/(1024*1024));
            }

            server.child_info_data.cow_size = private_dirty;
            sendChildInfo(CHILD_INFO_TYPE_RDB);

            /* If we are returning OK, at least one slave was served
             * with the RDB file as expected, so we need to send a report
             * to the parent via the pipe. The format of the message is:
             *
             * <len> <slave[0].id> <slave[0].error> ...
             *
             * len, slave IDs, and slave errors, are all uint64_t integers,
             * so basically the reply is composed of 64 bits for the len field
             * plus 2 additional 64 bit integers for each entry, for a total
             * of 'len' entries.
             *
             * The 'id' represents the slave's client ID, so that the master
             * can match the report with a specific slave, and 'error' is
             * set to 0 if the replication process terminated with a success
             * or the error code if an error occurred. */
            void *msg = zmalloc(sizeof(uint64_t)*(1+2*numfds));
            uint64_t *len = msg;
            uint64_t *ids = len+1;
            int j, msglen;

            *len = numfds;
            for (j = 0; j < numfds; j++) {
                *ids++ = clientids[j];
                *ids++ = slave_sockets.io.fdset.state[j];
            }

            /* Write the message to the parent. If we have no good slaves or
             * we are unable to transfer the message to the parent, we exit
             * with an error so that the parent will abort the replication
             * process with all the childre that were waiting. */
            msglen = sizeof(uint64_t)*(1+2*numfds);
            if (*len == 0 ||
                write(server.rdb_pipe_write_result_to_parent,msg,msglen)
                != msglen)
            {
                retval = C_ERR;
            }
            zfree(msg);
        }
        zfree(clientids);
        rioFreeFdset(&slave_sockets);
        exitFromChild((retval == C_OK) ? 0 : 1);
    } else {
        /* Parent */
        if (childpid == -1) {
            serverLog(LL_WARNING,"Can't save in background: fork: %s",
                strerror(errno));

            /* Undo the state change. The caller will perform cleanup on
             * all the slaves in BGSAVE_START state, but an early call to
             * replicationSetupSlaveForFullResync() turned it into BGSAVE_END */
            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                client *slave = ln->value;
                int j;

                for (j = 0; j < numfds; j++) {
                    if (slave->id == clientids[j]) {
                        slave->replstate = SLAVE_STATE_WAIT_BGSAVE_START;
                        break;
                    }
                }
            }
            close(pipefds[0]);
            close(pipefds[1]);
            closeChildInfoPipe();
        } else {
            server.stat_fork_time = ustime()-start;
            server.stat_fork_rate = (double) zmalloc_used_memory() * 1000000 / server.stat_fork_time / (1024*1024*1024); /* GB per second. */
            latencyAddSampleIfNeeded("fork",server.stat_fork_time/1000);

            serverLog(LL_NOTICE,"Background RDB transfer started by pid %d",
                childpid);
            server.rdb_save_time_start = time(NULL);
            server.rdb_child_pid = childpid;
            server.rdb_child_type = RDB_CHILD_TYPE_SOCKET;
            updateDictResizePolicy();
        }
        zfree(clientids);
        zfree(fds);
        return (childpid == -1) ? C_ERR : C_OK;
    }
    return C_OK; /* Unreached. */
}

/***
 *   由于save命令的保存工作是在主进程直接执行，所以命令执行时，redis服务器会被阻塞，
 *   客户端发送的所有命令都会被拒绝。此外，在进行bgsave命令是save命令会被服务器拒绝。
 **/
void saveCommand(client *c) {

    // BGSAVE 已经在执行中，不能再执行 SAVE
    // 否则将产生竞争条件
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
        return;
    }
    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);
    // 执行 save命令
    if (rdbSave(server.rdb_filename,rsiptr) == C_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}

/* BGSAVE [SCHEDULE] */
/**
 *   由于bgsave命令的保存工作是在子进程中执行，在子进程中带有主进程的数据副本，
 *    避免了和主进程竞争db->dict，
 *      
 *   所以期间服务器仍然可以继续处理客户端的命令。在执行bgsave时，
 *   拒绝save和其他bgsave命令为了避免多个进程调用
 *   rdbsave进行磁盘写入，产生竞争。
 * **/
void bgsaveCommand(client *c) {
    int schedule = 0;

    /* The SCHEDULE option changes the behavior of BGSAVE when an AOF rewrite
     * is in progress. Instead of returning an error a BGSAVE gets scheduled. */
    if (c->argc > 1) {
        if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"schedule")) {
            schedule = 1;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    rdbSaveInfo rsi, *rsiptr;
    rsiptr = rdbPopulateSaveInfo(&rsi);

    // 服务器中已经在执行bgsave
    if (server.rdb_child_pid != -1) {
        addReplyError(c,"Background save already in progress");
    // 服务器中在执行BGREWRITEAOF，重写aof    
    } else if (server.aof_child_pid != -1) {
        if (schedule) {
            server.rdb_bgsave_scheduled = 1;
            addReplyStatus(c,"Background saving scheduled");
        } else {
            addReplyError(c,
                "An AOF log rewriting in progress: can't BGSAVE right now. "
                "Use BGSAVE SCHEDULE in order to schedule a BGSAVE whenever "
                "possible.");
        }
    // 执行bgsave逻辑    
    } else if (rdbSaveBackground(server.rdb_filename,rsiptr) == C_OK) {
        addReplyStatus(c,"Background saving started");
    } else {
        addReply(c,shared.err);
    }
}

/* Populate the rdbSaveInfo structure used to persist the replication
 * information inside the RDB file. Currently the structure explicitly
 * contains just the currently selected DB from the master stream, however
 * if the rdbSave*() family functions receive a NULL rsi structure also
 * the Replication ID/offset is not saved. The function popultes 'rsi'
 * that is normally stack-allocated in the caller, returns the populated
 * pointer if the instance has a valid master client, otherwise NULL
 * is returned, and the RDB saving will not persist any replication related
 * information. */
/**
 *  填充用于持久复制的rdbSaveInfo结构 
 *  当前的rdbSaveInfo只包含在master选择的DB中
 *  
 *  主从 rdb 数据同步
 *  
 * 
 **/
rdbSaveInfo *rdbPopulateSaveInfo(rdbSaveInfo *rsi) {
    rdbSaveInfo rsi_init = RDB_SAVE_INFO_INIT;
    *rsi = rsi_init;

    /* If the instance is a master, we can populate the replication info
     * only when repl_backlog is not NULL. If the repl_backlog is NULL,
     * it means that the instance isn't in any replication chains. In this
     * scenario the replication info is useless, because when a slave
     * connects to us, the NULL repl_backlog will trigger a full
     * synchronization, at the same time we will use a new replid and clear
     * replid2. */
    if (!server.masterhost && server.repl_backlog) {
        /* Note that when server.slaveseldb is -1, it means that this master
         * didn't apply any write commands after a full synchronization.
         * So we can let repl_stream_db be 0, this allows a restarted slave
         * to reload replication ID/offset, it's safe because the next write
         * command must generate a SELECT statement. */
        rsi->repl_stream_db = server.slaveseldb == -1 ? 0 : server.slaveseldb;
        return rsi;
    }

    /* If the instance is a slave we need a connected master
     * in order to fetch the currently selected DB. */
    if (server.master) {
        rsi->repl_stream_db = server.master->db->id;
        return rsi;
    }

    /* If we have a cached master we can use it in order to populate the
     * replication selected DB info inside the RDB file: the slave can
     * increment the master_repl_offset only from data arriving from the
     * master, so if we are disconnected the offset in the cached master
     * is valid. */
    if (server.cached_master) {
        rsi->repl_stream_db = server.cached_master->db->id;
        return rsi;
    }
    return NULL;
}
