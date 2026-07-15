//////////////////////////////////////////////////////////////////////////  
///    COPYRIGHT NOTICE  
///    Copyright (c) 2024,513  
///    All rights reserved.  
///  
/// @file   pkfinder.c
/// @brief  
///  
///  找包功能.
///
/// @version 1.00
/// @author  xumd
/// @date    20240528
///  
///  修订说明：最初版本  
////////////////////////////////////////////////////////////////////////// 
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <net/if.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <sys/mount.h>
#include <stdint.h>
#include <execinfo.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "pkfinder.h"


int pkfinderFind( PKFINDER* pkfndr,UCHAR* buf,UINT len,UINT* fndPos,UINT* fndLen )
{
	unsigned int i,j;
	BOOL isHeadOK;
	UINT pkLen;
	UCHAR pkLenArr[4] = {0};

	if ((pkfndr==NULL)||(buf==NULL))
	{
		return -1;
	}
	if ((pkfndr->headBuf==NULL)||(pkfndr->headMasks==NULL))
	{
		return -1;
	}

	/*************1.找包头*************/
	isHeadOK=FALSE;
	for (i=0;i<(len - pkfndr->headLen + 1);++i)/*每次移动一个字节*/
	{
		for (j=0;j<pkfndr->headLen;++j)/*依次比对包头的每个字节*/
		{
			if ((buf[i+j]&pkfndr->headMasks[j])!=pkfndr->headBuf[j])
			{
				break;
			}
		}
		if (j==pkfndr->headLen)/*找到包头*/
		{
			isHeadOK=TRUE;
			*fndPos=i;/*包起始位置赋值*/
			break;
		}
	}
	if (isHeadOK==FALSE)
	{
		return 0;
	}

	/*************2.判断是否存在完整包*************/
	if (pkfndr->isPkFixLen==TRUE)/*定长包*/
	{
		if ((*fndPos+pkfndr->pkFixLen)>len)/*长度不够完整包*/
		{
			return 1;
		}
		else
		{
			*fndLen=pkfndr->pkFixLen;
			return 2;
		}
	}
	else/*变长包*/
	{
		if (pkfndr->pkLenLen>4)/*异常*/
		{
			return -1;
		}

		if ((*fndPos + pkfndr->pkLenPos + pkfndr->pkLenLen)>len)/*不能提取包长*/
		{
			return 1;
		}
		else
		{
			for (i=0;i<pkfndr->pkLenLen;++i)/*取出包长 到 数组*/
			{
				pkLenArr[i]=buf[*fndPos + pkfndr->pkLenPos +i];
			}
			pkLenArr[0]&=pkfndr->pkLenFirstMsk;/*首字节与掩码*/
			pkLen=0;
			if (pkfndr->pkLenEnd==ENDIAN_LITTLE)/*包长在buf中是小端*/
			{
				for (i=0;i<pkfndr->pkLenLen;++i)/*得到包长*/
				{
					pkLen|=pkLenArr[i]<<(i*8);
				}
			}
			else/*包长在buf中是大端*/
			{
				for (i=0;i<pkfndr->pkLenLen;++i)/*得到包长*/
				{
					pkLen|=pkLenArr[pkfndr->pkLenLen-i-1]<<(i*8);
				}
			}

			if ((*fndPos + pkLen + pkfndr->pkLenAdd) > len)/*长度不够完整包*/
			{
				*fndLen=pkLen+pkfndr->pkLenAdd;
				return 1;
			}
			else
			{
				*fndLen=pkLen+pkfndr->pkLenAdd;
				return 2;
			}
		}
	}
}

STATUS pkfinderParse( PKFINDER* pkfndr,RBUF* pRbuf,UCHAR* parseBuf,UINT parseBufLen,chkExecFptr chkExec )
{
	UINT datLen;
	int findRslt;
	UINT fndIndex=0;
	UINT pkPos=0;
	UINT pkLen=0;
	BOOL f;
	BOOL pk_fres;

	pk_fres = ERROR;
	if ((pkfndr==NULL)||(pRbuf==NULL))
	{
		return ERROR;
	}
	if ((parseBuf==NULL)||(chkExec==NULL))
	{
		return ERROR;
	}

	datLen=rBufGet(pRbuf,parseBuf,parseBufLen);/*将整个环形缓存中的数据取出*/
	fndIndex=0;
	while ((fndIndex+pkfndr->headLen)<=datLen)/*至少headLen字节*/			
	{				
		findRslt=pkfinderFind(pkfndr,&parseBuf[fndIndex],datLen-fndIndex,&pkPos,&pkLen);
		if (findRslt==0)/*未找到包头*/
		{
			fndIndex=datLen-pkfndr->headLen;/*保留包头的长度,前面的删除*/
			break;					
		}
		else if (findRslt==1)/*找到包头但长度不足*/
		{
			if (pkLen>parseBufLen)/*包长度异常*/
			{
				fndIndex+=pkPos+pkfndr->headLen;/*删除包头前面的数据和包头 避免异常包一直在*/
			}
			else
			{
				fndIndex+=pkPos;/*删除包头前面的数据*/
			}
			break;
		}
		else if (findRslt==2)/*找到完整包*/
		{
			f=chkExec(&parseBuf[fndIndex+pkPos],pkLen);/*对完整包进行校验并执行*/
			if (f==TRUE)/*校验正确或包尾正确*/
			{
				pk_fres = OK;
				fndIndex += pkPos+pkLen;/*删除整包以及包前面的数据.继续找包.*/
			}
			else/*校验错误或包尾错误 说明找到的残包与下一包合并了*/
			{
				fndIndex += pkPos+pkfndr->headLen;/*删除包头.继续找包.*/
			}
		}
		else
		{
			return ERROR;
		}
	}
	rBufRemove(pRbuf,fndIndex);/*删除数据*/
	return pk_fres;
}
