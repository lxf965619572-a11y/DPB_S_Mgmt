//////////////////////////////////////////////////////////////////////////  
///    COPYRIGHT NOTICE  
///    Copyright (c) 2015,513  
///    All rights reserved.  
///  
/// @file   rng.c 
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
#include "rng.h"

UCHAR* rngGetWriteBuf( RNG* pRng )
{
	return &(pRng->buf[pRng->inIndex * pRng->msgLen]);
}

void rngWriteDone( RNG* pRng )
{
	UINT added=pRng->inIndex+1;

	if (added>=pRng->maxMsgs)
	{
		added=0;
	}
	if ( added == pRng->outIndex )/*避免套圈.如果生产者太快则覆盖*/
	{
		return;
	}
	pRng->inIndex=added;
}

UCHAR* rngGetReadBuf( RNG* pRng )
{
	return 	&(pRng->buf[pRng->outIndex * pRng->msgLen]);
}

void rngReadDone( RNG* pRng )
{
	UINT added=pRng->outIndex+1;

	if (added>=pRng->maxMsgs)
	{
		added=0;
	}
	pRng->outIndex=added;
}

UINT rngGetNMsgs( RNG* pRng )
{
	int n = (int)(pRng->inIndex) - (int)(pRng->outIndex);

	if (n<0)
	{
		n+=pRng->maxMsgs;
	}
	return (UINT)n;
}

void rngClear( RNG* pRng )
{
	pRng->inIndex=0;
	pRng->outIndex=0;
}

void rngInit( RNG* pRng )
{
	rngClear(pRng);
}

BOOL rngIsFul( RNG* pRng )
{
	UINT added=pRng->inIndex+1;

	if (added>=pRng->maxMsgs)
	{
		added=0;
	}
	if ( added == pRng->outIndex )/*已满*/
	{
		return TRUE;
	}
	return FALSE;
}


/*=====================================乒乓操作接口==================================================*/

UCHAR* pingpGetWriteBuf( PINGP* pPingp )
{
	return &(pPingp->buf[pPingp->inIndex * pPingp->msgLen]);
}

UCHAR* pingpGetReadBuf( PINGP* pPingp )
{
	return 	&(pPingp->buf[pPingp->outIndex * pPingp->msgLen]);
}

void pingpSwap( PINGP* pPingp )
{
	if (pPingp->inIndex==0)/*如果入指针在前*/
	{
		pPingp->inIndex=1;
		pPingp->outIndex=0;
	} 
	else/*如果入指针在后*/
	{
		pPingp->inIndex=0;
		pPingp->outIndex=1;
	}
}

void pingpInit( PINGP* pPingp )
{
	UINT i=0;
	UINT len=0;

	pPingp->inIndex=0;
	pPingp->outIndex=1;
	pPingp->maxMsgs=2;

	len=pPingp->msgLen*2;
	for (i=0;i<len;++i)
	{
		pPingp->buf[i]=0;
	}
}
