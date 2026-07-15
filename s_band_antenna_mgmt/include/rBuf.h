//////////////////////////////////////////////////////////////////////////  
///    COPYRIGHT NOTICE  
///    Copyright (c) 2024,513  
///    All rights reserved.  
///  
/// @file   rBuf.h  
/// @brief  
///  
///  环形缓存功能.入口写入,出口读取和删除.
///  注意:只用于一个生产者和一个消费者,两者可以是不同的任务或中断.
///       多生产者或多消费者需要加锁.
///       Put在生产者调用,Get和Remove在消费者调用.
///
/// @version 1.00
/// @author  xumd
/// @date    20240528
///  
///  修订说明：最初版本  
////////////////////////////////////////////////////////////////////////// 
#ifndef __INC_RBUF_H
#define __INC_RBUF_H
#include "gtypes.h"

typedef struct rbuf
{	
	UCHAR* buf;			/**<需配置，缓存地址*/
	UINT len;			/**<需配置，缓存长度(字节)*/
	UINT inIndex;		/**< 入指针*/
	UINT outIndex;		/**< 出指针*/
}RBUF;


/** 从入口写入多字节. 
 *     @param pRbuf RBUF指针变量
 *     @param buf 输入多字节缓存
 *     @param len 缓存长度
 *     @return  实际的输入长度. 
 */	
extern UINT rBufPut(RBUF* pRbuf,UCHAR* buf,UINT len);

/** 从出口获取多字节(不删除). 
 *     @param pRbuf RBUF指针变量
 *     @param buf 输出多字节缓存
 *     @param len 缓存长度
 *     @return  实际的输出长度.  
 */	
extern UINT rBufGet(RBUF* pRbuf,UCHAR* buf,UINT len);

/** 从出口删除多字节. 
 *     @param pRbuf RBUF指针变量
 *     @param len 要删除的长度
 *     @return  实际的删除长度. 
 */	
extern UINT rBufRemove(RBUF* pRbuf,UINT len);

/** 获取当前的可用字节数. 
 *     @param pRbuf RBUF指针变量
 *     @return  字节数  
 */
extern UINT rBufNBytes(RBUF* pRbuf);

/** 判断是否满. 
 *     @param pRbuf RBUF指针变量
 *     @return  BOOL TRUE表示满,FALSE表示不满.  
 */
extern BOOL rBufIsFull(RBUF* pRbuf);

/** 清空. 
 *     @param pRbuf RBUF指针变量
 *     @return  无  
 */
extern void rBufClear(RBUF* pRbuf);

/** 初始化. 
 *     @param pRbuf RBUF指针变量
 *     @return  无  
 */
extern void rBufInit(RBUF* pRbuf);


#endif //__INC_RBUF_H
