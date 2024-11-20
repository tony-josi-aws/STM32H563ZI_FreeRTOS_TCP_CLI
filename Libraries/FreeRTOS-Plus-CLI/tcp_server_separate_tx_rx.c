/* Standard includes. */
#include <stdint.h>
#include <stdio.h>

/* FreeRTOS includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

/* FreeRTOS+TCP includes. */
#include "FreeRTOS_IP.h"
#include "FreeRTOS_Sockets.h"

    QueueHandle_t xTxRxQueue = NULL;

    Socket_t xConnectedSocket = NULL;

/* The maximum time to wait for a closing socket to close. */
    #define tcpechoSHUTDOWN_DELAY    ( pdMS_TO_TICKS( 5000 ) )

    #define TCP_TX_RX_QUEUE_LENGTH    ( 30 )

/* The standard echo port number. */
    #define tcpechoPORT_NUMBER       7

/* If ipconfigUSE_TCP_WIN is 1 then the Tx sockets will use a buffer size set by
 * ipconfigTCP_TX_BUFFER_LENGTH, and the Tx window size will be
 * configECHO_SERVER_TX_WINDOW_SIZE times the buffer size.  Note
 * ipconfigTCP_TX_BUFFER_LENGTH is set in FreeRTOSIPConfig.h as it is a standard TCP/IP
 * stack constant, whereas configECHO_SERVER_TX_WINDOW_SIZE is set in
 * FreeRTOSConfig.h as it is a demo application constant. */
    #ifndef configECHO_SERVER_TX_WINDOW_SIZE
        #define configECHO_SERVER_TX_WINDOW_SIZE    2
    #endif

/* If ipconfigUSE_TCP_WIN is 1 then the Rx sockets will use a buffer size set by
 * ipconfigTCP_RX_BUFFER_LENGTH, and the Rx window size will be
 * configECHO_SERVER_RX_WINDOW_SIZE times the buffer size.  Note
 * ipconfigTCP_RX_BUFFER_LENGTH is set in FreeRTOSIPConfig.h as it is a standard TCP/IP
 * stack constant, whereas configECHO_SERVER_RX_WINDOW_SIZE is set in
 * FreeRTOSConfig.h as it is a demo application constant. */
    #ifndef configECHO_SERVER_RX_WINDOW_SIZE
        #define configECHO_SERVER_RX_WINDOW_SIZE    2
    #endif

static void prvConnectionListeningTask( void * pvParameters );
static void prvConnectionTransmittingTask( void * pvParameters );
    
static void prvConnectionListeningTask( void * pvParameters )
{
    struct freertos_sockaddr xClient, xBindAddress;
    Socket_t xListeningSocket;
    socklen_t xSize = sizeof( xClient );
    static const TickType_t xReceiveTimeOut = portMAX_DELAY;
    const BaseType_t xBacklog = 1;
    const BaseType_t xReuseSocket = 1;
    TickType_t xTimeOnShutdown;

    uint8_t pucRxBuffer[ipconfigTCP_MSS ];

    static StaticQueue_t xTxRxStaticQueue;
    static uint8_t ucTxRxStaticQueueStorageArea[ TCP_TX_RX_QUEUE_LENGTH * sizeof( uint32_t ) ];
    xTxRxQueue = xQueueCreateStatic( TCP_TX_RX_QUEUE_LENGTH,
                                                sizeof( uint32_t ),
                                                ucTxRxStaticQueueStorageArea,
                                                &xTxRxStaticQueue );

    do
    {
        uint32_t rxCount = 0;
        int32_t lBytes;



        #if ( ipconfigUSE_TCP_WIN == 1 )
            WinProperties_t xWinProps;

            /* Fill in the buffer and window sizes that will be used by the socket. */
            xWinProps.lTxBufSize = ipconfigTCP_TX_BUFFER_LENGTH;
            xWinProps.lTxWinSize = configECHO_SERVER_TX_WINDOW_SIZE;
            xWinProps.lRxBufSize = ipconfigTCP_RX_BUFFER_LENGTH;
            xWinProps.lRxWinSize = configECHO_SERVER_RX_WINDOW_SIZE;
        #endif /* ipconfigUSE_TCP_WIN */

        /* Just to prevent compiler warnings. */
        ( void ) pvParameters;

        /* Attempt to open the socket. */
        xListeningSocket = FreeRTOS_socket( FREERTOS_AF_INET, FREERTOS_SOCK_STREAM, FREERTOS_IPPROTO_TCP );
        configASSERT( xListeningSocket != FREERTOS_INVALID_SOCKET );

        /* Set a time out so accept() will just wait for a connection. */
        FreeRTOS_setsockopt( xListeningSocket, 0, FREERTOS_SO_RCVTIMEO, &xReceiveTimeOut, sizeof( xReceiveTimeOut ) );

        /* Set the window and buffer sizes. */
        #if ( ipconfigUSE_TCP_WIN == 1 )
        {
            FreeRTOS_setsockopt( xListeningSocket, 0, FREERTOS_SO_WIN_PROPERTIES, ( void * ) &xWinProps, sizeof( xWinProps ) );
        }
        #endif /* ipconfigUSE_TCP_WIN */

        /* Reuse socket */
        FreeRTOS_setsockopt( xListeningSocket, 0, FREERTOS_SO_REUSE_LISTEN_SOCKET, ( void * ) &xReuseSocket, sizeof( xReuseSocket ) );

        /* Bind the socket to the port that the client task will send to, then
            * listen for incoming connections. */
        xBindAddress.sin_port = tcpechoPORT_NUMBER;
        xBindAddress.sin_port = FreeRTOS_htons( xBindAddress.sin_port );
        xBindAddress.sin_family = FREERTOS_AF_INET;
        FreeRTOS_bind( xListeningSocket, &xBindAddress, sizeof( xBindAddress ) );
        FreeRTOS_listen( xListeningSocket, xBacklog );

        /* Wait for a client to connect. */
        xConnectedSocket = FreeRTOS_accept( xListeningSocket, &xClient, &xSize );

        FreeRTOS_printf(("Connected to client\r\n"));

        configASSERT( xConnectedSocket != FREERTOS_INVALID_SOCKET );

        FreeRTOS_setsockopt( xConnectedSocket, 0, FREERTOS_SO_RCVTIMEO, &xReceiveTimeOut, sizeof( xReceiveTimeOut ) );

        for( ; ; )
        {

            /* Do receive data */
            memset( pucRxBuffer, 0x00, ipconfigTCP_MSS );

            /* Receive data on the socket. */
            lBytes = FreeRTOS_recv( xConnectedSocket, pucRxBuffer, ipconfigTCP_MSS, 0 );


            if( lBytes >= 0 )
            {
                rxCount++;

                FreeRTOS_printf(("Received data %lu\r\n", rxCount));

                xQueueSend(xTxRxQueue, &rxCount, portMAX_DELAY );
            }
            else
            {
                /* Socket closed? */
                FreeRTOS_printf(("Receive failed break\r\n"));
                break;
            }

        }

        /* Initiate a shutdown in case it has not already been initiated. */
        FreeRTOS_shutdown( xConnectedSocket, FREERTOS_SHUT_RDWR );

        /* Wait for the shutdown to take effect, indicated by FreeRTOS_recv()
         * returning an error. */
        xTimeOnShutdown = xTaskGetTickCount();

        do
        {
            if( FreeRTOS_recv( xConnectedSocket, pucRxBuffer, ipconfigTCP_MSS, 0 ) < 0 )
            {
                break;
            }
        } while( ( xTaskGetTickCount() - xTimeOnShutdown ) < tcpechoSHUTDOWN_DELAY );


        FreeRTOS_closesocket( xConnectedSocket );

        FreeRTOS_printf(("Socket closed.. Retrying!\r\n"));


    } while (pdTRUE);
}


static void prvConnectionTransmittingTask( void * pvParameters )
{
    uint32_t rxCount = 0;

    do 
    
    {

        while( ( xTxRxQueue != NULL) && (xQueueReceive( xTxRxQueue, ( void * ) &rxCount, portMAX_DELAY )) )
        {
            uint8_t ucTransmitBuffer[128];
            int32_t lBytes, lSent, lTotalSent;

            snprintf((char *)ucTransmitBuffer, sizeof(ucTransmitBuffer), "FreeRTOS+TCP Hello, World %lu", rxCount);

            lSent = 0;
            lTotalSent = 0;
            lBytes = strlen((char *)ucTransmitBuffer);

            FreeRTOS_printf(("Send queue unblocks\r\n"));

            /* Call send() until all the data has been sent. */
            while( ( lSent >= 0 ) && ( lTotalSent < lBytes ) )
            {
                lSent = FreeRTOS_send( xConnectedSocket, ucTransmitBuffer, lBytes - lTotalSent, 0 );
                FreeRTOS_printf(("Send done\r\n"));
                if(rxCount == 10)
                {
                    lTotalSent += lSent;
                }
                lTotalSent += lSent;
            }

            if( lSent < 0 )
            {
                /* Socket closed? */
                FreeRTOS_printf(("Send failed!!!!!!!!!!!!!!!!!!!!!!\r\n"));
                break;
            }

        }

        vTaskDelay(pdMS_TO_TICKS(100));

    } while (pdTRUE);

}


void startTCPServerReuseTxRx_Tasks( uint16_t usTaskStackSize, UBaseType_t uxTaskPriority )
{

    xTaskCreate( 	prvConnectionListeningTask,	/* The function that implements the task. */
                    "Echo0",			/* Just a text name for the task to aid debugging. */
                    usTaskStackSize,	/* The stack size is defined in FreeRTOSIPConfig.h. */
                    NULL,		/* The task parameter, not used in this case. */
                    uxTaskPriority,		/* The priority assigned to the task is defined in FreeRTOSConfig.h. */
                    NULL );				/* The task handle is not used. */

    xTaskCreate( 	prvConnectionTransmittingTask,	/* The function that implements the task. */
                    "Echo0",			/* Just a text name for the task to aid debugging. */
                    usTaskStackSize,	/* The stack size is defined in FreeRTOSIPConfig.h. */
                    NULL,		/* The task parameter, not used in this case. */
                    uxTaskPriority,		/* The priority assigned to the task is defined in FreeRTOSConfig.h. */
                    NULL );				/* The task handle is not used. */

}
