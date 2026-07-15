//////////////////////////////////////////////////////////////////////////  
///    COPYRIGHT NOTICE  
///    Copyright (c) 2024,513  
///    All rights reserved.  
///  
/// @file   rBuf.c  
/// @brief  
///  
///  环形缓存功能.
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
#include "rBuf.h"


UINT rBufPut( RBUF* pRbuf,UCHAR* buf,UINT len )
{
	unsigned int i;
	UINT retLen=0;
	UINT rests=0;

	if ((pRbuf==NULL)||(buf==NULL))
	{
		return 0;
	}

	if (pRbuf->outIndex > pRbuf->inIndex)/*如果翻转了,则最多放到outIndex之前(避免套圈)*/
	{
		retLen = min (len, pRbuf->outIndex - pRbuf->inIndex - 1);
		for (i=0;i<retLen;++i)
		{
			pRbuf->buf[pRbuf->inIndex+i]=buf[i];
		}
		pRbuf->inIndex+=retLen;
	}
    else if (pRbuf->outIndex == 0)/*如果出指针在起始,则最多可放到结束之前(避免套圈).*/
	{
		retLen = min (len, pRbuf->len - pRbuf->inIndex - 1);
		for (i=0;i<retLen;++i)
		{
			pRbuf->buf[pRbuf->inIndex+i]=buf[i];
		}
		pRbuf->inIndex+=retLen;
	}
    else/*如果未翻转,则需要分两次拷贝.*/
	{
		retLen = min (len, pRbuf->len - pRbuf->inIndex);
		for (i=0;i<retLen;++i)
		{
			pRbuf->buf[pRbuf->inIndex+i]=buf[i];
		}
		if (retLen<len)/*是否还需要拷贝*/
		{
			rests = min (len-retLen, pRbuf->outIndex-1);
			for (i=0;i<rests;++i)
			{
				pRbuf->buf[i]=buf[retLen+i];
			}
			retLen+=rests;
			pRbuf->inIndex=rests;			
		}
		else
		{
			pRbuf->inIndex+=retLen;
			if (pRbuf->inIndex==pRbuf->len)
			{
				pRbuf->inIndex=0;
			}
		}	
	}
    return retLen;
}

UINT rBufGet( RBUF* pRbuf,UCHAR* buf,UINT len )
{
	unsigned int i;
	UINT retLen=0;
	UINT rests=0;

	if ((pRbuf==NULL)||(buf==NULL))
	{
		return 0;
	}

	if (pRbuf->inIndex>=pRbuf->outIndex)/*未翻转*/
	{
		retLen=min(len, pRbuf->inIndex - pRbuf->outIndex);
		for (i=0;i<retLen;++i)
		{
			buf[i]=pRbuf->buf[pRbuf->outIndex+i];
		}				
	}
	else/*翻转*/
	{
		retLen=min(len, pRbuf->len - pRbuf->outIndex);
		for (i=0;i<retLen;++i)/*拷贝前一部分*/
		{
			buf[i]=pRbuf->buf[pRbuf->outIndex+i];
		}
		if (retLen<len)/*是否还需要拷贝*/
		{
			rests=min(len-retLen,pRbuf->inIndex);
			for (i=0;i<rests;++i)/*拷贝剩余部分*/
			{
				buf[retLen+i]=pRbuf->buf[i];
			}
			retLen+=rests;
		}		
	}
	return retLen;
}

UINT rBufRemove( RBUF* pRbuf,UINT len )
{
	int n;
	UINT retLen;
	UINT added;	

	if (pRbuf==NULL)
	{
		return 0;
	}

	n = (int)(pRbuf->inIndex)-(int)(pRbuf->outIndex);
	if (n<0)
	{
		n+=pRbuf->len;
	}
	retLen=min(len, n);

	added=pRbuf->outIndex+retLen;
	if (added>=pRbuf->len)
	{
		added=added-pRbuf->len;
	}
	pRbuf->outIndex=added;

	return retLen;
}

UINT rBufNBytes( RBUF* pRbuf )
{
	int n = (int)(pRbuf->inIndex)-(int)(pRbuf->outIndex);

	if (n<0)
	{
		n+=pRbuf->len;
	}
	return (UINT)n;
}

BOOL rBufIsFull( RBUF* pRbuf )
{
	int n = (int)(pRbuf->inIndex)-(int)(pRbuf->outIndex)+1;

	if((n == 0) || (n == pRbuf->len))
	{
		return TRUE;
	}
	return FALSE;
}

void rBufClear( RBUF* pRbuf )
{
	pRbuf->inIndex=0;
	pRbuf->outIndex=0;
}

void rBufInit( RBUF* pRbuf )
{
	pRbuf->inIndex=0;
	pRbuf->outIndex=0;
}

