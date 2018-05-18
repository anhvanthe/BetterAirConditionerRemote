/**
 * Copyright (c) 2016 - 2018, Nordic Semiconductor ASA
 * 
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 * 
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form, except as embedded into a Nordic
 *    Semiconductor ASA integrated circuit in a product or a software update for
 *    such product, must reproduce the above copyright notice, this list of
 *    conditions and the following disclaimer in the documentation and/or other
 *    materials provided with the distribution.
 * 
 * 3. Neither the name of Nordic Semiconductor ASA nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 * 
 * 4. This software, with or without modification, must only be used with a
 *    Nordic Semiconductor ASA integrated circuit.
 * 
 * 5. Any software provided in binary form under this license must not be reverse
 *    engineered, decompiled, modified and/or disassembled.
 * 
 * THIS SOFTWARE IS PROVIDED BY NORDIC SEMICONDUCTOR ASA "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY, NONINFRINGEMENT, AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL NORDIC SEMICONDUCTOR ASA OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 */

#include "nrf_dfu_utils.h"

#include <string.h>
#include "nrf_dfu_settings.h"
#include "nrf_dfu_mbr.h"
#include "nrf_dfu_transport.h"
#include "nrf_bootloader_app_start.h"
#include "nrf_bootloader_info.h"
#include "nrf_fstorage.h"
#include "crc32.h"
#include "app_timer.h"
#include "nrf_delay.h"

#include "sr3_bootloader.h"

#define NRF_LOG_MODULE_NAME dfu_utils
#include "nrf_log.h"
#include "nrf_log_ctrl.h"
NRF_LOG_MODULE_REGISTER();

#if !defined(NRF_DFU_UPDATABLE_APPLICATION_ONLY) || (NRF_DFU_UPDATABLE_APPLICATION_ONLY == 0)
static app_timer_t nrf_dfu_post_sd_bl_timeout_timer = { {0} };
const app_timer_id_t nrf_dfu_post_sd_bl_timeout_timer_id = &nrf_dfu_post_sd_bl_timeout_timer;
#endif


#if ( defined(NRF_DFU_DUAL_BANK_SUPPORT) && (NRF_DFU_DUAL_BANK_SUPPORT == 1)) || \
    (!defined(NRF_DFU_UPDATABLE_APPLICATION_ONLY) || (NRF_DFU_UPDATABLE_APPLICATION_ONLY == 0))
/** @brief Round up val to the next page boundry
 */
static uint32_t align_to_page(uint32_t val)
{
    return ((val + CODE_PAGE_SIZE - 1 ) &~ (CODE_PAGE_SIZE - 1));
}


static void nrf_dfu_invalidate_bank(nrf_dfu_bank_t * p_bank)
{
    // Set the bank-code to invalid, and reset size/CRC
    memset(p_bank, 0, sizeof(nrf_dfu_bank_t));
    p_bank->bank_code = NRF_DFU_BANK_INVALID;
}
#endif /* ( defined(NRF_DFU_DUAL_BANK_SUPPORT) && (NRF_DFU_DUAL_BANK_SUPPORT == 1)) || \
          (!defined(NRF_DFU_UPDATABLE_APPLICATION_ONLY) || (NRF_DFU_UPDATABLE_APPLICATION_ONLY == 0)) */


#if defined(NRF_DFU_DUAL_BANK_SUPPORT) && (NRF_DFU_DUAL_BANK_SUPPORT == 1)
/** @brief Function to continue application update.
 *
 * @details This function will be called after reset if there is a valid application in Bank1
 *          required to be copied down to Bank 0.
 *
 * @param[in]       src_addr            Source address of the application to copy from Bank1 to Bank0.
 *
 * @retval  NRF_SUCCESS                 Continuation was successful.
 * @retval  NRF_ERROR_NULL              Invalid data during compare.
 * @retval  FS_ERR_UNALIGNED_ADDR       A call to fstorage was not aligned to a page boundary or the address was not word aliged.
 * @retval  FS_ERR_INVALID_ADDR         The destination of a call to fstorage does not point to
 *                                      the start of a flash page or the operation would
 *                                      go beyond the flash memory boundary.
 * @retval  FS_ERR_NOT_INITIALIZED      The fstorage module is not initialized.
 * @retval  FS_ERR_INVALID_CFG          The initialization of the fstorage module is invalid.
 * @retval  FS_ERR_NULL_ARG             A call to fstorage had an invalid NULL argument.
 * @retval  FS_ERR_INVALID_ARG          A call to fstorage had invalid arguments.
 * @retval  FS_ERR_QUEUE_FULL           If the internal operation queue of the fstorage module is full.
 * @retval  FS_ERR_FAILURE_SINCE_LAST   If an error occurred in another transaction and fstorage cannot continue before
 *                                      the event has been dealt with.
 */
static uint32_t nrf_dfu_app_continue(uint32_t src_addr)
{
    // This function is only in use when new app is present in Bank 1
    uint32_t const image_size  = s_dfu_settings.bank_1.image_size;
    uint32_t const copy_len  = CODE_PAGE_SIZE; // Must be page aligned

    uint32_t ret_val;
    uint32_t target_addr = MAIN_APPLICATION_START_ADDR + s_dfu_settings.write_offset;
    uint32_t length_left = (image_size - s_dfu_settings.write_offset);

    NRF_LOG_DEBUG("Enter nrf_dfu_app_continue");

    if ((s_dfu_settings.write_offset == 0) &&
        (s_dfu_settings.bank_0.bank_code == NRF_DFU_BANK_VALID_APP))
    {
        NRF_LOG_INFO("Invalidating existing application...");
        s_dfu_settings.bank_0.bank_code = NRF_DFU_BANK_INVALID;
        (void)nrf_dfu_settings_write();
    }

    src_addr += s_dfu_settings.write_offset;

    // Copy the application down safely
    do
    {
        // Feed watchdog to avoid a sudden reboot.
        sr3_bootloader_watchdog_feed();

        uint32_t cur_len = (length_left > copy_len) ? copy_len : length_left;

        // Erase the target page
        ret_val = nrf_dfu_flash_erase(target_addr, align_to_page(cur_len) / CODE_PAGE_SIZE, NULL);
        if (ret_val != NRF_SUCCESS)
        {
            return ret_val;
        }

        // Flash one page
        ret_val = nrf_dfu_flash_store(target_addr, (const void *)src_addr, cur_len, NULL);
        if (ret_val != NRF_SUCCESS)
        {
            return ret_val;
        }

        ret_val = nrf_dfu_mbr_compare((uint32_t *)target_addr, (uint32_t *)src_addr, cur_len);
        if (ret_val != NRF_SUCCESS)
        {
            // We will not retry the copy
            NRF_LOG_ERROR("Invalid data during compare: target: 0x%08x, src: 0x%08x", target_addr, src_addr);
            return ret_val;
        }

        s_dfu_settings.write_offset += cur_len;
        ret_val = nrf_dfu_settings_write();
        if (ret_val != NRF_SUCCESS)
        {
            NRF_LOG_ERROR("%s(): failed to write settings", __func__);
            return ret_val;
        }

        target_addr += cur_len;
        src_addr += cur_len;

        length_left -= cur_len;
    }
    while (length_left > 0);

    // Check the CRC of the copied data. Enable if so.
    uint32_t crc = crc32_compute((uint8_t *)MAIN_APPLICATION_START_ADDR, image_size, NULL);

    if (crc == s_dfu_settings.bank_1.image_crc)
    {
        NRF_LOG_DEBUG("Setting app as valid");
        s_dfu_settings.bank_0.bank_code = NRF_DFU_BANK_VALID_APP;
        s_dfu_settings.bank_0.image_crc = crc;
        s_dfu_settings.bank_0.image_size = image_size;
    }
    else
    {
        NRF_LOG_ERROR("CRC computation failed for copied app: "
                      "src crc: 0x%08x, res crc: 0x%08x",
                      s_dfu_settings.bank_1.image_crc,
                      crc);
    }

    nrf_dfu_invalidate_bank(&s_dfu_settings.bank_1);
    ret_val = nrf_dfu_settings_write();

    return ret_val;
}
#endif /* defined(NRF_DFU_DUAL_BANK_SUPPORT) && (NRF_DFU_DUAL_BANK_SUPPORT == 1) */

#if !defined(NRF_DFU_UPDATABLE_APPLICATION_ONLY) || (NRF_DFU_UPDATABLE_APPLICATION_ONLY == 0)
/** @brief Reset the device if no new DFU is initiated within the timer's expiration.
 *
 * After the completion of a SD, BL or SD + BL update, the controller might want to update the
 * application as well. Because of this, the DFU target will stay in bootloader mode for some
 * time after completion. However, if no such update is received, the device should reset
 * to look for a valid app and resume regular operation.
 */
static void nrf_dfu_post_sd_bl_timeout_start(void)
{
    NRF_LOG_DEBUG("%s", __func__);

    uint32_t err_code = app_timer_create(&nrf_dfu_post_sd_bl_timeout_timer_id,
                                         APP_TIMER_MODE_SINGLE_SHOT,
                                         nrf_dfu_reset_timeout_handler);
    APP_ERROR_CHECK(err_code);
    // 3400 ms is the smallest stable value with nRF Connect for PC v1.1.1.
    // 7500 ms is the smallest stable value with nRF Connect for Android v1.1.1.
    // Smaller values may allow the device to reset before the next DFU transation is started.
    err_code = app_timer_start(nrf_dfu_post_sd_bl_timeout_timer_id,
                               APP_TIMER_TICKS(NRF_DFU_POST_SD_BL_TIMEOUT_MS),
                               NULL);
    APP_ERROR_CHECK(err_code);
}

/** @brief Function to execute the continuation of a SoftDevice update.
 *
 * @param[in]       src_addr            Source address of the SoftDevice to copy from.
 * @param[in]       p_bank              Pointer to the bank where the SoftDevice resides.
 *
 * @retval NRF_SUCCESS Continuation was successful.
 * @retval NRF_ERROR_INVALID_LENGTH Invalid length.
 * @retval NRF_ERROR_NO_MEM If UICR.NRFFW[1] is not set (i.e. is 0xFFFFFFFF).
 * @retval NRF_ERROR_INVALID_PARAM If an invalid command is given.
 * @retval NRF_ERROR_INTERNAL Indicates that the contents of the memory blocks were not verified correctly after copying.
 * @retval NRF_ERROR_NULL If the content of the memory blocks differs after copying.
 */
#if defined(SOFTDEVICE_PRESENT)
static uint32_t nrf_dfu_sd_continue_impl(uint32_t             src_addr,
                                         nrf_dfu_bank_t     * p_bank)
{
    uint32_t target_addr = SOFTDEVICE_REGION_START + s_dfu_settings.write_offset;
    uint32_t length_left = align_to_page(s_dfu_settings.sd_size - s_dfu_settings.write_offset);
    uint32_t copy_len;
    uint32_t ret_val;

    NRF_LOG_DEBUG("Enter nrf_bootloader_dfu_sd_continue");

    ASSERT(s_dfu_settings.sd_size != 0);
    ASSERT(s_dfu_settings.write_offset <= s_dfu_settings.sd_size);

    if ((s_dfu_settings.sd_size != 0) && (s_dfu_settings.write_offset == s_dfu_settings.sd_size))
    {
        NRF_LOG_DEBUG("SD already copied");
        return NRF_SUCCESS;
    }

    if (s_dfu_settings.write_offset == 0)
    {
        NRF_LOG_DEBUG("Updating SD. Old SD ver: %u, New ver: %u",
                      SD_VERSION_GET(MBR_SIZE),
                      SD_VERSION_GET(src_addr));

        // Save the absolute address of SD in case SD/SD+BL update.
        s_dfu_settings.progress.sd_start_address = src_addr;

        (void)nrf_dfu_settings_write();
    }

    // This can be a continuation due to a power failure
    src_addr += s_dfu_settings.write_offset;

    if (length_left <= CODE_PAGE_SIZE)
    {
        copy_len = align_to_page(length_left);
    }
    else
    {
        copy_len = align_to_page(length_left / 4);
    }

    do
    {
        // Feed watchdog to avoid a sudden reboot.
        sr3_bootloader_watchdog_feed();

        // If less than split size remain, reduce split size to avoid overwriting Bank 0.
        if (copy_len > length_left)
        {
            copy_len = align_to_page(length_left);
        }

        NRF_LOG_DEBUG("Copying [0x%08x-0x%08x] to [0x%08x-0x%08x]: Len: 0x%08x",
                      src_addr, src_addr + copy_len, target_addr, target_addr + copy_len, copy_len);

        // Copy a chunk of the SD. Size in words
        ret_val = nrf_dfu_mbr_copy_sd((uint32_t *)target_addr, (uint32_t *)src_addr, copy_len);
        if (ret_val != NRF_SUCCESS)
        {
            NRF_LOG_ERROR("Failed to copy SD: target: 0x%08x, src: 0x%08x, len: 0x%08x",
                          target_addr, src_addr, copy_len);
            return ret_val;
        }

        NRF_LOG_DEBUG("Finished copying [0x%08x-0x%08x] to [0x%08x-0x%08x]: Len: 0x%08x",
                      src_addr, src_addr + copy_len, target_addr, target_addr + copy_len, copy_len);

        // Validate copy. Size in words
        ret_val = nrf_dfu_mbr_compare((uint32_t *)target_addr, (uint32_t *)src_addr, copy_len);
        if (ret_val != NRF_SUCCESS)
        {
            NRF_LOG_ERROR("Failed to Compare SD: target: 0x%08x, src: 0x%08x, len: 0x%08x",
                          target_addr, src_addr, copy_len);
            return ret_val;
        }

        NRF_LOG_DEBUG("Validated 0x%08x-0x%08x to 0x%08x-0x%08x: Size: 0x%08x",
                      src_addr, src_addr + copy_len, target_addr, target_addr + copy_len, copy_len);

        target_addr += copy_len;
        src_addr += copy_len;

        if (copy_len > length_left)
        {
            length_left = 0;
        }
        else
        {
            length_left -= copy_len;
        }

        // Save the updated point of writes in case of power loss
        s_dfu_settings.write_offset = s_dfu_settings.sd_size - length_left;

        ret_val = nrf_dfu_settings_write();
        if (ret_val != NRF_SUCCESS)
        {
            NRF_LOG_ERROR("%s(): failed to write settings", __func__);
            return ret_val;
        }
    }
    while (length_left > 0);

    NRF_LOG_DEBUG("Finished with the SD update.");

    return ret_val;
}


/** @brief Function to continue SoftDevice update.
 *
 * @details This function will be called after reset if there is a valid SoftDevice in Bank0 or Bank1
 *          required to be relocated and activated through MBR commands.
 *
 * @param[in]       src_addr            Source address of the SoftDevice to copy from.
 * @param[in]       p_bank              Pointer to the bank where the SoftDevice resides.
 *
 * @retval NRF_SUCCESS Continuation was successful.
 * @retval NRF_ERROR_INVALID_LENGTH Invalid length.
 * @retval NRF_ERROR_NO_MEM If UICR.NRFFW[1] is not set (i.e. is 0xFFFFFFFF).
 * @retval NRF_ERROR_INVALID_PARAM If an invalid command is given.
 * @retval NRF_ERROR_INTERNAL Indicates that the contents of the memory blocks were not verified correctly after copying.
 * @retval NRF_ERROR_NULL If the content of the memory blocks differs after copying.
 */
static uint32_t nrf_dfu_sd_continue(uint32_t             src_addr,
                                    nrf_dfu_bank_t     * p_bank)
{
    uint32_t ret_val;

    // If the continuation is after a power loss, SoftDevice may already have started to be copied.
    // In this case, use the absolute value of the SoftDevice start address to ensure correct copy.
    if (s_dfu_settings.progress.sd_start_address != 0)
    {
        src_addr = s_dfu_settings.progress.sd_start_address;
    }

    ret_val = nrf_dfu_sd_continue_impl(src_addr, p_bank);
    if (ret_val != NRF_SUCCESS)
    {
        NRF_LOG_ERROR("SD update continuation failed");
        return ret_val;
    }

    nrf_dfu_invalidate_bank(p_bank);

    // Upon successful completion, the callback function will be called and reset the device. If a valid app is present, it will launch.
    NRF_LOG_DEBUG("Writing settings and reseting device.");
    ret_val = nrf_dfu_settings_write();

    return ret_val;
}
#endif

/** @brief Function to continue bootloader update.
 *
 * @details     This function will be called after reset if there is a valid bootloader in Bank 0 or Bank 1
 *              required to be relocated and activated through MBR commands.
 *
 * @param[in]       src_addr            Source address of the BL to copy from.
 * @param[in]       p_bank              Pointer to the bank where the SoftDevice resides.
 *
 * @return This function will not return if the bootloader is copied successfully.
 *         After the copy is verified, the device will reset and start the new bootloader.
 *
 * @retval NRF_SUCCESS Continuation was successful.
 * @retval NRF_ERROR_INVALID_LENGTH Invalid length of flash operation.
 * @retval NRF_ERROR_NO_MEM If no parameter page is provided (see sds for more info).
 * @retval NRF_ERROR_INVALID_PARAM If an invalid command is given.
 * @retval NRF_ERROR_INTERNAL Internal error that should not happen.
 * @retval NRF_ERROR_FORBIDDEN If NRF_UICR->BOOTADDR is not set.
 */
static uint32_t nrf_dfu_bl_continue(uint32_t src_addr, nrf_dfu_bank_t * p_bank)
{
    uint32_t        ret_val     = NRF_ERROR_NULL;
    uint32_t const  len         = (p_bank->image_size - s_dfu_settings.sd_size);

    NRF_LOG_DEBUG("Verifying BL: Addr: 0x%08x, Src: 0x%08x, Len: 0x%08x", BOOTLOADER_START_ADDR, src_addr, len);

#if NRF_DFU_WORKAROUND_PRE_SDK_14_1_0_SD_BL_UPDATE
    // This code is a configurable workaround for updating SD+BL from SDK 12.x.y - 14.1.0
    // SoftDevice size increase would lead to unaligned source address when comparing new BL in SD+BL updates.
    // This workaround is not required once BL is successfully installed with a version that is compiled SDK 14.1.0
#if defined(NRF52832_XXAA)
    if (p_bank->bank_code == NRF_DFU_BANK_VALID_SD_BL)
    {
        ret_val = nrf_dfu_mbr_compare((uint32_t *)BOOTLOADER_START_ADDR, (uint32_t *)(src_addr - 0x4000), len);
    }
#endif // defined(NRF52832_XXAA)
#endif // #if NRF_DFU_WORKAROUND_PRE_SDK_14_1_0_SD_BL_UPDATE

    // Check if the BL has already been copied.
    if (ret_val != NRF_SUCCESS)
    {
        ret_val = nrf_dfu_mbr_compare((uint32_t *)BOOTLOADER_START_ADDR, (uint32_t *)src_addr, len);
    }

    if (ret_val == NRF_SUCCESS)
    {
        // Don't copy if the bootloader is the same as the banked version.
        NRF_LOG_DEBUG("Bootloader is identical - skip copy");
    }
    else
    {
        // Bootloader is different than the banked version. Continue copy
        // Note that if the SD and BL was combined, then the split point between
        // them is in s_dfu_settings.sd_size
        NRF_LOG_DEBUG("Copy updated bootloader: src: 0x%08x, len: 0x%08x", src_addr, len);

        // Feed watchdog to avoid a sudden reboot.
        sr3_bootloader_watchdog_feed();

        ret_val = nrf_dfu_mbr_copy_bl((uint32_t *)src_addr, len);
        if (ret_val != NRF_SUCCESS)
        {
            NRF_LOG_ERROR("Request to copy BL failed %d", ret_val);
        }
    }

    // Invalidate bank, marking completion.
    nrf_dfu_invalidate_bank(p_bank);

    NRF_LOG_DEBUG("Writing settings and reseting device.");
    ret_val = nrf_dfu_settings_write();

    return ret_val;
}


/** @brief Function to continue combined bootloader and SoftDevice update.
 *
 * @details     This function will be called after reset if there is a valid bootloader and SoftDevice in Bank 0 or Bank 1
 *              required to be relocated and activated through MBR commands.
 *
 * @param[in]       src_addr            Source address of the combined bootloader and SoftDevice to copy from.
 * @param[in]       p_bank              Pointer to the bank where the SoftDevice resides.
 *
 * @retval NRF_SUCCESS Continuation was successful.
 * @retval NRF_ERROR_INVALID_LENGTH Invalid length.
 * @retval NRF_ERROR_NO_MEM If UICR.NRFFW[1] is not set (i.e. is 0xFFFFFFFF).
 * @retval NRF_ERROR_INVALID_PARAM If an invalid command is given.
 * @retval NRF_ERROR_INTERNAL Indicates that the contents of the memory blocks where not verified correctly after copying.
 * @retval NRF_ERROR_NULL If the content of the memory blocks differs after copying.
 * @retval NRF_ERROR_FORBIDDEN If NRF_UICR->BOOTADDR is not set.
 */
#if defined(SOFTDEVICE_PRESENT)
static uint32_t nrf_dfu_sd_bl_continue(uint32_t src_addr, nrf_dfu_bank_t * p_bank)
{
    uint32_t ret_val = NRF_SUCCESS;

    NRF_LOG_DEBUG("Enter nrf_dfu_sd_bl_continue");

    // If the continuation is after a power loss, SoftDevice may already have started to be copied.
    // In this case, use the absolute value of the SoftDevice start address to ensure correct copy.
    if (s_dfu_settings.progress.sd_start_address != 0)
    {
        src_addr = s_dfu_settings.progress.sd_start_address;
    }

    ret_val = nrf_dfu_sd_continue_impl(src_addr, p_bank);
    if (ret_val != NRF_SUCCESS)
    {
        NRF_LOG_ERROR("SD+BL: SD copy failed");
        return ret_val;
    }

    src_addr += s_dfu_settings.sd_size;

    ret_val = nrf_dfu_bl_continue(src_addr, p_bank);
    if (ret_val != NRF_SUCCESS)
    {
        NRF_LOG_ERROR("SD+BL: BL copy failed");
        return ret_val;
    }

    return ret_val;
}
#endif
#endif /* !defined(NRF_DFU_UPDATABLE_APPLICATION_ONLY) || (NRF_DFU_UPDATABLE_APPLICATION_ONLY == 0) */

#if ( defined(NRF_DFU_DUAL_BANK_SUPPORT) && (NRF_DFU_DUAL_BANK_SUPPORT == 1)) || \
    (!defined(NRF_DFU_UPDATABLE_APPLICATION_ONLY) || (NRF_DFU_UPDATABLE_APPLICATION_ONLY == 0))
static uint32_t nrf_dfu_continue_bank(nrf_dfu_bank_t * p_bank, uint32_t src_addr, bool * p_enter_dfu_mode)
{
    uint32_t ret_val = NRF_SUCCESS;

    switch (p_bank->bank_code)
    {
       case NRF_DFU_BANK_VALID_APP:
            NRF_LOG_DEBUG("Valid App");
#if defined(NRF_DFU_DUAL_BANK_SUPPORT) && (NRF_DFU_DUAL_BANK_SUPPORT == 1)
            if (s_dfu_settings.bank_current == NRF_DFU_CURRENT_BANK_1)
            {
                // Only continue copying if valid app in Bank 1
                ret_val = nrf_dfu_app_continue(src_addr);
            }
#endif /* defined(NRF_DFU_DUAL_BANK_SUPPORT) && (NRF_DFU_DUAL_BANK_SUPPORT == 1) */
            break;

#if !defined(NRF_DFU_UPDATABLE_APPLICATION_ONLY) || (NRF_DFU_UPDATABLE_APPLICATION_ONLY == 0)
#if defined(SOFTDEVICE_PRESENT)
       case NRF_DFU_BANK_VALID_SD:
            NRF_LOG_DEBUG("Valid SD");
            // There is a valid SD that needs to be copied (or continued)
            ret_val = nrf_dfu_sd_continue(src_addr, p_bank);
            (*p_enter_dfu_mode) = true;
            break;
#endif

        case NRF_DFU_BANK_VALID_BL:
            NRF_LOG_DEBUG("Valid BL");
            // There is a valid BL that must be copied (or continued)
            ret_val = nrf_dfu_bl_continue(src_addr, p_bank);
            break;

#if defined(SOFTDEVICE_PRESENT)
        case NRF_DFU_BANK_VALID_SD_BL:
            NRF_LOG_DEBUG("Valid SD + BL");
            // There is a valid SD + BL that must be copied (or continued)
            ret_val = nrf_dfu_sd_bl_continue(src_addr, p_bank);
            (*p_enter_dfu_mode) = true;
            // Set the bank-code to invalid, and reset size/CRC
            break;
#endif
#endif /* !defined(NRF_DFU_UPDATABLE_APPLICATION_ONLY) || (NRF_DFU_UPDATABLE_APPLICATION_ONLY == 0)*/

        case NRF_DFU_BANK_INVALID:
        default:
            NRF_LOG_ERROR("Invalid bank (code %u)", p_bank->bank_code);
            break;
    }

    if ((ret_val == NRF_SUCCESS) &&
        (*p_enter_dfu_mode != false) &&
        nrf_dfu_app_is_valid(false))
    {
        // FLASH is in NVM mode. All operations are blocking are are now completed.
        // If update is complete start post SD/BL DFU inactivity timeout.
        nrf_dfu_post_sd_bl_timeout_start();
    }

    return ret_val;
}


uint32_t nrf_dfu_continue(bool * p_enter_dfu_mode)
{
    uint32_t            ret_val;
    nrf_dfu_bank_t    * p_bank;
    uint32_t            src_addr = CODE_REGION_1_START;

    NRF_LOG_DEBUG("Enter nrf_dfu_continue");

    if (s_dfu_settings.bank_layout == NRF_DFU_BANK_LAYOUT_SINGLE)
    {
        p_bank = &s_dfu_settings.bank_0;
    }
    else if (s_dfu_settings.bank_current == NRF_DFU_CURRENT_BANK_0)
    {
        p_bank = &s_dfu_settings.bank_0;
    }
    else
    {
        p_bank = &s_dfu_settings.bank_1;
        src_addr += align_to_page(s_dfu_settings.bank_0.image_size);
    }

    ret_val = nrf_dfu_continue_bank(p_bank, src_addr, p_enter_dfu_mode);
    return ret_val;
}
#endif /* ( defined(NRF_DFU_DUAL_BANK_SUPPORT) && (NRF_DFU_DUAL_BANK_SUPPORT == 1)) || \
          (!defined(NRF_DFU_UPDATABLE_APPLICATION_ONLY) || (NRF_DFU_UPDATABLE_APPLICATION_ONLY == 0)) */


void nrf_dfu_reset_timeout_handler(void * p_context)
{
    UNUSED_PARAMETER(p_context);

    NRF_LOG_DEBUG("DFU inactivity timeout");

    APP_ERROR_CHECK(nrf_dfu_transports_close());

    if (nrf_dfu_app_is_valid(false))
    {
#if NRF_LOG_ENABLED
        NRF_LOG_INFO("Application valid: reset system");
        NRF_LOG_FLUSH();
        nrf_delay_ms(100);
#endif

        NVIC_SystemReset();
    }

    NRF_LOG_INFO("Application invalid: enter low-power mode");

    APP_ERROR_CHECK(nrf_dfu_transports_init(true));
}


bool nrf_dfu_app_is_valid(bool skip_crc_check)
{
    NRF_LOG_DEBUG("Enter %s", __func__);

    if (s_dfu_settings.bank_0.bank_code != NRF_DFU_BANK_VALID_APP)
    {
        // Bank 0 has no valid app. Nothing to boot.
        NRF_LOG_ERROR("No application in bank 0");
        return false;
    }

    if (!skip_crc_check && (s_dfu_settings.bank_0.image_crc != 0))
    {
        uint32_t crc = crc32_compute((uint8_t *) CODE_REGION_1_START,
                                     s_dfu_settings.bank_0.image_size,
                                     NULL);

        if (crc != s_dfu_settings.bank_0.image_crc)
        {
            // CRC does not match with what is stored.
            NRF_LOG_ERROR("Invalid application CRC");
            return false;
        }

        NRF_LOG_DEBUG("Application CRC is valid");
    }

    NRF_LOG_DEBUG("App is valid");

    return true;
}


uint32_t nrf_dfu_find_cache(uint32_t size_req, uint32_t * p_address)
{
    const uint32_t   available_size = DFU_REGION_TOTAL_SIZE - DFU_APP_DATA_RESERVED;
    nrf_dfu_bank_t * p_bank = NULL;

    NRF_LOG_DEBUG("Enter nrf_dfu_find_cache");

    ASSERT(p_address != NULL);

    // Simple check whether the size requirement can be met
    if (available_size < size_req)
    {
        NRF_LOG_DEBUG("No way to fit the new firmware on device");
        return NRF_ERROR_NO_MEM;
    }

    NRF_LOG_DEBUG("Bank content");
    NRF_LOG_DEBUG("Bank type: %d", s_dfu_settings.bank_layout);
    NRF_LOG_DEBUG("Bank 0 code: 0x%02x: Size: %d", s_dfu_settings.bank_0.bank_code, s_dfu_settings.bank_0.image_size);

#if defined(NRF_DFU_DUAL_BANK_SUPPORT) && (NRF_DFU_DUAL_BANK_SUPPORT == 1)
    NRF_LOG_DEBUG("Bank 1 code: 0x%02x: Size: %d", s_dfu_settings.bank_1.bank_code, s_dfu_settings.bank_1.image_size);

    // Check if update can be done using dual bank layout.
    if (s_dfu_settings.bank_0.bank_code == NRF_DFU_BANK_VALID_APP)
    {
        size_t bank0_size = align_to_page(s_dfu_settings.bank_0.image_size);

        NRF_LOG_DEBUG("available size: %u, free_size: %u, size_req: %u",
                      available_size,
                      available_size - bank0_size,
                      size_req);

        if (size_req <= available_size - bank0_size)
        {
            // Update can be done using dual bank layout.
            // Set to first free page boundary after previous app

            s_dfu_settings.bank_layout  = NRF_DFU_BANK_LAYOUT_DUAL;
            s_dfu_settings.bank_current = NRF_DFU_CURRENT_BANK_1;

            (*p_address) = MAIN_APPLICATION_START_ADDR + bank0_size;

            p_bank = &s_dfu_settings.bank_1;
            NRF_LOG_DEBUG("Using second bank");
        }
    }
#endif /* defined(NRF_DFU_DUAL_BANK_SUPPORT) && (NRF_DFU_DUAL_BANK_SUPPORT == 1) */

    if (p_bank == NULL)
    {
        // Can only support single bank update, clearing old app.
        s_dfu_settings.bank_layout  = NRF_DFU_BANK_LAYOUT_SINGLE;
        s_dfu_settings.bank_current = NRF_DFU_CURRENT_BANK_0;

        (*p_address) = MAIN_APPLICATION_START_ADDR;

        p_bank = &s_dfu_settings.bank_0;
        NRF_LOG_DEBUG("Single bank DFU");
    }

    // Set the bank-code to invalid, and reset size/CRC
    memset(p_bank, 0, sizeof(nrf_dfu_bank_t));

    // Store the Firmware size in the bank for continuations
    p_bank->image_size = size_req;
    p_bank->bank_code  = NRF_DFU_BANK_INVALID;

    return NRF_SUCCESS;
}
