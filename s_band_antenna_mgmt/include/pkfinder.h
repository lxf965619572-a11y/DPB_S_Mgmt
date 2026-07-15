//////////////////////////////////////////////////////////////////////////  
///    COPYRIGHT NOTICE  
///    Copyright (c) 2024,513  
///    All rights reserved.  
///  
/// @file   pkfinder.h 
/// @brief  
///  
///  找包功能.
///  在数据缓存中查找包格式. 用于串口,TCP等非完整包传输的接收.
///
/// @version 1.00
/// @author  xumd
/// @date    20240528
///  
///  修订说明：最初版本  
////////////////////////////////////////////////////////////////////////// 
#ifndef _PKFINDER_H_
#define _PKFINDER_H_

#include "gtypes.h"
#include "rBuf.h"

/**校验并执行函数指针. 
 *	   当找到一包时回调此函数,进行校验并执行.
 *     @param pkfndr PKFINDER指针. 
 *     @param pkBuf 包缓存地址.
 *     @param pkLen 包缓存长度.
 *     @return TRUE代表校验包通过或包尾正确,FASLE代表校验不通过或包尾错误. 
 */
typedef BOOL (*chkExecFptr) (UCHAR* pkBuf,UINT pkLen);

typedef struct pkfinder
{ 
	UCHAR* headBuf;			/**< 需要配置.包头缓存.用于比对匹配是否找到包.*/
	UINT headLen;			/**< 需要配置.包头长度*/
	UCHAR* headMasks;		/**< 需要配置.包头掩码数组首地址,当比对包头时先将对应数据进行与.长度必须与headLen一致.*/
	BOOL isPkFixLen;		/**< 需要配置.是否是定长包*/
	UINT pkFixLen;			/**< 需要配置.定长包的长度*/
	UINT pkLenPos;			/**< 需要配置.变长包长度域所在位置*/
	UINT pkLenLen;			/**< 需要配置.变长包长度域所占字节数.不能大于4.*/
	UCHAR pkLenFirstMsk;	/**< 需要配置.变长包长度域第一个字节的掩码*/
	UCHAR pkLenEnd;			/**< 需要配置.变长包长度域的大小端.采用宏ENDIAN_LITTLE或ENDIAN_BIG.*/
	UINT pkLenAdd;			/**< 需要配置.变长包长度域的附加值,即:包长=长度域值+pkLenAdd.*/
}PKFINDER;


/**找包. 
 *	   在输入缓存中按字节依次扫描,查看包头和长度是否OK.
 *     @param pkfndr PKFINDER指针. 
 *     @param buf 输入缓存地址.
 *     @param len 输入缓存长度.
 *     @param fndPos 输出找到包头的起始位置,或找到整个包的起始位置.
 *     @param fndLen 输出找到完整包的长度.
 *     @return -1代表异常,0代表未找到包头,1代表找到包头但长度不足,2代表找到完整包. 
 */
extern int pkfinderFind(PKFINDER* pkfndr,UCHAR* buf,UINT len,UINT* fndPos,UINT* fndLen);

/**找包并解析. 
 *	   当串行数据放入rbuf后,使用此函数从rbuf中查找包,
 *     如果找到包则进行处理并删除缓存数据,如果没找到则进行适当删除缓存数据.
 *     @param pkfndr PKFINDER指针 
 *     @param pRbuf RBUF指针.
 *     @param parseBuf 解析缓存地址,用于将rbuf的所有有效数据读出,parseBuf不能小于rbuf的buf大小.
 *     @param parseBufLen 解析缓存长度.
 *     @param chkExec 找到包后调用此函数进行判断包校验并执行.此函数需要尽快运行完毕.
 *     @return OK代表正常执行,ERROR代表有严重错误. 
 */
extern STATUS pkfinderParse(PKFINDER* pkfndr,RBUF* pRbuf,UCHAR* parseBuf,UINT parseBufLen,chkExecFptr chkExec);


#endif/*_PKFINDER_H_*/


