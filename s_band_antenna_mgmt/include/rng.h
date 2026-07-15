//////////////////////////////////////////////////////////////////////////  
///    COPYRIGHT NOTICE  
///    Copyright (c) 2015,513  
///    All rights reserved.  
///  
/// @file   rng.h  
/// @brief  
///  
///  环形队列功能.
///
/// @version 1.00
/// @author  xumd
/// @date    20170328
///  
///  修订说明：最初版本  
////////////////////////////////////////////////////////////////////////// 
#ifndef __INC_RNG_H
#define __INC_RNG_H

#include "gtypes.h"

/** 环形队列. 
 *   环形队列结构体。如果需要三模则定义三个RNG变量,且buf指向同一个缓冲地址即可,因为一般缓冲里的数不会长时间不处理.
 *	 注意需要关开中断产生临界区.
 */	
typedef struct rng
{	
	UCHAR* buf;			/**<需配置，缓冲指针*/
	UINT msgLen;		/**<需配置，一条消息长度(字节)*/
	UINT maxMsgs;		/**<需配置，最多多少条消息*/
	UINT inIndex;		/**< 入指针*/
	UINT outIndex;		/**< 出指针*/
}RNG;


/** 用于定义RNG变量的宏. 
 *     采用宏的方式定义RNG变量,可以避免重复的参数配置错误.
 *	   需要先定义消息结构体.
 *     @param rngName RNG变量名称.
 *     @param MSG_STRUCT 消息结构体类型,需要提前用typedef定义结构体类型.如定义了typedef struct {USHORT d[32];}MSG;则MSG_STRUCT应为MSG.
 *     @param bufName 消息缓存名称.
 *     @param maxMsgs 最多多少条消息.
 */
#define RNG_DEF(rngName,MSG_STRUCT,bufName,maxMsgs)	\
	MSG_STRUCT bufName[(maxMsgs)];RNG rngName={(UCHAR*)bufName,sizeof(MSG_STRUCT),(maxMsgs),0,0}	/*特殊设计*/

/** 用于定义1553的RNG变量的宏. 
 *     采用宏的方式定义RNG变量,可以避免重复的参数配置错误.
 *	   注意:每个消息的第一个字代表消息有效字个数.
 *     @param rngName RNG变量名称.
 *     @param bufName 消息缓存名称.
 *     @param msgWords 每个消息的字个数.
 *     @param maxMsgs 最多多少条消息.
 */
#define RNG_DEF_1553(rngName,bufName,msgWords,maxMsgs)	\
	USHORT bufName[(1+(msgWords))*(maxMsgs)];RNG rngName={(UCHAR*)bufName,(1+(msgWords))*2,(maxMsgs),0,0}	/*特殊设计*/


/** 获取当前的写入缓冲指针. 
 *      获取当前的写入缓冲指针,用户使用此指针进行写入.
 *		此函数在生产者端调用.
 *     @param rng RNG指针变量 (RNG*)
 *     @return  写入缓冲指针(UCHAR*). 如果RNG采用宏RNG_DEF定义,则可将返回值强制转换为MSG_STRUCT.  
 *     @note  如果RNG需要三模,则定义三个RNG变量,调用之前先tmr(&rng0,&rng1,&rng2,&rng0).
 */	
extern UCHAR*  rngGetWriteBuf(RNG* pRng);

/** 写入完成. 
 *      用户写入一个消息后,调用此函数表示写入完成,此函数内部使写指针前进一个消息。
 *		此函数在生产者端调用.
 *     @param rng RNG指针变量 (RNG*)
 *     @return  无  
 *     @note  如果RNG需要三模,则定义三个RNG变量,三个rng变量都要调用.
 */	
extern void rngWriteDone(RNG* pRng);

/** 获取当前的读出缓冲指针. 
 *      获取当前的读出缓冲指针,用户使用此指针进行读出.
 *		此函数在消费者端调用.
 *     @param rng RNG指针变量 (RNG*)
 *     @return  读出缓冲指针(UCHAR*). 如果RNG采用宏RNG_DEF定义,则可将返回值强制转换为MSG_STRUCT.
 *     @note  如果RNG需要三模,则定义三个RNG变量,调用之前先tmr(&rng0,&rng1,&rng2,&rng0).
 */	
extern UCHAR*  rngGetReadBuf(RNG* pRng);

/** 读出完成. 
 *      用户读出一个消息后,调用此函数表示读出完成,此函数内部使读指针前进一个消息。
 *		此函数在消费者端调用.
 *     @param rng RNG指针变量 (RNG*)
 *     @return  无  
 *     @note  如果RNG需要三模,则定义三个RNG变量,三个rng变量都要调用.
 */	
extern void rngReadDone(RNG* pRng);

/** 获取当前的可用消息数. 
 *      获取当前有多少输入的消息没处理。
 *     @param rng RNG指针变量 (RNG*)
 *     @return  消息数(UINT)  
 *     @note  如果RNG需要三模,则定义三个RNG变量,调用之前先tmr(&rng0,&rng1,&rng2,&rng0).
 */
extern UINT rngGetNMsgs(RNG* pRng);

/** 清空RNG. 
 *      使入指针和出指针赋值为0,相当于清空RNG。
 *     @param rng RNG指针变量 (RNG*)
 *     @return  无  
 *     @note  如果RNG需要三模,则定义三个RNG变量,三个rng变量都要调用.
 */
extern void rngClear(RNG* pRng);

/** 初始化RNG. 
 *      使入指针和出指针赋值为0,相当于清空RNG。
 *     @param rng RNG指针变量 (RNG*)
 *     @return  无  
 *     @note  如果RNG需要三模,则定义三个RNG变量,三个rng变量都要调用.
 */
extern void rngInit(RNG* pRng);

/** 判断RNG是否满. 
 *      RNG是否满,即如果入指针加1后和出指针相等则认为满.在写入消息前判断一下是否已满.
 *     @param rng RNG指针变量 (RNG*)
 *     @return  BOOL TRUE表示满,FALSE表示不满.  
 *     @note  如果RNG需要三模,则定义三个RNG变量,调用之前先tmr(&rng0,&rng1,&rng2,&rng0).
 */
extern BOOL rngIsFul(RNG* pRng);


/*=====================================乒乓操作接口==================================================*/
/*为了通用化,乒乓的数据结构与RNG一致,在定义时buf必须为2个消息大小,入指针为0,出指针为1.*/
typedef RNG PINGP;

/** 用于定义PINGP变量的宏. 
 *     采用宏的方式定义PINGP变量,可以避免重复的参数配置错误.
 *	   需要先定义消息结构体.
 *     @param pingpName PINGP变量名称.
 *     @param MSG_STRUCT 消息结构体类型,需要提前用typedef定义结构体类型.如定义了typedef struct {USHORT d[32];}MSG;则MSG_STRUCT应为MSG.
 *     @param bufName 消息缓存名称.
 */
#define PINGP_DEF(pingpName,MSG_STRUCT,bufName)	\
	MSG_STRUCT bufName[2];PINGP pingpName={(UCHAR*)bufName,sizeof(MSG_STRUCT),2,0,1}	/*特殊设计*/

/** 用于定义1553的PINGP变量的宏. 
 *     采用宏的方式定义PINGP变量,可以避免重复的参数配置错误.
 *	   注意:每个消息的第一个字代表消息有效字个数.
 *     @param pingpName PINGP变量名称.
 *     @param bufName 消息缓存名称.
 *     @param msgWords 每个消息的字个数.
 */
#define PINGP_DEF_1553(pingpName,bufName,msgWords)	\
	USHORT bufName[(1+(msgWords))*2];PINGP pingpName={(UCHAR*)bufName,(1+(msgWords))*2,2,0,1}	/*特殊设计*/


/** 获取当前的写入缓冲指针. 
 *      获取当前的写入缓冲指针,用户使用此指针进行写入.
 *		此函数在生产者端调用.
 *     @param pPingp PINGP指针变量 (PINGP*)
 *     @return  写入缓冲指针(UCHAR*)  
 *     @note  如果PINGP需要三模,则定义三个PINGP变量,调用之前先tmr(&pingp0,&pingp1,&pingp2,&pingp0).
 */	
extern UCHAR*  pingpGetWriteBuf(PINGP* pPingp);

/** 获取当前的读出缓冲指针. 
 *      获取当前的读出缓冲指针,用户使用此指针进行读出.
 *		此函数在消费者端调用.
 *     @param pPingp PINGP指针变量 (PINGP*)
 *     @return  读出缓冲指针(UCHAR*)  
 *     @note  如果PINGP需要三模,则定义三个PINGP变量,调用之前先tmr(&pingp0,&pingp1,&pingp2,&pingp0).
 */	
extern UCHAR*  pingpGetReadBuf(PINGP* pPingp);

/** 乒乓交换缓存区. 
 *      使乒乓的两个缓存区相互交换位置。
 *		写入或读出后进行交换,一般,生产者或消费者哪个优先级低哪个调用此函数进行交换.
 *     @param pPingp PINGP指针变量 (PINGP*)
 *     @return  无  
 *     @note  如果PINGP需要三模,则定义三个PINGP变量,三个pingp变量都要调用.
 */	
extern void pingpSwap(PINGP* pPingp);

/** 初始化. 
 *      使入指针inIndex为0,出指针outIndex为1,maxMsgs为2,数据缓存清全零。
 *     @param pPingp PINGP指针变量 (PINGP*)
 *     @return  无  
 *     @note  如果PINGP需要三模,则定义三个PINGP变量,三个pingp变量都要调用.
 */
extern void pingpInit(PINGP* pPingp);

#endif //__INC_RNG_H
