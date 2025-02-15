/* ====================================================================
 *
 *
 *   BSD LICENSE
 *
 *   Copyright(c) 2020-2021 Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * ====================================================================
 */

/*****************************************************************************
 * @file qat_hw_gcm.c
 *
 * This file contains the engine implementations for GCM cipher operations
 *
 *****************************************************************************/

#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <string.h>
#include <pthread.h>
#include <signal.h>
#ifdef USE_QAT_CONTIG_MEM
# include "qae_mem_utils.h"
#endif
#ifdef USE_USDM_MEM
# include "qat_hw_usdm_inf.h"
#endif
#include "e_qat.h"
#include "qat_utils.h"
#include "qat_hw_callback.h"
#include "qat_hw_polling.h"
#include "qat_events.h"
#include "e_qat_err.h"

#include "cpa.h"
#include "cpa_types.h"
#include "cpa_cy_sym.h"
#include "qat_hw_gcm.h"
#include "qat_hw_ciphers.h"

#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/err.h>
#include <openssl/sha.h>
#include <openssl/tls1.h>
#include <openssl/async.h>
#include <openssl/ssl.h>

#ifdef ENABLE_QAT_HW_GCM
# ifdef DISABLE_QAT_HW_GCM
#  undef DISABLE_QAT_HW_GCM
# endif
#endif

#ifndef DISABLE_QAT_HW_GCM
static int qat_aes_gcm_init(EVP_CIPHER_CTX *ctx,
                            const unsigned char *inkey,
                            const unsigned char *iv, int enc);
static int qat_aes_gcm_cleanup(EVP_CIPHER_CTX *ctx);
static int qat_aes_gcm_cipher(EVP_CIPHER_CTX *ctx,
                              unsigned char *out,
                              const unsigned char *in, size_t len);
static int qat_aes_gcm_ctrl(EVP_CIPHER_CTX *ctx, int type, int arg,
                            void *ptr);
#endif

static inline const EVP_CIPHER *qat_gcm_cipher_sw_impl(int nid)
{
    switch (nid) {
        case NID_aes_128_gcm:
            return EVP_aes_128_gcm();
        case NID_aes_256_gcm:
            return EVP_aes_256_gcm();
        default:
            WARN("Invalid nid %d\n", nid);
            return NULL;
    }
}

const EVP_CIPHER *qat_create_gcm_cipher_meth(int nid, int keylen)
{
#ifndef DISABLE_QAT_HW_GCM
    EVP_CIPHER *c = NULL;
    int res = 1;

    if ((c = EVP_CIPHER_meth_new(nid, AES_GCM_BLOCK_SIZE, keylen)) == NULL) {
        WARN("Failed to allocate cipher methods for nid %d\n", nid);
        return NULL;
    }

    res &= EVP_CIPHER_meth_set_iv_length(c, AES_GCM_IV_LEN);
    res &= EVP_CIPHER_meth_set_flags(c, QAT_GCM_FLAGS);
    res &= EVP_CIPHER_meth_set_init(c, qat_aes_gcm_init);
    res &= EVP_CIPHER_meth_set_do_cipher(c, qat_aes_gcm_cipher);
    res &= EVP_CIPHER_meth_set_cleanup(c, qat_aes_gcm_cleanup);
    res &= EVP_CIPHER_meth_set_impl_ctx_size(c, sizeof(qat_gcm_ctx));
    res &= EVP_CIPHER_meth_set_set_asn1_params(c, EVP_CIPH_FLAG_DEFAULT_ASN1 ?
                                               NULL : EVP_CIPHER_set_asn1_iv);
    res &= EVP_CIPHER_meth_set_get_asn1_params(c, EVP_CIPH_FLAG_DEFAULT_ASN1 ?
                                               NULL : EVP_CIPHER_get_asn1_iv);
    res &= EVP_CIPHER_meth_set_ctrl(c, qat_aes_gcm_ctrl);

    if (0 == res) {
        WARN("Failed to set cipher methods for nid %d\n", nid);
        EVP_CIPHER_meth_free(c);
        c = NULL;
    }

    return c;
#else
    return qat_gcm_cipher_sw_impl(nid);
#endif
}

#ifndef DISABLE_QAT_HW_GCM
/******************************************************************************
* function:
*         qat_session_data_init(EVP_CIPHER_CTX *ctx,
*                               qat_aes_gcm_ctx *qctx,
*                               const unsigned char* key,
*                               const unsigned char* iv,
*                               int enc)
*
* @param ctx     [IN] - pointer to the evp context
* @param qctx    [IN] - pointer to the qat context
* @param key     [IN] - pointer to the cipher key
* @param iv      [IN] - pointer to the iv this maybe NULL.
* @param enc     [IN] - whether we are doing encryption (1) or decryption (0).
*
* description:
*    This function is to create QAT specific session data.
*    It is called from the function qat_aes_gcm_init().
*
*    It will return 1 if successful and 0 on failure.
******************************************************************************/
static int qat_session_data_init(EVP_CIPHER_CTX *ctx,
                                 qat_gcm_ctx *qctx,
                                 const unsigned char* key,
                                 const unsigned char* iv,
                                 int enc)
{
    DEBUG("Entering\n");
    if (NULL == qctx || NULL == ctx) {
        WARN("qctx or ctx is NULL\n");
        QATerr(QAT_F_QAT_SESSION_DATA_INIT, QAT_R_QCTX_CTX_NULL);
        return 0;
    }

    if (key != NULL) {
        if (qctx->cipher_key) {
            qaeCryptoMemFreeNonZero(qctx->cipher_key);
            qctx->cipher_key = NULL;
        }
        qctx->cipher_key = qaeCryptoMemAlloc(EVP_CIPHER_CTX_key_length(ctx), __FILE__, __LINE__);
        if (qctx->cipher_key == NULL) {
            WARN("Unable to allocate memory for qctx->cipher_key.\n");
            QATerr(QAT_F_QAT_SESSION_DATA_INIT, QAT_R_KEY_MALLOC_FAILURE);
            return 0;
        }
        memcpy(qctx->cipher_key, key, EVP_CIPHER_CTX_key_length(ctx));
        qctx->key_set = 1;
    }

    if (iv != NULL && qctx->iv_set) {
        qctx->OpData.pIv = qctx->iv;
        qctx->OpData.ivLenInBytes = qctx->iv_len;
    }
    qctx->session_data = OPENSSL_malloc(sizeof(CpaCySymSessionSetupData));
    if (NULL == qctx->session_data) {
        WARN("session setup data Malloc failure\n");
        QATerr(QAT_F_QAT_SESSION_DATA_INIT, QAT_R_SSD_MALLOC_FAILURE);
        return 0;
    }

    /* Set priority and operation of this session */
    qctx->session_data->sessionPriority = CPA_CY_PRIORITY_HIGH;
    qctx->session_data->symOperation = CPA_CY_SYM_OP_ALGORITHM_CHAINING;

    /* --- Cipher configuration --- */

    /* Cipher algorithm and mode */
    qctx->session_data->cipherSetupData.cipherAlgorithm =
     CPA_CY_SYM_CIPHER_AES_GCM;
    /* Cipher key length */
    qctx->session_data->cipherSetupData.cipherKeyLenInBytes = (Cpa32U)EVP_CIPHER_CTX_key_length(ctx);

    if (qctx->key_set)
        qctx->session_data->cipherSetupData.pCipherKey = (Cpa8U *)qctx->cipher_key;

    /* Operation to perform */
    if (enc) {
        qctx->session_data->cipherSetupData.cipherDirection =
                        CPA_CY_SYM_CIPHER_DIRECTION_ENCRYPT;
        qctx->session_data->algChainOrder =
                        CPA_CY_SYM_ALG_CHAIN_ORDER_CIPHER_THEN_HASH;
    } else {
        qctx->session_data->cipherSetupData.cipherDirection =
                        CPA_CY_SYM_CIPHER_DIRECTION_DECRYPT;
        qctx->session_data->algChainOrder =
                        CPA_CY_SYM_ALG_CHAIN_ORDER_HASH_THEN_CIPHER;
    }

    /* --- Hash Configuration --- */

    /* Set the hash mode and the length of the digest */
    qctx->session_data->hashSetupData.hashAlgorithm = CPA_CY_SYM_HASH_AES_GCM;
    qctx->session_data->hashSetupData.hashMode = CPA_CY_SYM_HASH_MODE_AUTH;
    qctx->session_data->hashSetupData.digestResultLenInBytes = EVP_GCM_TLS_TAG_LEN;

    /* For GCM authKey and authKeyLen are not required.
     * This information is provided by the cipherKey in cipherSetupData */
    qctx->session_data->hashSetupData.authModeSetupData.authKey = NULL;
    qctx->session_data->hashSetupData.authModeSetupData.authKeyLenInBytes = 0;

    /* Set the length of the AAD to the default value */
    qctx->session_data->hashSetupData.authModeSetupData.aadLenInBytes = 0;

    /* Tag follows immediately after the region to hash */
    qctx->session_data->digestIsAppended = CPA_TRUE;

    /* digestVerify is not required to be set. For GCM authenticated
     * encryption this value is understood to be CPA_FALSE during encryption and
     * CPA_TRUE during decryption */

    qctx->init_params_set = 1;

    return 1;
}


/******************************************************************************
* function:
*         qat_aes_gcm_init(EVP_CIPHER_CTX *ctx,
*                          const unsigned char *inkey,
*                          const unsigned char *iv,
*                          int enc)
*
* @param ctx     [IN]  - pointer to existing context
* @param inKey   [IN]  - input cipher key
* @param iv      [IN]  - initialisation vector
* @param enc     [IN]  - 1 encrypt 0 decrypt
*
* @retval 1      function succeeded
* @retval 0      function failed
*
* description:
*    This function initialises the cipher and hash algorithm parameters for this
*  EVP context.
*
******************************************************************************/
static int qat_aes_gcm_init(EVP_CIPHER_CTX *ctx,
                            const unsigned char *inkey,
                            const unsigned char *iv,
                            int enc)
{
    qat_gcm_ctx* qctx = NULL;

    if (NULL == ctx) {
        WARN("ctx is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_INIT, QAT_R_CTX_NULL);
        return 0;
    }

    DEBUG("CTX = %p, key = %p, iv = %p, enc = %d\n",
         (void*)ctx, (void*)inkey, (void*)iv, enc);

    if (!inkey && !iv) {
        DEBUG("key and IV not set\n");
        return 1;
    }

    /* Initialise a QAT session and set the cipher keys*/
    qctx = QAT_GCM_GET_CTX(ctx);
    if (NULL == qctx) {
        WARN("qctx is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_INIT, QAT_R_QCTX_NULL);
        return 0;
    }

    /* Initialize pointers to NULL to handle failures correctly */
    qctx->iv = NULL;

    DEBUG("IV Len = %d Key Len = %d\n",
           EVP_CIPHER_CTX_iv_length(ctx), EVP_CIPHER_CTX_key_length(ctx));

    /* The length of the IV in the TLS case is fixed = 12 Bytes */
    qctx->iv_len = QAT_GCM_TLS_TOTAL_IV_LEN;

    /* Allocate pinned memory since there can be at most 1 in-flight request */
    /* The length of the full IV used by GCM must be multiple of the block size.
     * When the length is equal to 12 (TLS case) a padding of 0s is
     * automatically added, but you need to reserve memory for the entire block.
     * That's why EVP_CIPHER_CTX_iv_length(ctx) returns 16 */
    qctx->iv = qaeCryptoMemAlloc(EVP_CIPHER_CTX_iv_length(ctx), __FILE__, __LINE__);
    if (qctx->iv == NULL) {
        WARN("iv is NULL.\n");
        QATerr(QAT_F_QAT_AES_GCM_INIT, QAT_R_IV_MALLOC_FAILURE);
        goto err;
    }

    /* Set the value of the IV */
    if (NULL != iv) {
        memcpy(qctx->iv, iv, qctx->iv_len);
        memcpy(qctx->next_iv, iv, qctx->iv_len);
        qctx->iv_set = 1;
        DUMPL("iv", iv, qctx->iv_len);
    } else {
        memset(qctx->iv, 0, qctx->iv_len);
        memset(qctx->next_iv, 0, qctx->iv_len);
    }

    DUMPL("inkey", inkey, EVP_CIPHER_CTX_key_length(ctx));

    qctx->tls_aad_len = -1;
    qctx->tag_len = -1;
    qctx->iv_gen = 0;

    /* Initialize QAT session */
    if (0 == qat_session_data_init(ctx, qctx, inkey, iv, enc)) {
        WARN("qat_session_data_init failed.\n");
        goto err;
    }

    return 1;

err:
    if (NULL != qctx->iv) {
        if (qctx->iv != EVP_CIPHER_CTX_iv_noconst(ctx)) {
            qaeCryptoMemFreeNonZero(qctx->iv);
        }
        qctx->iv = NULL;
    }
    if (NULL != qctx->cipher_key) {
        qaeCryptoMemFree(qctx->cipher_key);
        qctx->cipher_key = NULL;
    }
    return 0;
}


/******************************************************************************
 *  * function:
 *            aes_gcm_increment_counter(unsigned char *ifc)
 *
 * @param ifc    [IN,OUT]  - pointer to invocation field counter
 *
 * description:
 *     Increment provided invocation field counter (64-bit int) by 1
 *
 *******************************************************************************/
static inline void qat_aes_gcm_inc_ctr(unsigned char* ifc)
{
    int inv_field_size = 8;
    int i = 0;
    unsigned char byte;

    /* Loop over ifc starting with the least significant byte
     * and work towards the most significant byte of ifc*/
    for (i = inv_field_size - 1; i > 0; --i) {
        byte = ifc[i];

        /* increment by one and copy back to invocation field */
        byte++;
        ifc[i] = byte;

        /* check if incremented invocation field counter wrapped to zero,
         * if greater than zero then break, else continue to loop and
         * increment the next ifc byte */
        if (byte > 0)
            break;
    }
}

/******************************************************************************
* function:
*    qat_aes_gcm_ctrl(EVP_CIPHER_CTX *ctx,
*                     int type, int arg, void *ptr)
*
* @param ctx     [IN]  - pointer to existing context
* @param type    [IN]  - type of request either
*                        EVP_CTRL_AEAD_SET_MAC_KEY or EVP_CTRL_AEAD_TLS1_AAD
* @param arg     [IN]  - size of the pointed to by ptr
* @param ptr     [IN]  - input buffer contain the necessary parameters
*
* @retval x      The return value is dependent on the type of request being made
*                EVP_CTRL_AEAD_SET_MAC_KEY return of 1 is success
*                EVP_CTRL_AEAD_TLS1_AAD return value indicates the amount
*                for padding to be applied to the SSL/TLS record
* @retval -1     function failed
*
* description:
*    This function is a generic control interface provided by the EVP API. For
*  chained requests this interface is used for setting the hmac key value for
*  authentication of the SSL/TLS record.
*  The second type is used to specify the TLS virtual header which is used
*  in the authentication calculation and to identify record payload size.
*
******************************************************************************/
static int qat_aes_gcm_ctrl(EVP_CIPHER_CTX *ctx, int type, int arg, void *ptr)
{
    qat_gcm_ctx *qctx = NULL;
    unsigned int plen = 0;
    int enc = 0;

    if (NULL == ctx) {
        WARN("ctx is NULL.\n");
        QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_CTX_NULL);
        return 0;
    }

    qctx = QAT_GCM_GET_CTX(ctx);

    if (NULL == qctx) {
        WARN("qctx is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_QCTX_NULL);
        return 0;
    }

    enc = EVP_CIPHER_CTX_encrypting(ctx);

    switch (type) {
        case EVP_CTRL_INIT:
            DEBUG("EVP_CTRL_INIT, ctx = %p, type = %d, "
                  "arg = %d, ptr = %p\n", (void*)ctx, type, arg, ptr);
            qctx->key_set = 0;
            qctx->iv_len = 0;
            qctx->iv_set = 0;
            qctx->iv_gen = 0;
            qctx->tls_aad_len = -1;
            qctx->tag_len = -1;
            return 1;

        case EVP_CTRL_GET_IVLEN:
            DEBUG("EVP_CTRL_GCM_GET_IVLEN, ctx = %p, type = %d,"
                  " arg = %d, ptr = %p\n", (void*)ctx, type, arg, ptr);
            *(int*)ptr = qctx->iv_len;
            return 1;

        case EVP_CTRL_GCM_SET_IVLEN:
            DEBUG("EVP_CTRL_GCM_SET_IVLEN, ctx = %p, type = %d,"
                  " arg = %d, ptr = %p\n", (void*)ctx, type, arg, ptr);

            /* At the moment we support only IV with length of 12 Bytes.
             * This is the length of the IV in TLS.
             *
             * If the user wants to use an IV with different length we should
             * implement the function J0 as described in GCM standard
             */
            if (QAT_GCM_TLS_TOTAL_IV_LEN != arg) {
                WARN("Unsupported IV length %d\n", arg);
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_IV_LEN_NOT_SUPPORTED);
                return 0;
            }
            return 1;

        case EVP_CTRL_GCM_SET_TAG:
            DEBUG("EVP_CTRL_GCM_SET_TAG, ctx = %p, type = %d,"
                  " arg = %d, ptr = %p\n", (void*)ctx, type, arg, ptr);

            if (arg <= QAT_GCM_TAG_MIN_LEN || arg > QAT_GCM_TAG_MAX_LEN || enc) {
                WARN("TAG length invalid or invalid operation enc\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_SET_TAG_INVALID_OP);
                return 0;
            }
            if (EVP_GCM_TLS_TAG_LEN != arg) {
                WARN("TAG length %d not supported. Accepted value = 16\n", arg);
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_IV_LEN_NOT_SUPPORTED);
                return 0;
            }

            if (NULL == EVP_CIPHER_CTX_buf_noconst(ctx) || NULL == ptr) {
                WARN("Memory pointer is not valid\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_INVALID_PTR);
                return 0;
            }

            /* ctx->buf is a static buffer of size
             * EVP_MAX_BLOCK_LENGTH = 32
             */
            memcpy(EVP_CIPHER_CTX_buf_noconst(ctx), ptr, arg);
            qctx->tag_len = arg;
            return 1;

        case EVP_CTRL_GCM_GET_TAG:
            DEBUG("EVP_CTRL_GCM_GET_TAG, ctx = %p, type = %d,"
                  " arg = %d, ptr = %p\n", (void*)ctx, type, arg, ptr);
            if (arg <= QAT_GCM_TAG_MIN_LEN || arg > QAT_GCM_TAG_MAX_LEN ||
                !enc || qctx->tag_len <= 0) {
                WARN("TAG length invalid or invalid operation (!enc)\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_SET_TAG_INVALID_OP);
                return 0;
            }
            if (NULL == EVP_CIPHER_CTX_buf_noconst(ctx) || NULL == ptr) {
                WARN("Memory pointer is not valid\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_INVALID_PTR);
                return 0;
            }
            memcpy(ptr, EVP_CIPHER_CTX_buf_noconst(ctx), arg);
            DUMPL("Tag", ptr, arg);
            return 1;

        case EVP_CTRL_GCM_SET_IV_FIXED:
            DEBUG("EVP_CTRL_GCM_SET_IV_FIXED, ctx = %p, type = %d,"
                  " arg = %d, ptr = %p\n", (void*)ctx, type, arg, ptr);
            if (NULL == qctx->next_iv || NULL == ptr) {
                WARN("Memory pointer is not valid\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_INVALID_PTR);
                return 0;
            }

            /* Special case: -1 length restores whole IV */
            if (arg == -1) {
                memcpy(qctx->next_iv, ptr, qctx->iv_len);
                qctx->iv_gen = 1;
                return 1;
            }
            /* Fixed field must be at least 4 bytes (EVP_GCM_TLS_FIXED_IV_LEN)
             * and invocation field at least 8 (EVP_GCM_TLS_EXPLICIT_IV_LEN)
             */
            if ((arg < EVP_GCM_TLS_FIXED_IV_LEN) ||
                (qctx->iv_len - arg) < EVP_GCM_TLS_EXPLICIT_IV_LEN) {
                WARN("IV length invalid\n");
                return 0;
            }

            if (arg != EVP_GCM_TLS_FIXED_IV_LEN) {
                WARN("IV length not supported\n");
                return 0;
            }
            if (arg) {
                memcpy(qctx->next_iv, ptr, arg);
            }
            /* Generate the explicit part of the IV for encryption */
            if (enc &&
                RAND_bytes(qctx->next_iv + arg, qctx->iv_len - arg) <= 0) {
                WARN("Error while generating random explicit IV\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_RAND_FAILURE);
                return 0;
                break;
            }
            qctx->iv_gen = 1;
            return 1;

        case EVP_CTRL_GCM_IV_GEN:
            /* Called in TLS case before encryption */
            DEBUG("EVP_CTRL_GCM_IV_GEN, ctx = %p, type = %d,"
                  " arg = %d, ptr = %p\n", (void*)ctx, type, arg, ptr);
            if (NULL == qctx->iv || NULL == qctx->next_iv || NULL == ptr) {
                WARN("Memory pointer is not valid\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_IV_NULL_PTR_INVALID);
                return 0;
            }
            if (0 == qctx->iv_gen) {
                WARN("Operation is not valid\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_IV_GEN_INVALID);
                return 0;
            }

            /* Set the IV that will be used in the current operation */
            memcpy(qctx->iv, qctx->next_iv, qctx->iv_len);
            if (arg <= 0 || arg > qctx->iv_len) {
                arg = qctx->iv_len;
            }

            /* Copy the explicit IV in the output buffer */
            memcpy(ptr, qctx->next_iv + qctx->iv_len - arg, arg);

            /* Invocation field will be at least 8 bytes in size and
             * so no need to check wrap around or increment more than
             * last 8 bytes.
             */
            qat_aes_gcm_inc_ctr(qctx->next_iv + qctx->iv_len - 8);
            qctx->iv_set = 1;
            return 1;

        case EVP_CTRL_GCM_SET_IV_INV:
            /* Called in TLS case before decryption */
            DEBUG("EVP_CTRL_GCM_SET_IV_INV, ctx = %p, type = %d,"
                  " arg = %d, ptr = %p\n", (void*)ctx, type, arg, ptr);
            if (0 == qctx->iv_gen || enc) {
                WARN("Operation is not valid\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_IV_NVALID);
                return 0;
            }
            if (NULL == qctx->iv || NULL == qctx->next_iv || NULL == ptr) {
                WARN("Memory pointer is not valid\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_IV_INVALID);
                return 0;
            }
            /* Retrieve the explicit IV from the message buffer */
            memcpy(qctx->next_iv + qctx->iv_len - arg, ptr, arg);

            /* Set the IV that will be used in the current operation */
            memcpy(qctx->iv, qctx->next_iv, qctx->iv_len);
            qctx->iv_set = 1;
            return 1;

        case EVP_CTRL_AEAD_TLS1_AAD:
            DEBUG("EVP_CTRL_AEAD_TLS1_AAD, ctx = %p, type = %d,"
                  " arg = %d, ptr = %p\n", (void*)ctx, type, arg, ptr);
            if (TLS_VIRT_HDR_SIZE != arg) {
                WARN("AAD length is not valid\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_AAD_LEN_INVALID);
                return 0;
            }

            /* Allocate the memory only the first time */
            if (qctx->tls_aad_len < 0) {
                int aad_buffer_len = TLS_VIRT_HDR_SIZE;
                DEBUG("Allocating memory for AAD in TLS sync\n");
                /* For QAT the length of the buffer for AAD must be multiple
                 * of block size */
                if (aad_buffer_len % AES_BLOCK_SIZE) {
                    aad_buffer_len += AES_BLOCK_SIZE - (aad_buffer_len % AES_BLOCK_SIZE);
                    DEBUG("Adjusting AAD buffer length = %d\n", aad_buffer_len);
                }
                qctx->aad = qaeCryptoMemAlloc(aad_buffer_len, __FILE__, __LINE__);
                if (NULL == qctx->aad) {
                    WARN("Unable to allocate memory for TLS header\n");
                    QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_AAD_MALLOC_FAILURE);
                    return 0;
                }

                /* Set the flag to mark the TLS case */
                qctx->tls_aad_len = TLS_VIRT_HDR_SIZE;

                /* Set the length of the AAD in the session
                 * The session hasn't been initialized yet here and this value
                 * should never change in the TLS case */
                qctx->session_data->hashSetupData.authModeSetupData.aadLenInBytes = TLS_VIRT_HDR_SIZE;
            }

            if (NULL == qctx->aad || NULL == ptr) {
                WARN("Memory pointer is not valid\n");
                QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_AAD_INVALID_PTR);
                return 0;
            }

            /* Copy the header from p into the buffer */
            memcpy(qctx->aad, ptr, TLS_VIRT_HDR_SIZE);

            /* Extract the length of the payload from the TLS header */
            plen = qctx->aad[arg - QAT_GCM_TLS_PAYLOADLENGTH_MSB_OFFSET]
                    << QAT_BYTE_SHIFT |
                    qctx->aad[arg - QAT_GCM_TLS_PAYLOADLENGTH_LSB_OFFSET];

            DEBUG("IN plen = %d\n", plen);
            DUMPL("IN qctx->aad", qctx->aad, TLS_VIRT_HDR_SIZE);

            /* The payload contains the explicit IV -> correct the length */
            plen -= EVP_GCM_TLS_EXPLICIT_IV_LEN;

            /* If decrypting correct for tag too */
            if (!enc) {
                plen -= EVP_GCM_TLS_TAG_LEN;
            }

            /* Fix the length like in the SW version of GCM */
            qctx->aad[TLS_VIRT_HDR_SIZE - QAT_GCM_TLS_PAYLOADLENGTH_MSB_OFFSET]
              = plen >> QAT_BYTE_SHIFT;
            qctx->aad[TLS_VIRT_HDR_SIZE - QAT_GCM_TLS_PAYLOADLENGTH_LSB_OFFSET]
              = plen;

            DEBUG("OUT plen = %d\n", plen);
            DUMPL("OUT qctx->aad", qctx->aad, TLS_VIRT_HDR_SIZE);

            /* Return the length of the TAG */
            return EVP_GCM_TLS_TAG_LEN;

        default:
            WARN("Invalid type %d\n", type);
            QATerr(QAT_F_QAT_AES_GCM_CTRL, QAT_R_INVALID_CTRL_TYPE);
            return -1;
    }
}

/******************************************************************************
* function:
*    qat_aes_gcm_cleanup(EVP_CIPHER_CTX *ctx)
*
* @param ctx    [IN]  - pointer to existing context
*
* @retval 1      function succeeded
* @retval 0      function failed
*
* description:
*    This function will cleanup all allocated resources required to perform the
*  cryptographic transform.
*
******************************************************************************/
static int qat_aes_gcm_cleanup(EVP_CIPHER_CTX *ctx)
{
    qat_gcm_ctx* qctx = NULL;
    CpaStatus sts = 0;
    CpaCySymSessionSetupData* session_data = NULL;
    int ret_val = 1;

    DEBUG("- Entering\n");

    if (NULL == ctx) {
        WARN("ctx is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_CLEANUP, QAT_R_CTX_NULL);
        return 0;
    }

    qctx = QAT_GCM_GET_CTX(ctx);

    if (NULL == qctx) {
        WARN("qctx is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_CLEANUP, QAT_R_QCTX_NULL);
        return 0;
    }

    if (0 == qctx->is_session_init) {
        /* It is valid to call cleanup even if the context has not been
         * initialised. */
        return ret_val;
    }

    session_data = qctx->session_data;
    if (session_data) {
        /* Remove the session */
        if (qctx->qat_ctx) {
            if ((sts = cpaCySymRemoveSession(qat_instance_handles[qctx->inst_num], qctx->qat_ctx))
               != CPA_STATUS_SUCCESS) {
                WARN("cpaCySymRemoveSession FAILED, sts = %d.!\n", sts);
                ret_val = 0;
                /* Lets not return yet and instead make a best effort to
                 * cleanup the rest to avoid memory leaks
                 */
            }
            qaeCryptoMemFreeNonZero(qctx->qat_ctx);
            qctx->qat_ctx = NULL;
        }

        /* Cleanup the memory */
        if (qctx->aad) {
            qaeCryptoMemFreeNonZero(qctx->aad);
            qctx->aad = NULL;
        }
        if (qctx->srcBufferList.pPrivateMetaData) {
            qaeCryptoMemFreeNonZero(qctx->srcBufferList.pPrivateMetaData);
            qctx->srcBufferList.pPrivateMetaData = NULL;
        }
        if (qctx->dstBufferList.pPrivateMetaData) {
            qaeCryptoMemFreeNonZero(qctx->dstBufferList.pPrivateMetaData);
            qctx->dstBufferList.pPrivateMetaData = NULL;
        }
        if (qctx->iv) {
            qaeCryptoMemFreeNonZero(qctx->iv);
            qctx->iv = NULL;
        }
        if (qctx->cipher_key) {
            qaeCryptoMemFree(qctx->cipher_key);
            qctx->cipher_key = NULL;
        }
        session_data->cipherSetupData.pCipherKey = NULL;
        OPENSSL_clear_free(session_data, sizeof(CpaCySymSessionSetupData));
    }
    qctx->is_session_init = 0;
    return ret_val;
}

/******************************************************************************
 *  * function:
 *
 * static void qat_gcm_cb(void *pCallbackTag, CpaStatus status,
 *                        const CpaCySymOp operationType,
 *                        void *pOpData, CpaBufferList *pDstBuffer,
 *                        CpaBoolean verifyResult)
 *
 * @param pCallbackTag  [IN] -  Opaque value provided by user while making
 *                              individual function call. Cast to op_done_pipe_t.
 * @param status        [IN] -  Status of the operation.
 * @param operationType [IN] -  Identifies the operation type requested.
 * @param pOpData       [IN] -  Pointer to structure with input parameters.
 * @param pDstBuffer    [IN] -  Destination buffer to hold the data output.
 * @param verifyResult  [IN] -  Used to verify digest result.
 *
 * description:
 Callback to indicate the completion of crypto operation
 ******************************************************************************/
static void qat_gcm_cb(void *pCallbackTag, CpaStatus status,
                       const CpaCySymOp operationType,
                       void *pOpData, CpaBufferList *pDstBuffer,
                       CpaBoolean verifyResult)
{
    if (enable_heuristic_polling) {
        QAT_ATOMIC_DEC(num_cipher_pipeline_requests_in_flight);
    }
    qat_crypto_callbackFn(pCallbackTag, status, CPA_CY_SYM_OP_CIPHER, pOpData,
                          NULL, CPA_TRUE);
}

/******************************************************************************
* function:
*         qat_aes_gcm_session_init(EVP_CIPHER_CTX *ctx)
*
* @param ctx [IN] - pointer to context
*
* @retval 1      function succeeded
* @retval 0      function failed
*
* description:
*    This function synchronises the initialisation of the QAT session and
*  pre-allocates the necessary buffers for the session.
******************************************************************************/
static int qat_aes_gcm_session_init(EVP_CIPHER_CTX *ctx)
{
    qat_gcm_ctx* qctx = NULL;
    CpaCySymSessionSetupData *sessionSetupData = NULL;
    Cpa32U sessionCtxSize = 0;
    CpaCySymSessionCtx pSessionCtx = NULL;
    const unsigned int NUM_BUFFERS = 1;

    DEBUG("- Entering\n");

    if (NULL == ctx) {
        WARN("parameter ctx is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_SESSION_INIT, QAT_R_CTX_NULL);
        return 0;
    }

    qctx = QAT_GCM_GET_CTX(ctx);

    if (NULL == qctx) {
        WARN("qctx is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_SESSION_INIT, QAT_R_QCTX_NULL);
        return 0;
    }

    /* All parameters have not been set yet or we have already been
     * initialised. */
    if ((1 != qctx->init_params_set) || (1 == qctx->is_session_init)) {
        WARN("Parameters not set or session already initialised\n");
        return 0;
    }

    sessionSetupData = qctx->session_data;
    if (NULL == sessionSetupData) {
        WARN("sessionSetupData is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_SESSION_INIT, QAT_R_SSD_NULL);
        return 0;
    }

    qctx->inst_num = get_next_inst_num();
    if (qctx->inst_num == QAT_INVALID_INSTANCE) {
        WARN("Failed to get QAT Instance Handle\n");
        QATerr(QAT_F_QAT_AES_GCM_SESSION_INIT, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    if (cpaCySymSessionCtxGetSize(qat_instance_handles[qctx->inst_num],
                                  sessionSetupData,
                                  &sessionCtxSize) != CPA_STATUS_SUCCESS) {
        WARN("cpaCySymSessionCtxGetSize failed.\n");
        QATerr(QAT_F_QAT_AES_GCM_SESSION_INIT, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    pSessionCtx = (CpaCySymSessionCtx) qaeCryptoMemAlloc(sessionCtxSize,
                                                         __FILE__, __LINE__);
    if (NULL == pSessionCtx) {
        WARN("pSessionCtx malloc failed\n");
        QATerr(QAT_F_QAT_AES_GCM_SESSION_INIT, ERR_R_INTERNAL_ERROR);
        return 0;
    }

    DUMP_SESSION_SETUP_DATA(sessionSetupData);
    if (cpaCySymInitSession(qat_instance_handles[qctx->inst_num],
        qat_gcm_cb,
        sessionSetupData,
        pSessionCtx) != CPA_STATUS_SUCCESS) {
        WARN("cpaCySymInitSession failed.\n");
        QATerr(QAT_F_QAT_AES_GCM_SESSION_INIT, ERR_R_INTERNAL_ERROR);
        qaeCryptoMemFree(pSessionCtx);
        return 0;
    }
    qctx->qat_ctx = pSessionCtx;

    /* Setup meta data for buffer lists */
    if (cpaCyBufferListGetMetaSize(qat_instance_handles[qctx->inst_num],
                                   NUM_BUFFERS,
                                   &(qctx->meta_size))
                                   != CPA_STATUS_SUCCESS) {
        WARN("cpaCyBufferListGetBufferSize failed.\n");
        QATerr(QAT_F_QAT_AES_GCM_SESSION_INIT, ERR_R_INTERNAL_ERROR);
        qaeCryptoMemFree(pSessionCtx);
        return 0;
    }

    qctx->srcBufferList.numBuffers = NUM_BUFFERS;
    qctx->srcBufferList.pBuffers = qctx->srcFlatBuffer;
    qctx->srcBufferList.pUserData = NULL;

    qctx->dstBufferList.numBuffers = NUM_BUFFERS;
    qctx->dstBufferList.pBuffers = qctx->dstFlatBuffer;
    qctx->dstBufferList.pUserData = NULL;


    if (qctx->meta_size) {
        qctx->srcBufferList.pPrivateMetaData =
            qaeCryptoMemAlloc(qctx->meta_size, __FILE__, __LINE__);
        if (NULL == qctx->srcBufferList.pPrivateMetaData) {
            WARN("srcBufferList.pPrivateMetaData is NULL.\n");
            QATerr(QAT_F_QAT_AES_GCM_SESSION_INIT, ERR_R_INTERNAL_ERROR);
            qaeCryptoMemFreeNonZero(pSessionCtx);
            return 0;
        }

        qctx->dstBufferList.pPrivateMetaData =
            qaeCryptoMemAlloc(qctx->meta_size, __FILE__, __LINE__);
        if (NULL == qctx->dstBufferList.pPrivateMetaData) {
            WARN("dstBufferList.pPrivateMetaData is NULL.\n");
            QATerr(QAT_F_QAT_AES_GCM_SESSION_INIT, ERR_R_INTERNAL_ERROR);
            qaeCryptoMemFreeNonZero(qctx->srcBufferList.pPrivateMetaData);
            qctx->srcBufferList.pPrivateMetaData = NULL;
            qaeCryptoMemFreeNonZero(pSessionCtx);
            return 0;
        }
    } else {
        qctx->srcBufferList.pPrivateMetaData = NULL;
        qctx->dstBufferList.pPrivateMetaData = NULL;
    }

    /* Create the OpData structure to remove this processing from the data
     * path */
    qctx->OpData.sessionCtx = qctx->qat_ctx;
    qctx->OpData.packetType = CPA_CY_SYM_PACKET_TYPE_FULL;

    /* Set the IV */
    /* Set the AAD */
    qctx->OpData.pAdditionalAuthData = qctx->aad;
    DUMPL("qctx->OpData.pAdditionalAuthData",
           qctx->OpData.pAdditionalAuthData,
           qctx->session_data->hashSetupData.authModeSetupData.aadLenInBytes);

    /* All the data in the buffer must be encrypted */
    qctx->OpData.cryptoStartSrcOffsetInBytes = 0;

    /* Following parameters are ignored in GCM */
    qctx->OpData.messageLenToHashInBytes = 0;
    qctx->OpData.hashStartSrcOffsetInBytes = 0;

    qctx->is_session_init = 1;
    return 1;
}

/******************************************************************************
* function:
*    qat_aes_gcm_tls_cipher(EVP_CIPHER_CTX *ctx, unsigned char *out,
*                           const unsigned char *in, size_t len)
*
* @param ctx [IN]  - pointer to existing context
* @param out     [OUT] - output buffer for transform result
* @param in      [IN]  - input buffer
* @param len     [IN]  - length of input buffer
*
* @retval 0      function failed
* @retval 1      function succeeded
*
* description:
*    This function performs the cryptographic transform according to the
*  parameters setup during initialisation.
*
*  This is the function used in the TLS case.
*
******************************************************************************/
int qat_aes_gcm_tls_cipher(EVP_CIPHER_CTX *ctx, unsigned char *out,
                           const unsigned char *in, size_t len)
{
    CpaStatus sts = 0;
    op_done_t op_done;
    qat_gcm_ctx *qctx = NULL;
    int ret_val = -1;
    int job_ret = 0;
    int enc = 0;
    unsigned int message_len = 0;
    unsigned int buffer_len = 0;
    thread_local_variables_t *tlv = NULL;

    CRYPTO_QAT_LOG("CIPHER - %s\n", __func__);

    /* Encrypt/decrypt must be performed in place */
    if (NULL == in ||
        out != in ||
        len < (EVP_GCM_TLS_EXPLICIT_IV_LEN + EVP_GCM_TLS_TAG_LEN)) {
        WARN("Input parameters are not valid.\n");
        QATerr(QAT_F_QAT_AES_GCM_TLS_CIPHER, QAT_R_INVALID_LEN);
        return -1;
    }

    if (NULL == ctx) {
        WARN("ctx is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_TLS_CIPHER, QAT_R_CTX_NULL);
        return -1;
    }

    qctx = QAT_GCM_GET_CTX(ctx);
    if (NULL == qctx) {
        WARN("qctx is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_TLS_CIPHER, QAT_R_QCTX_NULL);
        return -1 ;
    }

    enc = EVP_CIPHER_CTX_encrypting(ctx);
    DEBUG("enc = %d - ctx = %p, out = %p, in = %p, len = %zu\n",
           enc, (void*)ctx, (void*)out, (void*)in, len);

    /* The key has been set in the init function: no need to check it here*/

    /* Initialize the session if not done before */
    if (0 == qctx->is_session_init) {
        if (0 == qat_aes_gcm_session_init(ctx)) {
            WARN("Unable to initialise Cipher context.\n");
            goto err;
        }
    }

    /* Encryption: generate explicit IV and write to start of buffer.
     * Decryption: read the explicit IV from start of buffer
     */
    if (EVP_CIPHER_CTX_ctrl(ctx, enc ?
        EVP_CTRL_GCM_IV_GEN : EVP_CTRL_GCM_SET_IV_INV,
        EVP_GCM_TLS_EXPLICIT_IV_LEN, out) <= 0) {
        goto err;
    }
    DUMPL("Post ctrl IV: ", qctx->iv, qctx->iv_len);
    DUMPL("Post ctrl next IV: ", qctx->next_iv, qctx->iv_len);

    DUMPL("qctx->OpData.pAdditionalAuthData",
          qctx->OpData.pAdditionalAuthData,
          qctx->session_data->hashSetupData.authModeSetupData.aadLenInBytes);

    /* Set the generated IV to OpData */
    if (qctx->iv_set) {
        qctx->OpData.pIv = qctx->iv;
        qctx->OpData.ivLenInBytes = qctx->iv_len;
    }

    /* If key or IV not set, throw error here and return. */
    if (!qctx->key_set || !qctx->iv_set) {
        WARN("Cipher key or IV not set.\n");
        QATerr(QAT_F_QAT_AES_GCM_TLS_CIPHER, QAT_R_KEY_IV_NOT_SET);
        goto err;
    }

    /* Include the explicit part of the IV at the beginning of the output  */
    in += EVP_GCM_TLS_EXPLICIT_IV_LEN;
    out += EVP_GCM_TLS_EXPLICIT_IV_LEN;

    /* This is the length of the message that must be encrypted */
    message_len = len - (EVP_GCM_TLS_EXPLICIT_IV_LEN + EVP_GCM_TLS_TAG_LEN);

    /* The buffer must have enough memory to save also the TAG */
    buffer_len = message_len + EVP_GCM_TLS_TAG_LEN;

    /* Build request/response buffers */
    /* Allocate the memory of the FlatBuffer and copy the payload */
    qctx->srcFlatBuffer[0].pData = qaeCryptoMemAlloc(buffer_len, __FILE__, __LINE__);
    if (NULL == qctx->srcFlatBuffer[0].pData) {
        WARN("src/dst buffer allocation.\n");
        goto err;
    }
    qctx->dstFlatBuffer[0].pData = qctx->srcFlatBuffer[0].pData;
    if (!enc) {
        /* Decryption: the tag is appended and must be copied to the buffer */
        memcpy(qctx->srcFlatBuffer[0].pData, in, buffer_len);
        qctx->tag_len = EVP_GCM_TLS_TAG_LEN;
    } else {
        /* Encryption: copy only the payload */
        memcpy(qctx->srcFlatBuffer[0].pData, in, message_len);
    }

    /* The operation is done in place in the buffers
     * The variables in and out remain separate */
    qctx->srcFlatBuffer[0].dataLenInBytes = buffer_len;
    qctx->srcBufferList.pUserData = NULL;
    qctx->dstFlatBuffer[0].dataLenInBytes = buffer_len;
    qctx->dstBufferList.pUserData = NULL;

    qctx->OpData.messageLenToCipherInBytes = message_len;

    tlv = qat_check_create_local_variables();
    if (NULL == tlv) {
            WARN("could not create local variables\n");
            QATerr(QAT_F_QAT_AES_GCM_TLS_CIPHER, ERR_R_INTERNAL_ERROR);
            goto err;
    }

    QAT_INC_IN_FLIGHT_REQS(num_requests_in_flight, tlv);
    if (qat_use_signals()) {
        if (tlv->localOpsInFlight == 1) {
            if (qat_kill_thread(qat_timer_poll_func_thread, SIGUSR1) != 0) {
                WARN("qat_kill_thread error\n");
                QATerr(QAT_F_QAT_AES_GCM_TLS_CIPHER, ERR_R_INTERNAL_ERROR);
                QAT_DEC_IN_FLIGHT_REQS(num_requests_in_flight, tlv);
                goto err;
            }
        }
    }

    qat_init_op_done(&op_done);
    if (op_done.job != NULL) {
        if (qat_setup_async_event_notification(0) == 0) {
            WARN("Failure to setup async event notifications\n");
            QATerr(QAT_F_QAT_AES_GCM_TLS_CIPHER, ERR_R_INTERNAL_ERROR);
            qat_cleanup_op_done(&op_done);
            QAT_DEC_IN_FLIGHT_REQS(num_requests_in_flight, tlv);
            goto err;
        }
    }

    CRYPTO_QAT_LOG("CIPHER - %s\n", __func__);
    DUMP_SYM_PERFORM_OP_GCM(qat_instance_handles[qctx->inst_num],
                            qctx->OpData, qctx->srcBufferList,
                            qctx->dstBufferList);

    sts = qat_sym_perform_op(qctx->inst_num,
                             &op_done,
                             &(qctx->OpData),
                             &(qctx->srcBufferList),
                             &(qctx->dstBufferList),
                             &(qctx->session_data->verifyDigest));
    if (sts != CPA_STATUS_SUCCESS) {
        qaeCryptoMemFreeNonZero(qctx->srcFlatBuffer[0].pData);
        qctx->srcFlatBuffer[0].pData = NULL;
        qctx->dstFlatBuffer[0].pData = NULL;
        QAT_DEC_IN_FLIGHT_REQS(num_requests_in_flight, tlv);
        qat_cleanup_op_done(&op_done);
        WARN("cpaCySymPerformOp failed sts=%d.\n",sts);
        QATerr(QAT_F_QAT_AES_GCM_TLS_CIPHER, ERR_R_INTERNAL_ERROR);
        goto err;
    }

    if (enable_heuristic_polling) {
        QAT_ATOMIC_INC(num_cipher_pipeline_requests_in_flight);
    }

    do {
        if (op_done.job != NULL) {
            /* If we get a failure on qat_pause_job then we will
               not flag an error here and quit because we have
               an asynchronous request in flight.
               We don't want to start cleaning up data
               structures that are still being used. If
               qat_pause_job fails we will just yield and
               loop around and try again until the request
               completes and we can continue. */
            if ((job_ret = qat_pause_job(op_done.job, ASYNC_STATUS_OK)) == 0)
                pthread_yield();
        } else {
            pthread_yield();
        }
    } while (!op_done.flag ||
            QAT_CHK_JOB_RESUMED_UNEXPECTEDLY(job_ret));

    if (enc) {
        ret_val = len;
        DEBUG("Encryption succeeded\n");
    } else if (CPA_TRUE == op_done.verifyResult) {
        ret_val = message_len;
        DEBUG("Decryption succeeded\n");
    } else {
        DEBUG("Decryption failed\n");
    }

    DUMP_SYM_PERFORM_OP_GCM_OUTPUT(qctx->dstBufferList);
    QAT_DEC_IN_FLIGHT_REQS(num_requests_in_flight, tlv);

    qat_cleanup_op_done(&op_done);

    memcpy(out, qctx->dstFlatBuffer[0].pData, message_len + EVP_GCM_TLS_TAG_LEN);
    qaeCryptoMemFreeNonZero(qctx->srcFlatBuffer[0].pData);
    qctx->srcFlatBuffer[0].pData = NULL;
    qctx->dstFlatBuffer[0].pData = NULL;

err:
    /* Don't reuse the IV */
    qctx->iv_set = 0;
    DEBUG("Function result = %d\n",ret_val);
    return ret_val;
}

/******************************************************************************
* function:
*    qat_aes_gcm_cipher(EVP_CIPHER_CTX *ctx, unsigned char *out,
*                       const unsigned char *in, size_t len)
*
* @param ctx    [IN]  - pointer to existing context
* @param out        [OUT] - output buffer for transform result
* @param in         [IN]  - input buffer
* @param len        [IN]  - length of input buffer
*
* @retval -1      function failed
* @retval len     function succeeded
*
* description:
*    This function performs the cryptographic transform according to the
*  parameters setup during initialisation.
*
*  This is the implementation of the Update case: the AAD is added by calling
*  UpdateEncrypt() with out == NULL.
*  This is the case used in the speed test.
*
*  The flag EVP_CIPH_FLAG_CUSTOM_CIPHER is required to have the Update function
*  working correctly when out == NULL, that is when setting the AAD.
*  When this flag is enabled, the function must return -1 in case of failure and
*  the length of the output (value >= 0) for success.
*
*  QAT vs SW Engine
*  ================
*
*  In the Update use case, the behavior of the functions is different between
*  the QAT and the SW engine.
*
*  EVP Function     SW Engine                       QAT Engine
*  ----------------------------------------------------------------------------
*  Encrypt Update   Encrypt the payload             Encrypt the payload AND
*                                                   compute the tag
*
*  Encrypt Final    Compute the tag                 Does nothing
*
*  Decrypt Update   Decrypt the payload             Decrpyt the payload and
*                                                   verify the TAG. Return failure
*                                                   if the TAG is not correct
*
*  Decrypt Final    Verify the TAG and              Does nothing
*                   return failure if not correct
*
*  This doesn't impact the TLS case because Update and Final are considered
*  a single operation like in the QAT engine.
*
******************************************************************************/
int qat_aes_gcm_cipher(EVP_CIPHER_CTX *ctx, unsigned char *out,
                       const unsigned char *in, size_t len)
{
    CpaStatus sts = 0;
    op_done_t op_done;
    qat_gcm_ctx *qctx = NULL;
    const int RET_SUCCESS = 0;
    const int RET_FAIL = -1;
    int ret_val = RET_FAIL;
    int job_ret = 0;
    int enc = 0;
    size_t aad_len = 0;
    int aad_buffer_len = 0;
    unsigned buffer_len = 0;
    thread_local_variables_t *tlv = NULL;

    CRYPTO_QAT_LOG("CIPHER - %s\n", __func__);

    if (NULL == ctx) {
        WARN("ctx is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_CIPHER, QAT_R_CTX_NULL);
        return RET_FAIL;
    }

    qctx = QAT_GCM_GET_CTX(ctx);
    if (NULL == qctx) {
        WARN("qctx is NULL\n");
        QATerr(QAT_F_QAT_AES_GCM_CIPHER, QAT_R_QCTX_NULL);
        return RET_FAIL;
    }

    enc = EVP_CIPHER_CTX_encrypting(ctx);
    DEBUG("enc = %d - ctx = %p, out = %p, in = %p, len = %zu\n",
          enc, (void*)ctx, (void*)out, (void*)in, len);

    /* Distinguish the Update and TLS case */
    if (qctx->tls_aad_len >= 0) {
        return qat_aes_gcm_tls_cipher(ctx, out, in, len);
    }

    /* If either key or IV not set, throw error here. */
    if (!qctx->key_set || !qctx->iv_set) {
        WARN("Cipher key or IV not set.\n");
        QATerr(QAT_F_QAT_AES_GCM_CIPHER, QAT_R_KEY_IV_NOT_SET);
        return RET_FAIL;
    }

    if (in) {
        /* This is called when doing Update */
        if (NULL == out) {
            DEBUG("in != NULL && out == NULL -> Adding AAD\n");
            aad_len = len;

            /* Check if the length of the AAD has changed */
            if (qctx->session_data->hashSetupData.authModeSetupData.aadLenInBytes != aad_len) {
                /* Free the memory used for the previous AAD */
                if (qctx->aad) {
                    qaeCryptoMemFreeNonZero(qctx->aad);
                    qctx->aad = NULL;
                }
                /* For QAT the length of the buffer for AAD must be multiple of block size */
                aad_buffer_len = aad_len;
                if (aad_buffer_len % AES_BLOCK_SIZE) {
                    aad_buffer_len += AES_BLOCK_SIZE - (aad_buffer_len % AES_BLOCK_SIZE);
                    DEBUG("Adjusting AAD buffer length = %d\n", aad_buffer_len);
                }
                qctx->aad = qaeCryptoMemAlloc(aad_buffer_len, __FILE__, __LINE__);
                if (NULL == qctx->aad) {
                    WARN("Unable to allocate memory for AAD\n");
                    QATerr(QAT_F_QAT_AES_GCM_CIPHER, ERR_R_INTERNAL_ERROR);
                    return RET_FAIL ;
                }

                /* Set the length of the AAD */
                qctx->session_data->hashSetupData.authModeSetupData.aadLenInBytes = aad_len;
            }

            memcpy(qctx->aad, in, aad_len);
            DUMPL("qctx->aad", qctx->aad, aad_len);

            /* In this case no data is actually encrypted/decrypted.
             * The return value follows the standard rule of OpenSSL: success -> 1
             */
            return 1;
        } else {
            /* The key has been set in the init function: no need to check it */
            if (0 == qctx->is_session_init) {
                if (0 == qat_aes_gcm_session_init(ctx)) {
                    WARN("Unable to initialise Cipher context.\n");
                    return RET_FAIL ;
                }
            }

            buffer_len = len + EVP_GCM_TLS_TAG_LEN;
            /* Build request/response buffers */
            /* Allocate the memory of the FlatBuffer and copy the payload */
            qctx->srcFlatBuffer[0].pData = qaeCryptoMemAlloc(buffer_len, __FILE__, __LINE__);
            if (NULL == qctx->srcFlatBuffer[0].pData) {
                WARN("src/dst buffer allocation.\n");
                QATerr(QAT_F_QAT_AES_GCM_CIPHER, ERR_R_INTERNAL_ERROR);
                return RET_FAIL;
            }
            qctx->dstFlatBuffer[0].pData = qctx->srcFlatBuffer[0].pData;
            memcpy(qctx->srcFlatBuffer[0].pData, in, len);

            /* The operation is done in place in the buffers
             * The variables in and out remain separate */
            qctx->srcFlatBuffer[0].dataLenInBytes = buffer_len;
            qctx->srcBufferList.pUserData = NULL;
            qctx->dstFlatBuffer[0].dataLenInBytes = buffer_len;
            qctx->dstBufferList.pUserData = NULL;

            qctx->OpData.messageLenToCipherInBytes = len;

            /* Decryption: set the digest (tag) for verification
             * This is different from SW implementation. Here we have a single
             * function to decrypt AND verify
            */
            if (!enc) {
                /* Check that a tag has been set */
                if (NULL == EVP_CIPHER_CTX_buf_noconst(ctx)) {
                    WARN("Tag not set\n");
                    qaeCryptoMemFreeNonZero(qctx->srcFlatBuffer[0].pData);
                    qctx->srcFlatBuffer[0].pData = NULL;
                    qctx->dstFlatBuffer[0].pData = NULL;
                    return RET_FAIL;
                }
                /* Copy EVP_GCM_TLS_TAG_LEN bytes from tag buffer
                   as the maximum tag length can only be
                   EVP_GCM_TLS_TAG_LEN */
                memcpy(qctx->dstFlatBuffer[0].pData + len,
                       EVP_CIPHER_CTX_buf_noconst(ctx),
                       EVP_GCM_TLS_TAG_LEN);
            }

            tlv = qat_check_create_local_variables();
            if (NULL == tlv) {
                WARN("could not create local variables\n");
                QATerr(QAT_F_QAT_AES_GCM_CIPHER, ERR_R_INTERNAL_ERROR);
                return RET_FAIL;
            }

            QAT_INC_IN_FLIGHT_REQS(num_requests_in_flight, tlv);
            if (qat_use_signals()) {
                if (tlv->localOpsInFlight == 1) {
                    if (qat_kill_thread(qat_timer_poll_func_thread, SIGUSR1) != 0) {
                        WARN("qat_kill_thread error\n");
                        QATerr(QAT_F_QAT_AES_GCM_CIPHER, ERR_R_INTERNAL_ERROR);
                        QAT_DEC_IN_FLIGHT_REQS(num_requests_in_flight, tlv);
                        return RET_FAIL;
                    }
                }
            }

            qat_init_op_done(&op_done);
            if (op_done.job != NULL) {
                if (qat_setup_async_event_notification(0) == 0) {
                    WARN("Failure to setup async event notifications\n");
                    QATerr(QAT_F_QAT_AES_GCM_CIPHER, ERR_R_INTERNAL_ERROR);
                    QAT_DEC_IN_FLIGHT_REQS(num_requests_in_flight, tlv);
                    qat_cleanup_op_done(&op_done);
                    return RET_FAIL;
                }
            }

            DUMP_SYM_PERFORM_OP_GCM(qat_instance_handles[qctx->inst_num],
                                    qctx->OpData, qctx->srcBufferList,
                                    qctx->dstBufferList);

            sts = qat_sym_perform_op(qctx->inst_num,
                                     &op_done,
                                     &(qctx->OpData),
                                     &(qctx->srcBufferList),
                                     &(qctx->dstBufferList),
                                     &(qctx->session_data->verifyDigest));
            if (sts != CPA_STATUS_SUCCESS) {
                qaeCryptoMemFreeNonZero(qctx->srcFlatBuffer[0].pData);
                qctx->srcFlatBuffer[0].pData = NULL;
                qctx->dstFlatBuffer[0].pData = NULL;
                QAT_DEC_IN_FLIGHT_REQS(num_requests_in_flight, tlv);
                qat_cleanup_op_done(&op_done);
                WARN("cpaCySymPerformOp failed sts=%d.\n",sts);
                QATerr(QAT_F_QAT_AES_GCM_CIPHER, ERR_R_INTERNAL_ERROR);
                return RET_FAIL;
            }

            if (enable_heuristic_polling) {
                QAT_ATOMIC_INC(num_cipher_pipeline_requests_in_flight);
            }

            do {
                if (op_done.job != NULL) {
                    /* If we get a failure on qat_pause_job then we will
                       not flag an error here and quit because we have
                       an asynchronous request in flight.
                       We don't want to start cleaning up data
                       structures that are still being used. If
                       qat_pause_job fails we will just yield and
                       loop around and try again until the request
                       completes and we can continue. */
                    if ((job_ret = qat_pause_job(op_done.job, 0)) == 0)
                        pthread_yield();
                } else {
                    pthread_yield();
                }
            } while (!op_done.flag ||
                     QAT_CHK_JOB_RESUMED_UNEXPECTEDLY(job_ret));

            DUMP_SYM_PERFORM_OP_GCM_OUTPUT(qctx->dstBufferList);

            if (enc) {
                ret_val = len;
                DEBUG("Encryption succeeded\n");
            } else if (CPA_TRUE == op_done.verifyResult) {
                ret_val = len;
                DEBUG("Decryption succeeded\n");
            }

            QAT_DEC_IN_FLIGHT_REQS(num_requests_in_flight, tlv);
            qat_cleanup_op_done(&op_done);

            if (enc) {
                /* After encryption, copy the TAG from the buffer to the ctx */
                memcpy(EVP_CIPHER_CTX_buf_noconst(ctx),
                       qctx->dstFlatBuffer[0].pData + len,
                       EVP_GCM_TLS_TAG_LEN);
                qctx->tag_len = EVP_GCM_TLS_TAG_LEN;
            }

            memcpy(out, qctx->dstFlatBuffer[0].pData, len);
            qaeCryptoMemFreeNonZero(qctx->srcFlatBuffer[0].pData);
            qctx->srcFlatBuffer[0].pData = NULL;
            qctx->dstFlatBuffer[0].pData = NULL;

            return ret_val;
        }
    } else {
        /* This is executed when Final is called */
        if (!enc) {
            if (qctx->tag_len < 0)
                return RET_FAIL;
            /* Don't reuse the IV */
            qctx->iv_set = 0;
            DEBUG("Decrypt Final()\n");
            /* The SW implem here compares the TAGs and returns -1 if they are different.
             * Now the TAGs are checked when decrypting the payload so Final always return success
             */
            return RET_SUCCESS;
        }

        DEBUG("Encrypt Final()\n");

        /* The SW implem here copy the TAG to ctx->buf so that it can be
         * retrieved using ctrl() with GET_TAG.
         * Now the TAG is appended and has already been copied to that location
         * hence we do nothing.
         */

        /* Don't reuse the IV */
        qctx->iv_set = 0;
        return RET_SUCCESS;

    }
}
#endif
