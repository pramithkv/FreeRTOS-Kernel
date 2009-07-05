/*
	FreeRTOS.org V5.3.1 - Copyright (C) 2003-2009 Richard Barry.

	This file is part of the FreeRTOS.org distribution.

	FreeRTOS.org is free software; you can redistribute it and/or modify it
	under the terms of the GNU General Public License (version 2) as published
	by the Free Software Foundation and modified by the FreeRTOS exception.
	**NOTE** The exception to the GPL is included to allow you to distribute a
	combined work that includes FreeRTOS.org without being obliged to provide
	the source code for any proprietary components.  Alternative commercial
	license and support terms are also available upon request.  See the
	licensing section of http://www.FreeRTOS.org for full details.

	FreeRTOS.org is distributed in the hope that it will be useful,	but WITHOUT
	ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
	FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
	more details.

	You should have received a copy of the GNU General Public License along
	with FreeRTOS.org; if not, write to the Free Software Foundation, Inc., 59
	Temple Place, Suite 330, Boston, MA  02111-1307  USA.


	***************************************************************************
	*                                                                         *
	* Get the FreeRTOS eBook!  See http://www.FreeRTOS.org/Documentation      *
	*                                                                         *
	* This is a concise, step by step, 'hands on' guide that describes both   *
	* general multitasking concepts and FreeRTOS specifics. It presents and   *
	* explains numerous examples that are written using the FreeRTOS API.     *
	* Full source code for all the examples is provided in an accompanying    *
	* .zip file.                                                              *
	*                                                                         *
	***************************************************************************

	1 tab == 4 spaces!

	Please ensure to read the configuration and relevant port sections of the
	online documentation.

	http://www.FreeRTOS.org - Documentation, latest information, license and
	contact details.

	http://www.SafeRTOS.com - A version that is certified for use in safety
	critical systems.

	http://www.OpenRTOS.com - Commercial support, development, porting,
	licensing and training services.
*/

/* Originally adapted from file written by Andreas Dannenberg.  Supplied with permission. */

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* Hardware specific includes. */
#include "LPC17xx_defs.h"
#include "EthDev_LPC17xx.h"

/* Time to wait between each inspection of the link status. */
#define emacWAIT_FOR_LINK_TO_ESTABLISH ( 500 / portTICK_RATE_MS )

/* Short delay used in several places during the initialisation process. */
#define emacSHORT_DELAY				   ( 2 )

/* Hardware specific bit definitions. */
#define emacLINK_ESTABLISHED		( 0x0001 )
#define emacFULL_DUPLEX_ENABLED		( 0x0004 )
#define emac10BASE_T_MODE			( 0x0002 )
#define emacPINSEL2_VALUE 0x50150105

/* If no buffers are available, then wait this long before looking again.... */
#define emacBUFFER_WAIT_DELAY	( 3 / portTICK_RATE_MS )

/* ...and don't look more than this many times. */
#define emacBUFFER_WAIT_ATTEMPTS	( 30 )

/* Index to the Tx descriptor that is always used first for every Tx.  The second
descriptor is then used to re-send in order to speed up the uIP Tx process. */
#define emacTX_DESC_INDEX			( 0 )

/*-----------------------------------------------------------*/

/*
 * Configure both the Rx and Tx descriptors during the init process.
 */
static void prvInitDescriptors( void );

/*
 * Setup the IO and peripherals required for Ethernet communication.
 */
static void prvSetupEMACHardware( void );

/*
 * Control the auto negotiate process.
 */
static void prvConfigurePHY( void );

/*
 * Wait for a link to be established, then setup the PHY according to the link
 * parameters.
 */
static long prvSetupLinkStatus( void );

/*
 * Search the pool of buffers to find one that is free.  If a buffer is found
 * mark it as in use before returning its address.
 */
static unsigned char *prvGetNextBuffer( void );

/*
 * Return an allocated buffer to the pool of free buffers.
 */
static void prvReturnBuffer( unsigned char *pucBuffer );

/*
 * Send lValue to the lPhyReg within the PHY.
 */
static long prvWritePHY( long lPhyReg, long lValue );

/*
 * Read a value from ucPhyReg within the PHY.  *plStatus will be set to
 * pdFALSE if there is an error.
 */
static unsigned short prvReadPHY( unsigned char ucPhyReg, long *plStatus );

/*-----------------------------------------------------------*/

/* The semaphore used to wake the uIP task when data arrives. */
extern xSemaphoreHandle xEMACSemaphore;

/* Each ucBufferInUse index corresponds to a position in the pool of buffers.
If the index contains a 1 then the buffer within pool is in use, if it
contains a 0 then the buffer is free. */
static unsigned char ucBufferInUse[ ETH_NUM_BUFFERS ] = { pdFALSE };

/* The uip_buffer is not a fixed array, but instead gets pointed to the buffers
allocated within this file. */
unsigned char * uip_buf;

/* Store the length of the data being sent so the data can be sent twice.  The
value will be set back to 0 once the data has been sent twice. */
static unsigned short usSendLen = 0;

/*-----------------------------------------------------------*/

long lEMACInit( void )
{
long lReturn = pdPASS;
volatile unsigned long regv, tout;
unsigned long ulID1, ulID2;

	/* Reset peripherals, configure port pins and registers. */
	prvSetupEMACHardware();

	/* Check the PHY part number is as expected. */
	ulID1 = prvReadPHY( PHY_REG_IDR1, &lReturn );
	ulID2 = prvReadPHY( PHY_REG_IDR2, &lReturn );
	if( ( (ulID1 << 16UL ) | ( ulID2 & 0xFFF0UL ) ) == DP83848C_ID )
	{
		/* Set the Ethernet MAC Address registers */
		MAC_SA0 = ( configMAC_ADDR0 << 8 ) | configMAC_ADDR1;
		MAC_SA1 = ( configMAC_ADDR2 << 8 ) | configMAC_ADDR3;
		MAC_SA2 = ( configMAC_ADDR4 << 8 ) | configMAC_ADDR5;

		/* Initialize Tx and Rx DMA Descriptors */
		prvInitDescriptors();

		/* Receive broadcast and perfect match packets */
		MAC_RXFILTERCTRL = RFC_UCAST_EN | RFC_BCAST_EN | RFC_PERFECT_EN;

		/* Setup the PHY. */
		prvConfigurePHY();
	}
	else
	{
		lReturn = pdFAIL;
	}

	/* Check the link status. */
	if( lReturn == pdPASS )
	{
		lReturn = prvSetupLinkStatus();
	}

	if( lReturn == pdPASS )
	{
		/* Initialise uip_buf to ensure it points somewhere valid. */
		uip_buf = prvGetNextBuffer();

		/* Reset all interrupts */
		MAC_INTCLEAR = ( INT_RX_OVERRUN | INT_RX_ERR | INT_RX_FIN | INT_RX_DONE | INT_TX_UNDERRUN | INT_TX_ERR | INT_TX_FIN | INT_TX_DONE | INT_SOFT_INT | INT_WAKEUP );

		/* Enable receive and transmit mode of MAC Ethernet core */
		MAC_COMMAND |= ( CR_RX_EN | CR_TX_EN );
		MAC_MAC1 |= MAC1_REC_EN;
	}

	return lReturn;
}
/*-----------------------------------------------------------*/

static unsigned char *prvGetNextBuffer( void )
{
long x;
unsigned char *pucReturn = NULL;
unsigned long ulAttempts = 0;

	while( pucReturn == NULL )
	{
		/* Look through the buffers to find one that is not in use by
		anything else. */
		for( x = 0; x < ETH_NUM_BUFFERS; x++ )
		{
			if( ucBufferInUse[ x ] == pdFALSE )
			{
				ucBufferInUse[ x ] = pdTRUE;
				pucReturn = ( unsigned char * ) ETH_BUF( x );
				break;
			}
		}

		/* Was a buffer found? */
		if( pucReturn == NULL )
		{
			ulAttempts++;

			if( ulAttempts >= emacBUFFER_WAIT_ATTEMPTS )
			{
				break;
			}

			/* Wait then look again. */
			vTaskDelay( emacBUFFER_WAIT_DELAY );
		}
	}

	return pucReturn;
}
/*-----------------------------------------------------------*/

static void prvInitDescriptors( void )
{
long x, lNextBuffer = 0;

	for( x = 0; x < NUM_RX_FRAG; x++ )
	{
		/* Allocate the next Ethernet buffer to this descriptor. */
		RX_DESC_PACKET( x ) = ETH_BUF( lNextBuffer );
		RX_DESC_CTRL( x ) = RCTRL_INT | ( ETH_FRAG_SIZE - 1 );
		RX_STAT_INFO( x ) = 0;
		RX_STAT_HASHCRC( x ) = 0;

		/* The Ethernet buffer is now in use. */
		ucBufferInUse[ lNextBuffer ] = pdTRUE;
		lNextBuffer++;
	}

	/* Set EMAC Receive Descriptor Registers. */
	MAC_RXDESCRIPTOR = RX_DESC_BASE;
	MAC_RXSTATUS = RX_STAT_BASE;
	MAC_RXDESCRIPTORNUM = NUM_RX_FRAG - 1;

	/* Rx Descriptors Point to 0 */
	MAC_RXCONSUMEINDEX = 0;

	/* A buffer is not allocated to the Tx descriptors until they are actually
	used. */
	for( x = 0; x < NUM_TX_FRAG; x++ )
	{
		TX_DESC_PACKET( x ) = NULL;
		TX_DESC_CTRL( x ) = 0;
		TX_STAT_INFO( x ) = 0;
	}

	/* Set EMAC Transmit Descriptor Registers. */
	MAC_TXDESCRIPTOR = TX_DESC_BASE;
	MAC_TXSTATUS = TX_STAT_BASE;
	MAC_TXDESCRIPTORNUM = NUM_TX_FRAG - 1;

	/* Tx Descriptors Point to 0 */
	MAC_TXPRODUCEINDEX = 0;
}
/*-----------------------------------------------------------*/

static void prvSetupEMACHardware( void )
{
unsigned short us;
long x, lDummy;

	/* Enable P1 Ethernet Pins. */
	PINSEL2 = emacPINSEL2_VALUE;
	PINSEL3 = ( PINSEL3 & ~0x0000000F ) | 0x00000005;

	/* Power Up the EMAC controller. */
	PCONP |= PCONP_PCENET;
	vTaskDelay( emacSHORT_DELAY );

	/* Reset all EMAC internal modules. */
	MAC_MAC1 = MAC1_RES_TX | MAC1_RES_MCS_TX | MAC1_RES_RX | MAC1_RES_MCS_RX | MAC1_SIM_RES | MAC1_SOFT_RES;
	MAC_COMMAND = CR_REG_RES | CR_TX_RES | CR_RX_RES | CR_PASS_RUNT_FRM;

	/* A short delay after reset. */
	vTaskDelay( emacSHORT_DELAY );

	/* Initialize MAC control registers. */
	MAC_MAC1 = MAC1_PASS_ALL;
	MAC_MAC2 = MAC2_CRC_EN | MAC2_PAD_EN;
	MAC_MAXF = ETH_MAX_FLEN;
	MAC_CLRT = CLRT_DEF;
	MAC_IPGR = IPGR_DEF;

	/* Enable Reduced MII interface. */
	MAC_COMMAND = CR_RMII | CR_PASS_RUNT_FRM;

	/* Reset Reduced MII Logic. */
	MAC_SUPP = SUPP_RES_RMII;
	vTaskDelay( emacSHORT_DELAY );
	MAC_SUPP = 0;

	/* Put the PHY in reset mode */
	prvWritePHY( PHY_REG_BMCR, MCFG_RES_MII );
	prvWritePHY( PHY_REG_BMCR, MCFG_RES_MII );

	/* Wait for hardware reset to end. */
	for( x = 0; x < 100; x++ )
	{
		vTaskDelay( emacSHORT_DELAY * 5 );
		us = prvReadPHY( PHY_REG_BMCR, &lDummy );
		if( !( us & MCFG_RES_MII ) )
		{
			/* Reset complete */
			break;
		}
	}
}
/*-----------------------------------------------------------*/

static void prvConfigurePHY( void )
{
unsigned short us;
long x, lDummy;

	/* Auto negotiate the configuration. */
	if( prvWritePHY( PHY_REG_BMCR, PHY_AUTO_NEG ) )
	{
		vTaskDelay( emacSHORT_DELAY * 5 );

		for( x = 0; x < 10; x++ )
		{
			us = prvReadPHY( PHY_REG_BMSR, &lDummy );

			if( us & PHY_AUTO_NEG_COMPLETE )
			{
				break;
			}

			vTaskDelay( emacWAIT_FOR_LINK_TO_ESTABLISH );
		}
	}
}
/*-----------------------------------------------------------*/

static long prvSetupLinkStatus( void )
{
long lReturn = pdFAIL, x;
unsigned short usLinkStatus;

	/* Wait with timeout for the link to be established. */
	for( x = 0; x < 10; x++ )
	{
		usLinkStatus = prvReadPHY( PHY_REG_STS, &lReturn );
		if( usLinkStatus & emacLINK_ESTABLISHED )
		{
			/* Link is established. */
			lReturn = pdPASS;
			break;
		}

        vTaskDelay( emacWAIT_FOR_LINK_TO_ESTABLISH );
	}

	if( lReturn == pdPASS )
	{
		/* Configure Full/Half Duplex mode. */
		if( usLinkStatus & emacFULL_DUPLEX_ENABLED )
		{
			/* Full duplex is enabled. */
			MAC_MAC2 |= MAC2_FULL_DUP;
			MAC_COMMAND |= CR_FULL_DUP;
			MAC_IPGT = IPGT_FULL_DUP;
		}
		else
		{
			/* Half duplex mode. */
			MAC_IPGT = IPGT_HALF_DUP;
		}

		/* Configure 100MBit/10MBit mode. */
		if( usLinkStatus & emac10BASE_T_MODE )
		{
			/* 10MBit mode. */
			MAC_SUPP = 0;
		}
		else
		{
			/* 100MBit mode. */
			MAC_SUPP = SUPP_SPEED;
		}
	}

	return lReturn;
}
/*-----------------------------------------------------------*/

static void prvReturnBuffer( unsigned char *pucBuffer )
{
unsigned long ul;

	/* Mark a buffer as free for use. */
	for( ul = 0; ul < ETH_NUM_BUFFERS; ul++ )
	{
		if( ETH_BUF( ul ) == ( unsigned long ) pucBuffer )
		{
			ucBufferInUse[ ul ] = pdFALSE;
			break;
		}
	}
}
/*-----------------------------------------------------------*/

unsigned long ulGetEMACRxData( void )
{
unsigned long ulLen = 0;
long lIndex;

	if( MAC_RXPRODUCEINDEX != MAC_RXCONSUMEINDEX )
	{
		/* Mark the current buffer as free as uip_buf is going to be set to
		the buffer that contains the received data. */
		prvReturnBuffer( uip_buf );

		ulLen = ( RX_STAT_INFO( MAC_RXCONSUMEINDEX ) & RINFO_SIZE ) - 3;
		uip_buf = ( unsigned char * ) RX_DESC_PACKET( MAC_RXCONSUMEINDEX );

		/* Allocate a new buffer to the descriptor. */
        RX_DESC_PACKET( MAC_RXCONSUMEINDEX ) = ( unsigned long ) prvGetNextBuffer();

		/* Move the consume index onto the next position, ensuring it wraps to
		the beginning at the appropriate place. */
		lIndex = MAC_RXCONSUMEINDEX;

		lIndex++;
		if( lIndex >= NUM_RX_FRAG )
		{
			lIndex = 0;
		}

		MAC_RXCONSUMEINDEX = lIndex;
	}

	return ulLen;
}
/*-----------------------------------------------------------*/

void vSendEMACTxData( unsigned short usTxDataLen )
{
unsigned long ulAttempts = 0UL;

	/* Check to see if the Tx descriptor is free, indicated by its buffer being
	NULL. */
	while( TX_DESC_PACKET( emacTX_DESC_INDEX ) != NULL )
	{
		/* Wait for the Tx descriptor to become available. */
		vTaskDelay( emacBUFFER_WAIT_DELAY );

		ulAttempts++;
		if( ulAttempts > emacBUFFER_WAIT_ATTEMPTS )
		{
			/* Something has gone wrong as the Tx descriptor is still in use.
			Clear it down manually, the data it was sending will probably be
			lost. */
			prvReturnBuffer( ( unsigned char * ) TX_DESC_PACKET( emacTX_DESC_INDEX ) );
			break;
		}
	}

	/* Setup the Tx descriptor for transmission.  Remember the length of the
	data being sent so the second descriptor can be used to send it again from
	within the ISR. */
	usSendLen = usTxDataLen;
	TX_DESC_PACKET( emacTX_DESC_INDEX ) = ( unsigned long ) uip_buf;
	TX_DESC_CTRL( emacTX_DESC_INDEX ) = ( usTxDataLen | TCTRL_LAST | TCTRL_INT );
	MAC_TXPRODUCEINDEX = ( emacTX_DESC_INDEX + 1 );

	/* uip_buf is being sent by the Tx descriptor.  Allocate a new buffer. */
	uip_buf = prvGetNextBuffer();
}
/*-----------------------------------------------------------*/

static long prvWritePHY( long lPhyReg, long lValue )
{
const long lMaxTime = 10;
long x;

	MAC_MADR = DP83848C_DEF_ADR | lPhyReg;
	MAC_MWTD = lValue;

	x = 0;
	for( x = 0; x < lMaxTime; x++ )
	{
		if( ( MAC_MIND & MIND_BUSY ) == 0 )
		{
			/* Operation has finished. */
			break;
		}

		vTaskDelay( emacSHORT_DELAY );
	}

	if( x < lMaxTime )
	{
		return pdPASS;
	}
	else
	{
		return pdFAIL;
	}
}
/*-----------------------------------------------------------*/

static unsigned short prvReadPHY( unsigned char ucPhyReg, long *plStatus )
{
long x;
const long lMaxTime = 10;

	MAC_MADR = DP83848C_DEF_ADR | ucPhyReg;
	MAC_MCMD = MCMD_READ;

	for( x = 0; x < lMaxTime; x++ )
	{
		/* Operation has finished. */
		if( ( MAC_MIND & MIND_BUSY ) == 0 )
		{
			break;
		}

		vTaskDelay( emacSHORT_DELAY );
	}

	MAC_MCMD = 0;

	if( x >= lMaxTime )
	{
		*plStatus = pdFAIL;
	}

	return( MAC_MRDD );
}
/*-----------------------------------------------------------*/

void vEMAC_ISR( void )
{
unsigned long ulStatus;
portBASE_TYPE	xHigherPriorityTaskWoken = pdFALSE;

	ulStatus = MAC_INTSTATUS;

	/* Clear the interrupt. */
	MAC_INTCLEAR = ulStatus;

	if( ulStatus & INT_RX_DONE )
	{
		/* Ensure the uIP task is not blocked as data has arrived. */
		xSemaphoreGiveFromISR( xEMACSemaphore, &xHigherPriorityTaskWoken );
	}

	if( ulStatus & INT_TX_DONE )
	{
		if( usSendLen > 0 )
		{
			/* Send the data again, using the second descriptor.  As there are
			only two descriptors the index is set back to 0. */
			TX_DESC_PACKET( ( emacTX_DESC_INDEX + 1 ) ) = TX_DESC_PACKET( emacTX_DESC_INDEX );
			TX_DESC_CTRL( ( emacTX_DESC_INDEX + 1 ) ) = ( usSendLen | TCTRL_LAST | TCTRL_INT );
			MAC_TXPRODUCEINDEX = ( emacTX_DESC_INDEX );

			/* This is the second Tx so set usSendLen to 0 to indicate that the
			Tx descriptors will be free again. */
			usSendLen = 0UL;
		}
		else
		{
			/* The Tx buffer is no longer required. */
			prvReturnBuffer( ( unsigned char * ) TX_DESC_PACKET( emacTX_DESC_INDEX ) );
            TX_DESC_PACKET( emacTX_DESC_INDEX ) = NULL;
		}
	}

	portEND_SWITCHING_ISR( xHigherPriorityTaskWoken );
}