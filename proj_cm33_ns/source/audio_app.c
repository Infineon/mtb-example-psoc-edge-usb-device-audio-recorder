/*****************************************************************************
* File Name        : audio_app.c
*
* Description      : This file contains the implementation of adding 
*                    audio interface and main audio process task.
*
* Related Document : See README.md
*
*******************************************************************************
* Copyright 2025, Cypress Semiconductor Corporation (an Infineon company) or
* an affiliate of Cypress Semiconductor Corporation.  All rights reserved.
*
* This software, including source code, documentation and related
* materials ("Software") is owned by Cypress Semiconductor Corporation
* or one of its affiliates ("Cypress") and is protected by and subject to
* worldwide patent protection (United States and foreign),
* United States copyright laws and international treaty provisions.
* Therefore, you may use this Software only as provided in the license
* agreement accompanying the software package from which you
* obtained this Software ("EULA").
* If no EULA applies, Cypress hereby grants you a personal, non-exclusive,
* non-transferable license to copy, modify, and compile the Software
* source code solely for use in connection with Cypress's
* integrated circuit products.  Any reproduction, modification, translation,
* compilation, or representation of this Software except as specified
* above is prohibited without the express written permission of Cypress.
*
* Disclaimer: THIS SOFTWARE IS PROVIDED AS-IS, WITH NO WARRANTY OF ANY KIND,
* EXPRESS OR IMPLIED, INCLUDING, BUT NOT LIMITED TO, NONINFRINGEMENT, IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. Cypress
* reserves the right to make changes to the Software without notice. Cypress
* does not assume any liability arising out of the application or use of the
* Software or any product or circuit described in the Software. Cypress does
* not authorize its products for use in any products where a malfunction or
* failure of the Cypress product may reasonably be expected to result in
* significant property damage, injury or death ("High Risk Product"). By
* including Cypress's product in a High Risk Product, the manufacturer
* of such system or application assumes all risk of such use and in doing
* so agrees to indemnify Cypress against all liability.
*******************************************************************************/
#include "audio_app.h"
#include "audio_in.h"
#include "audio.h"
#include "emusbdev_audio_config.h"
#include "retarget_io_init.h"
#include "rtos.h"


/*******************************************************************************
* Macros
*******************************************************************************/
/* Polling interval for the endpoint in units of 125us (8*125 = 1ms) */
#define EP_IN_INTERVAL               (8u)

#define TASK_DELAY_MS                (50u)
#define ONE_BYTE                     (1u)
#define THREE_BYTES                  (3u)
#define USB_DELAY_MS                 (50u)
#define USB_SUSPENDED                (0u)
#define USB_CONNECTED                (1u)

#define RESET_VAL                    (0u)
#define DEFAULT_RET_VAL              (1u)
#define BYTE_MASK                    (0xFF)

#if ((AUDIO_IN_SAMPLE_FREQ == AUDIO_SAMPLING_RATE_16KHZ) || \
     (AUDIO_IN_SAMPLE_FREQ == AUDIO_SAMPLING_RATE_32KHZ) || \
     (AUDIO_IN_SAMPLE_FREQ == AUDIO_SAMPLING_RATE_48KHZ))
    #define DPLL_LP_FREQ                 (49152000ul)
    #define PDM_CLK_DIV_INT              (3u)
#elif((AUDIO_IN_SAMPLE_FREQ == AUDIO_SAMPLING_RATE_22KHZ) || \
      (AUDIO_IN_SAMPLE_FREQ == AUDIO_SAMPLING_RATE_44KHZ))
    #define DPLL_LP_FREQ                 (45158400ul)
    #define PDM_CLK_DIV_INT              (3u)
#endif

#define DPLL_DELAY_MS                (2000ul)

/*******************************************************************************
* Global Variables
********************************************************************************/
/* RTOS task handles */
TaskHandle_t rtos_audio_app_task;
TaskHandle_t rtos_audio_in_task;


/*******************************************************************************
* Static data
*******************************************************************************/
static USBD_AUDIO_HANDLE handle;
static USBD_AUDIO_INIT_DATA init_data;
static USBD_AUDIO_IF_CONF* microphone_config = (USBD_AUDIO_IF_CONF *) &audio_interfaces[0];
static uint8_t current_mic_format_index;


/*******************************************************************************
* Function Name: audio_control_callback
********************************************************************************
* Summary:
*  Callback called in ISR context.
*  Receives audio class control commands and sends appropriate responses
*  where necessary.
*
* Parameters:
*  pUserContext: User context which is passed to the callback.
*  Event: Audio event ID.
*  Unit: ID of the feature unit. In case of USB_AUDIO_PLAYBACK_*
*        and USB_AUDIO_RECORD_*: 0.
*  ControlSelector: ID of the control. In case of USB_AUDIO_PLAYBACK_* and
*                   USB_AUDIO_RECORD_*: 0.
*  pBuffer: In case of GET events: pointer to a buffer into which the
*           callback should write the reply. In case of SET events: pointer
*           to a buffer containing the command value. In case of
*           USB_AUDIO_PLAYBACK_* and USB_AUDIO_RECORD_*: NULL.
*  NumBytes: In case of GET events: requested size of the reply in bytes.
*            In case of SET events: number of bytes in pBuffer. In case
*            of USB_AUDIO_PLAYBACK_* and USB_AUDIO_RECORD_*: 0.
*  InterfaceNo: The number of the USB interface for which the event was issued.
*  AltSetting: The alternative setting number of the USB interface for
*              which the event was issued.
*
* Return:
*  int: =0 Audio command was handled by the callback, ≠ 0 Audio command was
*       not handled by the callback. The stack will STALL the request.
*
*******************************************************************************/
static int audio_control_callback(void *pUserContext,
                                  U8   Event,
                                  U8   Unit,
                                  U8   ControlSelector,
                                  U8   *pBuffer,
                                  U32  NumBytes,
                                  U8   InterfaceNo,
                                  U8   AltSetting)
{
    int retVal;

    CY_UNUSED_PARAMETER(pUserContext);
    CY_UNUSED_PARAMETER(InterfaceNo);

    retVal = 0;
    switch (Event)
    {
        case USB_AUDIO_RECORD_START:
            /* Host enabled reception */
            audio_in_enable();
            break;

        case USB_AUDIO_RECORD_STOP:
            /* Host disabled reception. Some hosts do not always send this! */
            audio_in_disable();
            break;

        case USB_AUDIO_PLAYBACK_START:
        case USB_AUDIO_PLAYBACK_STOP:
            break;

        case USB_AUDIO_SET_CUR:
            switch (ControlSelector)
            {
                case USB_AUDIO_MUTE_CONTROL:
                    if (ONE_BYTE == NumBytes) 
                    {
                        if (Unit == microphone_config->pUnits->FeatureUnitID) 
                        {
                            mic_mute = *pBuffer;
                        }
                    }
                    break;

                case USB_AUDIO_VOLUME_CONTROL:
                    break;

                case USB_AUDIO_SAMPLING_FREQ_CONTROL:
                    if (THREE_BYTES == NumBytes)
                    {
                        if (Unit == microphone_config->pUnits->FeatureUnitID)
                        {
                            if ((AltSetting) && (AltSetting < microphone_config->NumFormats))
                            {
                                current_mic_format_index = AltSetting-1;
                            }
                        }
                    }
                    break;

                default:
                    retVal = DEFAULT_RET_VAL;
                    break;
            }
            break;

        case USB_AUDIO_GET_CUR:
            switch (ControlSelector)
            {
                case USB_AUDIO_MUTE_CONTROL:
                    pBuffer[0] = RESET_VAL;
                    break;

                case USB_AUDIO_VOLUME_CONTROL:
                    pBuffer[0] = RESET_VAL;
                    pBuffer[1] = RESET_VAL;
                    break;

                case USB_AUDIO_SAMPLING_FREQ_CONTROL:
                    if (Unit == microphone_config->pUnits->FeatureUnitID)
                    {
                        pBuffer[0] = microphone_config->paFormats[current_mic_format_index].SamFreq & BYTE_MASK;
                        pBuffer[1] = (microphone_config->paFormats[current_mic_format_index].SamFreq >> 8) & BYTE_MASK;
                        pBuffer[2] = (microphone_config->paFormats[current_mic_format_index].SamFreq >> 16) & BYTE_MASK;
                    }
                    break;

                default:
                    pBuffer[0] = RESET_VAL;
                    pBuffer[1] = RESET_VAL;
                    break;
            }           
            break;

        case USB_AUDIO_SET_MIN:
        case USB_AUDIO_SET_MAX:
        case USB_AUDIO_SET_RES:
            break;

        case USB_AUDIO_GET_MIN:
            switch (ControlSelector)
            {
                case USB_AUDIO_VOLUME_CONTROL:
                    pBuffer[0] = AUDIO_VOLUME_MIN_MSB;
                    pBuffer[1] = AUDIO_VOLUME_MIN_LSB;
                    break;

                default:
                    pBuffer[0] = RESET_VAL;
                    pBuffer[1] = RESET_VAL;
                    break;
            }
            break;

        case USB_AUDIO_GET_MAX:
            switch (ControlSelector)
            {
                case USB_AUDIO_VOLUME_CONTROL:
                    pBuffer[0] = AUDIO_VOLUME_MAX_MSB;
                    pBuffer[1] = AUDIO_VOLUME_MAX_LSB;
                    break;

                default:
                    pBuffer[0] = RESET_VAL;
                    pBuffer[1] = RESET_VAL;
                    break;
            }
            break;

        case USB_AUDIO_GET_RES:
            switch (ControlSelector)
            {
                case USB_AUDIO_VOLUME_CONTROL:
                    pBuffer[0] = AUDIO_VOLUME_RES_MSB;
                    pBuffer[1] = AUDIO_VOLUME_RES_LSB;
                    break;

                default:
                    pBuffer[0] = RESET_VAL;
                    pBuffer[1] = RESET_VAL;
                    break;
            }
            break;
        
         default:
            retVal = DEFAULT_RET_VAL;
            break;
    }

    return retVal;
}

/*******************************************************************************
* Function Name: add_audio
********************************************************************************
* Summary:
*  Add a USB Audio interface to the USB stack.
*
* Parameters:
*  None
*
* Return:
*  USBD_AUDIO_HANDLE
*
*******************************************************************************/
static USBD_AUDIO_HANDLE add_audio(void)
{
    USB_ADD_EP_INFO       ep_in;
    USBD_AUDIO_HANDLE     handle;

    memset(&ep_in, RESET_VAL, sizeof(ep_in));
    memset(&init_data, RESET_VAL, sizeof(init_data));

    ep_in.MaxPacketSize               = MAX_AUDIO_IN_PACKET_SIZE_BYTES;       /* Max packet size for IN endpoint (in bytes) */
    ep_in.Interval                    = EP_IN_INTERVAL;                       /* Interval of 1 ms (8 * 125us) */
    ep_in.Flags                       = USB_ADD_EP_FLAG_USE_ISO_SYNC_TYPES;   /* Optional parameters */
    ep_in.InDir                       = USB_DIR_IN;                           /* IN direction (Device to Host) */
    ep_in.TransferType                = USB_TRANSFER_TYPE_ISO;                /* Endpoint type - Isochronous. */
    ep_in.ISO_Type                    = USB_ISO_SYNC_TYPE_ASYNCHRONOUS;       /* Async for isochronous endpoints */

    init_data.EPIn                   = USBD_AddEPEx(&ep_in, NULL, 0);
    init_data.EPOut                  = RESET_VAL;
    init_data.OutPacketSize          = RESET_VAL;
    init_data.pfOnOut                = NULL;
    init_data.pfOnIn                 = audio_in_endpoint_callback;
    init_data.pfOnControl            = audio_control_callback;
    init_data.pControlUserContext    = NULL;
    init_data.NumInterfaces          = SEGGER_COUNTOF(audio_interfaces);
    init_data.paInterfaces           = audio_interfaces;
    init_data.pOutUserContext        = NULL;
    init_data.pInUserContext         = NULL;

    handle = USBD_AUDIO_Add(&init_data);

    return handle;
}


/*******************************************************************************
* Function Name: app_clock_init
********************************************************************************
* Summary:
*  Setup clock tree for PDM-PCM block
*
* Parameters:
*  None
*
* Return:
*  void
*
*******************************************************************************/
void app_clock_init(void)
{
    uint32_t source_freq;

    source_freq = Cy_SysClk_ClkPathMuxGetFrequency(SRSS_DPLL_LP_1_PATH_NUM);

    cy_stc_pll_config_t pll_config = 
    {
        .inputFreq = source_freq,
        .outputFreq = DPLL_LP_FREQ,
        .lfMode = true,
        .outputMode = CY_SYSCLK_FLLPLL_OUTPUT_OUTPUT,
    };

    if(CY_SYSCLK_SUCCESS == Cy_SysClk_PllDisable(SRSS_DPLL_LP_1_PATH_NUM))
    {
        if(CY_SYSCLK_SUCCESS != Cy_SysClk_DpllLpConfigure(SRSS_DPLL_LP_1_PATH_NUM, &pll_config))
        {
            handle_app_error();
        }
    
        if(CY_SYSCLK_SUCCESS != Cy_SysClk_DpllLpEnable(SRSS_DPLL_LP_1_PATH_NUM, DPLL_DELAY_MS))
        {
            handle_app_error();
        }
    
        if(!Cy_SysClk_DpllLpLocked(SRSS_DPLL_LP_1_PATH_NUM))
        {
            handle_app_error();
        }
    }

    if(CY_SYSCLK_SUCCESS == Cy_SysClk_ClkHfDisable(CY_CFG_SYSCLK_CLKHF7))
    {
        if(CY_SYSCLK_SUCCESS != Cy_SysClk_ClkHfSetSource(CY_CFG_SYSCLK_CLKHF7, CY_SYSCLK_CLKHF_IN_CLKPATH1))
        {
            handle_app_error(); 
        }
    
        if(CY_SYSCLK_SUCCESS != Cy_SysClk_ClkHfEnable(CY_CFG_SYSCLK_CLKHF7))
        {
            handle_app_error(); 
        }
    }

    if(CY_SYSCLK_SUCCESS == Cy_SysClk_PeriPclkDisableDivider((en_clk_dst_t)CYBSP_PDM_CLK_DIV_GRP_NUM,
                                                             CY_SYSCLK_DIV_16_5_BIT, 1U))
    {
        if(CY_SYSCLK_SUCCESS != Cy_SysClk_PeriPclkSetFracDivider((en_clk_dst_t)CYBSP_PDM_CLK_DIV_GRP_NUM,
                                                                 CY_SYSCLK_DIV_16_5_BIT, 1U, PDM_CLK_DIV_INT, 0U))
        {
            handle_app_error();
        }

        if(CY_SYSCLK_SUCCESS != Cy_SysClk_PeriPclkEnableDivider((en_clk_dst_t)CYBSP_PDM_CLK_DIV_GRP_NUM,
                                                                CY_SYSCLK_DIV_16_5_BIT, 1U))
        {
            handle_app_error();
        }
    }
}


/*******************************************************************************
* Function Name: audio_app_init
********************************************************************************
* Summary:
*  Creates the RTOS task "Audio App Task".
*
* Parameters:
*  None
*
* Return:
*  None
*
*******************************************************************************/
void audio_app_init(void)
{
    BaseType_t rtos_task_status;

    /* Create the AUDIO APP RTOS task */
    rtos_task_status = xTaskCreate(audio_app_task, "Audio App Task", AUDIO_TASK_STACK_DEPTH, NULL,
            AUDIO_APP_TASK_PRIORITY, &rtos_audio_app_task);


    if (pdPASS != rtos_task_status)
    {
        handle_app_error();
    }
}


/*******************************************************************************
* Function Name: audio_app_task
********************************************************************************
* Summary:
*  Main audio task. Initializes the USB communication and the audio application.
*  In the main loop, checks USB device connectivity and based on that start/stop
*  providing audio data to the host.
*
* Parameters:
*  arg
*
* Return:
*  None
*
*******************************************************************************/
void audio_app_task(void *arg)
{
    uint8_t usb_status = USB_SUSPENDED;

    CY_UNUSED_PARAMETER(arg);

    /* Initializes the USB stack */
    USBD_Init();

    /* Setup the clock tree for required audio sampling rate */
    app_clock_init();
    
    /* Endpoint Initialization for Audio class */
    handle = add_audio();

    /* Set device info used in enumeration */
    USBD_SetDeviceInfo(&usb_deviceInfo);

    /* Set write timeout for IN endpoint */
    USBD_AUDIO_Set_Timeouts(handle, 0, WRITE_TIMEOUT_MS);

    /* Init the audio IN application */
    audio_in_init();

    /* Start the USB stack */
    USBD_Start();

    for (;;)
    {
        /* Check if USB device is configured */
        if(USB_STAT_CONFIGURED != (USBD_GetState() & (USB_STAT_CONFIGURED | USB_STAT_SUSPENDED)))
        {
            /* Stop providing audio data to the host */
            USBD_AUDIO_Stop_Play(handle);

            printf("APP_LOG: USB Audio Device Disconnected\r\n");

            /* Wait for USB device configuration */
            while (USB_STAT_CONFIGURED != (USBD_GetState() & (USB_STAT_CONFIGURED | USB_STAT_SUSPENDED)))
            {
                Cy_GPIO_Inv(CYBSP_USER_LED_PORT, CYBSP_USER_LED_PIN);
                usb_status = USB_SUSPENDED;

                /* Stop providing audio data to the host */
                USBD_AUDIO_Stop_Play(handle);

                USB_OS_Delay(USB_DELAY_MS);
            }
        }

        if(USB_SUSPENDED == usb_status)
        {
            usb_status = USB_CONNECTED;

            /* Start providing audio data to the host */
            USBD_AUDIO_Start_Play(handle, NULL);

            printf("APP_LOG: USB Audio Device Connected\r\n");
        }

        vTaskDelay(pdMS_TO_TICKS(TASK_DELAY_MS));
    }
}

/* [] END OF FILE */
