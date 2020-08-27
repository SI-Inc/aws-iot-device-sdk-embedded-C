/*
 * Copyright (C) 2020 Amazon.com, Inc. or its affiliates.  All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/* Standard includes. */
#include <assert.h>
#include <string.h>

/* POSIX socket include. */
#include <unistd.h>

/* Transport interface include. */
#include "transport_interface.h"

#include "openssl_posix.h"
#include <openssl/err.h>

/*-----------------------------------------------------------*/

/**
 * @brief Label of root CA when calling @ref logPath.
 */
#define ROOT_CA_LABEL        "Root CA certificate"

/**
 * @brief Label of client certificate when calling @ref logPath.
 */
#define CLIENT_CERT_LABEL    "client's certificate"

/**
 * @brief Label of client key when calling @ref logPath.
 */
#define CLIENT_KEY_LABEL     "client's key"

/*-----------------------------------------------------------*/

/**
 * @brief Log the absolute path given a relative or absolute path.
 *
 * @param[in] path Relative or absolute path.
 * @param[in] fileType NULL-terminated string describing the file type to log.
 */
#if ( LIBRARY_LOG_LEVEL == LOG_DEBUG )
    static void logPath( const char * path,
                         const char * fileType );
#endif /* #if ( LIBRARY_LOG_LEVEL == LOG_DEBUG ) */

/**
 * @brief Add X509 certificate to the trusted list of root certificates.
 *
 * OpenSSL does not provide a single function for reading and loading certificates
 * from files into stores, so the file API must be called. Start with the
 * root certificate.
 *
 * @param[out] pSslContext SSL context to which the trusted server root CA is to be added.
 * @param[in] pRootCaPath Filepath string to the trusted server root CA.
 *
 * @return 1 on success; -1, 0 on failure;
 */
static int32_t setRootCa( const SSL_CTX * pSslContext,
                          const char * pRootCaPath );

/**
 * @brief Set X509 certificate as client certificate for the server to authenticate.
 *
 * @param[out] pSslContext SSL context to which the client certificate is to be set.
 * @param[in] pClientCertPath Filepath string to the client certificate.
 *
 * @return 1 on success; 0 failure;
 */
static int32_t setClientCertificate( SSL_CTX * pSslContext,
                                     const char * pClientCertPath );

/**
 * @brief Set private key for the client's certificate.
 *
 * @param[out] pSslContext SSL context to which the private key is to be added.
 * @param[in] pPrivateKeyPath Filepath string to the client private key.
 *
 * @return 1 on success; 0 on failure;
 */
static int32_t setPrivateKey( SSL_CTX * pSslContext,
                              const char * pPrivateKeyPath );

/**
 * @brief Passes TLS credentials to the OpenSSL library.
 *
 * Provides the root CA certificate, client certificate, and private key to the
 * OpenSSL library. If the client certificate or private key is not NULL, mutual
 * authentication is used when performing the TLS handshake.
 *
 * @param[out] pSslContext SSL context to which the credentials are to be imported.
 * @param[in] pOpensslCredentials TLS credentials to be imported.
 *
 * @return 1 on success; -1, 0 on failure;
 */
static int32_t setCredentials( SSL_CTX * pSslContext,
                               const OpensslCredentials_t * pOpensslCredentials );

/**
 * @brief Set optional configurations for the TLS connection.
 *
 * This function is used to set SNI, MFLN, and ALPN protocols.
 *
 * @param[in] pSsl SSL context to which the optional configurations are to be set.
 * @param[in] pOpensslCredentials TLS credentials containing configurations.
 */
static void setOptionalConfigurations( SSL * pSsl,
                                       const OpensslCredentials_t * pOpensslCredentials );

/**
 * @brief Converts the sockets wrapper status to openssl status.
 *
 * @param[in] socketStatus Sockets wrapper status.
 *
 * @return #OPENSSL_SUCCESS, #OPENSSL_INVALID_PARAMETER, #OPENSSL_DNS_FAILURE,
 * and #OPENSSL_CONNECT_FAILURE.
 */
static OpensslStatus_t convertToOpensslStatus( SocketStatus_t socketStatus );

/*-----------------------------------------------------------*/

#if ( LIBRARY_LOG_LEVEL == LOG_DEBUG )
    static void logPath( const char * path,
                         const char * fileType )
    {
        char * cwd = NULL;

        assert( path != NULL );
        assert( fileType != NULL );

        /* Unused parameter when logs are disabled. */
        ( void ) fileType;

        /* Log the absolute directory based on first character of path. */
        if( ( path[ 0 ] == '/' ) || ( path[ 0 ] == '\\' ) )
        {
            LogDebug( ( "Attempting to open %s: Path=%s.",
                        fileType,
                        path ) );
        }
        else
        {
            cwd = getcwd( NULL, 0 );
            LogDebug( ( "Attempting to open %s: Path=%s/%s.",
                        fileType,
                        cwd,
                        path ) );
        }

        /* Free cwd because getcwd calls malloc. */
        if( cwd != NULL )
        {
            free( cwd );
        }
    }
#endif /* #if ( LIBRARY_LOG_LEVEL == LOG_DEBUG ) */
/*-----------------------------------------------------------*/

static OpensslStatus_t convertToOpensslStatus( SocketStatus_t socketStatus )
{
    OpensslStatus_t opensslStatus = OPENSSL_INVALID_PARAMETER;

    switch( socketStatus )
    {
        case SOCKETS_SUCCESS:
            opensslStatus = OPENSSL_SUCCESS;
            break;

        case SOCKETS_INVALID_PARAMETER:
            opensslStatus = OPENSSL_INVALID_PARAMETER;
            break;

        case SOCKETS_DNS_FAILURE:
            opensslStatus = OPENSSL_DNS_FAILURE;
            break;

        case SOCKETS_CONNECT_FAILURE:
            opensslStatus = OPENSSL_CONNECT_FAILURE;
            break;

        default:
            LogError( ( "Unexpected status received from socket wrapper: Socket status = %u",
                        socketStatus ) );
            break;
    }

    return opensslStatus;
}
/*-----------------------------------------------------------*/

static int32_t setRootCa( const SSL_CTX * pSslContext,
                          const char * pRootCaPath )
{
    int32_t sslStatus = 1;
    FILE * pRootCaFile = NULL;
    X509 * pRootCa = NULL;

    assert( pSslContext != NULL );
    assert( pRootCaPath != NULL );

    #if ( LIBRARY_LOG_LEVEL == LOG_DEBUG )
        logPath( pRootCaPath, ROOT_CA_LABEL );
    #endif

    /* MISRA Rule 21.6 flags the following line for using the standard
     * library input/output function `fopen()`. This rule is suppressed because
     * openssl function #PEM_read_X509 takes an argument of type `FILE *` for
     * reading the root ca PEM file and `fopen()` needs to be used to get the
     * file pointer.  */
    /* coverity[misra_c_2012_rule_21_6_violation] */
    pRootCaFile = fopen( pRootCaPath, "r" );

    if( pRootCaFile == NULL )
    {
        LogError( ( "fopen failed to find the root CA certificate file: "
                    "ROOT_CA_PATH=%s.",
                    pRootCaPath ) );
        sslStatus = -1;
    }

    if( sslStatus == 1 )
    {
        /* Read the root CA into an X509 object. */
        pRootCa = PEM_read_X509( pRootCaFile, NULL, NULL, NULL );

        if( pRootCa == NULL )
        {
            LogError( ( "PEM_read_X509 failed to parse root CA." ) );
            sslStatus = -1;
        }
    }

    if( sslStatus == 1 )
    {
        /* Add the certificate to the context. */
        sslStatus = X509_STORE_add_cert( SSL_CTX_get_cert_store( pSslContext ),
                                         pRootCa );

        if( sslStatus != 1 )
        {
            LogError( ( "X509_STORE_add_cert failed to add root CA to certificate store." ) );
            sslStatus = -1;
        }
    }

    /* Close the file if it was successfully opened. */
    if( pRootCaFile != NULL )
    {
        /* MISRA Rule 21.6 flags the following line for using the standard
         * library input/output function `fclose()`. This rule is suppressed
         * because openssl function #PEM_read_X509 takes an argument of type
         * `FILE *` for reading the root ca PEM file and `fopen()` is used to
         * get the file pointer. The file opened with `fopen()` needs to be
         * closed by calling `fclose()`.*/
        /* coverity[misra_c_2012_rule_21_6_violation] */
        if( fclose( pRootCaFile ) != 0 )
        {
            LogWarn( ( "fclose failed to close file %s",
                       pRootCaPath ) );
        }
    }

    /* Log the success message if we successfully imported the root CA. */
    if( sslStatus == 1 )
    {
        LogDebug( ( "Successfully imported root CA." ) );
    }

    return sslStatus;
}
/*-----------------------------------------------------------*/

static int32_t setClientCertificate( SSL_CTX * pSslContext,
                                     const char * pClientCertPath )
{
    int32_t sslStatus = -1;

    assert( pSslContext != NULL );
    assert( pClientCertPath != NULL );

    #if ( LIBRARY_LOG_LEVEL == LOG_DEBUG )
        logPath( pClientCertPath, CLIENT_CERT_LABEL );
    #endif

    /* Import the client certificate. */
    sslStatus = SSL_CTX_use_certificate_chain_file( pSslContext,
                                                    pClientCertPath );

    if( sslStatus != 1 )
    {
        LogError( ( "SSL_CTX_use_certificate_chain_file failed to import "
                    "client certificate at %s.",
                    pClientCertPath ) );
    }
    else
    {
        LogDebug( ( "Successfully imported client certificate." ) );
    }

    return sslStatus;
}
/*-----------------------------------------------------------*/

static int32_t setPrivateKey( SSL_CTX * pSslContext,
                              const char * pPrivateKeyPath )
{
    int32_t sslStatus = -1;

    assert( pSslContext != NULL );
    assert( pPrivateKeyPath != NULL );

    #if ( LIBRARY_LOG_LEVEL == LOG_DEBUG )
        logPath( pPrivateKeyPath, CLIENT_KEY_LABEL );
    #endif

    /* Import the client certificate private key. */
    sslStatus = SSL_CTX_use_PrivateKey_file( pSslContext,
                                             pPrivateKeyPath,
                                             SSL_FILETYPE_PEM );

    if( sslStatus != 1 )
    {
        LogError( ( "SSL_CTX_use_PrivateKey_file failed to import client "
                    "certificate private key at %s.",
                    pPrivateKeyPath ) );
    }
    else
    {
        LogDebug( ( "Successfully imported client certificate private key." ) );
    }

    return sslStatus;
}
/*-----------------------------------------------------------*/

static int32_t setCredentials( SSL_CTX * pSslContext,
                               const OpensslCredentials_t * pOpensslCredentials )
{
    int32_t sslStatus = 0;

    assert( pSslContext != NULL );
    assert( pOpensslCredentials != NULL );

    if( pOpensslCredentials->pRootCaPath != NULL )
    {
        sslStatus = setRootCa( pSslContext,
                               pOpensslCredentials->pRootCaPath );
    }

    if( ( sslStatus == 1 ) &&
        ( pOpensslCredentials->pClientCertPath != NULL ) )
    {
        sslStatus = setClientCertificate( pSslContext,
                                          pOpensslCredentials->pClientCertPath );
    }

    if( ( sslStatus == 1 ) &&
        ( pOpensslCredentials->pPrivateKeyPath != NULL ) )
    {
        sslStatus = setPrivateKey( pSslContext,
                                   pOpensslCredentials->pPrivateKeyPath );
    }

    return sslStatus;
}
/*-----------------------------------------------------------*/

static void setOptionalConfigurations( SSL * pSsl,
                                       const OpensslCredentials_t * pOpensslCredentials )
{
    int32_t sslStatus = -1;
    int16_t readBufferLength = 0;

    assert( pSsl != NULL );
    assert( pOpensslCredentials != NULL );

    /* Set TLS ALPN if requested. */
    if( ( pOpensslCredentials->pAlpnProtos != NULL ) &&
        ( pOpensslCredentials->alpnProtosLen > 0U ) )
    {
        LogDebug( ( "Setting ALPN protos." ) );
        sslStatus = SSL_set_alpn_protos( pSsl,
                                         ( const uint8_t * ) pOpensslCredentials->pAlpnProtos,
                                         ( uint32_t ) pOpensslCredentials->alpnProtosLen );

        if( sslStatus != 0 )
        {
            LogError( ( "SSL_set_alpn_protos failed to set ALPN protos. %s",
                        pOpensslCredentials->pAlpnProtos ) );
        }
    }

    /* Set TLS MFLN if requested. */
    if( pOpensslCredentials->maxFragmentLength > 0U )
    {
        LogDebug( ( "Setting max send fragment length %u.",
                    pOpensslCredentials->maxFragmentLength ) );

        /* Set the maximum send fragment length. */

        /* MISRA Directive 4.6 flags the following line for using basic
         * numerical type long. This directive is suppressed because openssl
         * function #SSL_set_max_send_fragment expects a length argument
         * type of long. */
        /* coverity[misra_c_2012_directive_4_6_violation] */
        sslStatus = ( int32_t ) SSL_set_max_send_fragment( pSsl,
                                                           ( long ) pOpensslCredentials->maxFragmentLength );

        if( sslStatus != 1 )
        {
            LogError( ( "Failed to set max send fragment length %u.",
                        pOpensslCredentials->maxFragmentLength ) );
        }
        else
        {
            readBufferLength = ( int16_t ) pOpensslCredentials->maxFragmentLength +
                               SSL3_RT_MAX_ENCRYPTED_OVERHEAD;

            /* Change the size of the read buffer to match the
             * maximum fragment length + some extra bytes for overhead. */
            SSL_set_default_read_buffer_len( pSsl,
                                             ( size_t ) readBufferLength );
        }
    }

    /* Enable SNI if requested. */
    if( pOpensslCredentials->sniHostName != NULL )
    {
        LogDebug( ( "Setting server name %s for SNI.",
                    pOpensslCredentials->sniHostName ) );

        /* MISRA Rule 11.8 flags the following line for removing the const
         * qualifier from the pointed to type. This rule is suppressed because
         * openssl implementation of #SSL_set_tlsext_host_name internally casts
         * the pointer to a string literal to a `void *` pointer. */
        /* coverity[misra_c_2012_rule_11_8_violation] */
        sslStatus = ( int32_t ) SSL_set_tlsext_host_name( pSsl,
                                                          pOpensslCredentials->sniHostName );

        if( sslStatus != 1 )
        {
            LogError( ( "Failed to set server name %s for SNI.",
                        pOpensslCredentials->sniHostName ) );
        }
    }
}
/*-----------------------------------------------------------*/

OpensslStatus_t Openssl_Connect( NetworkContext_t * pNetworkContext,
                                 const ServerInfo_t * pServerInfo,
                                 const OpensslCredentials_t * pOpensslCredentials,
                                 uint32_t sendTimeoutMs,
                                 uint32_t recvTimeoutMs )
{
    SocketStatus_t socketStatus = SOCKETS_SUCCESS;
    OpensslStatus_t returnStatus = OPENSSL_SUCCESS;
    int32_t sslStatus = 0;
    uint8_t sslObjectCreated = 0;
    SSL_CTX * pSslContext = NULL;
    int32_t verifyPeerCertStatus = X509_V_OK;

    /* Validate parameters. */
    if( pNetworkContext == NULL )
    {
        LogError( ( "Parameter check failed: pNetworkContext is NULL." ) );
        returnStatus = OPENSSL_INVALID_PARAMETER;
    }
    else if( pOpensslCredentials == NULL )
    {
        LogError( ( "Parameter check failed: pOpensslCredentials is NULL." ) );
        returnStatus = OPENSSL_INVALID_PARAMETER;
    }
    else
    {
        /* Empty else. */
    }

    /* Establish the TCP connection. */
    if( returnStatus == OPENSSL_SUCCESS )
    {
        socketStatus = Sockets_Connect( &pNetworkContext->socketDescriptor,
                                        pServerInfo,
                                        sendTimeoutMs,
                                        recvTimeoutMs );

        /* Convert socket wrapper status to openssl status. */
        returnStatus = convertToOpensslStatus( socketStatus );
    }

    /* Create SSL context. */
    if( returnStatus == OPENSSL_SUCCESS )
    {
        pSslContext = SSL_CTX_new( TLS_client_method() );

        if( pSslContext == NULL )
        {
            LogError( ( "Creation of a new SSL_CTX object failed." ) );
            returnStatus = OPENSSL_API_ERROR;
        }
    }

    /* Setup credentials. */
    if( returnStatus == OPENSSL_SUCCESS )
    {
        /* Set auto retry mode for the blocking calls to SSL_read and SSL_write.
         * The mask returned by SSL_CTX_set_mode does not need to be checked. */
        /* MISRA Directive 4.6 flags the following line for using basic
        * numerical type long. This directive is suppressed because openssl
        * function #SSL_CTX_set_mode takes an argument of type long. */
        /* coverity[misra_c_2012_directive_4_6_violation] */
        ( void ) SSL_CTX_set_mode( pSslContext, ( long ) SSL_MODE_AUTO_RETRY );

        sslStatus = setCredentials( pSslContext,
                                    pOpensslCredentials );

        if( sslStatus != 1 )
        {
            LogError( ( "Setting up credentials failed." ) );
            returnStatus = OPENSSL_INVALID_CREDENTIALS;
        }
    }

    /* Create a new SSL session. */
    if( returnStatus == OPENSSL_SUCCESS )
    {
        pNetworkContext->pSsl = SSL_new( pSslContext );

        if( pNetworkContext->pSsl == NULL )
        {
            LogError( ( "SSL_new failed to create a new SSL context." ) );
            returnStatus = OPENSSL_API_ERROR;
        }
        else
        {
            sslObjectCreated = 1u;
        }
    }

    /* Setup the socket to use for communication. */
    if( returnStatus == OPENSSL_SUCCESS )
    {
        /* Enable SSL peer verification. */
        SSL_set_verify( pNetworkContext->pSsl, SSL_VERIFY_PEER, NULL );

        sslStatus = SSL_set_fd( pNetworkContext->pSsl, pNetworkContext->socketDescriptor );

        if( sslStatus != 1 )
        {
            LogError( ( "SSL_set_fd failed to set the socket fd to SSL context." ) );
            returnStatus = OPENSSL_API_ERROR;
        }
    }

    /* Perform the TLS handshake. */
    if( returnStatus == OPENSSL_SUCCESS )
    {
        setOptionalConfigurations( pNetworkContext->pSsl, pOpensslCredentials );

        sslStatus = SSL_connect( pNetworkContext->pSsl );

        if( sslStatus != 1 )
        {
            LogError( ( "SSL_connect failed to perform TLS handshake." ) );
            returnStatus = OPENSSL_HANDSHAKE_FAILED;
        }
    }

    /* Verify X509 certificate from peer. */
    if( returnStatus == OPENSSL_SUCCESS )
    {
        verifyPeerCertStatus = ( int32_t ) SSL_get_verify_result( pNetworkContext->pSsl );

        if( verifyPeerCertStatus != X509_V_OK )
        {
            LogError( ( "SSL_get_verify_result failed to verify X509 "
                        "certificate from peer." ) );
            returnStatus = OPENSSL_HANDSHAKE_FAILED;
        }
    }

    /* Free the SSL context. */
    if( pSslContext != NULL )
    {
        SSL_CTX_free( pSslContext );
    }

    /* Clean up on error. */
    if( ( returnStatus != OPENSSL_SUCCESS ) && ( sslObjectCreated == 1u ) )
    {
        SSL_free( pNetworkContext->pSsl );
        pNetworkContext->pSsl = NULL;
    }

    /* Log failure or success depending on status. */
    if( returnStatus != OPENSSL_SUCCESS )
    {
        LogError( ( "Failed to establish a TLS connection." ) );
    }
    else
    {
        LogDebug( ( "Established a TLS connection." ) );
    }

    return returnStatus;
}
/*-----------------------------------------------------------*/

OpensslStatus_t Openssl_Disconnect( const NetworkContext_t * pNetworkContext )
{
    SocketStatus_t socketStatus = SOCKETS_INVALID_PARAMETER;

    if( pNetworkContext == NULL )
    {
        /* No need to update the status here. The socket status
         * SOCKETS_INVALID_PARAMETER will be converted to openssl
         * status OPENSSL_INVALID_PARAMETER before returning from this
         * function. */
        LogError( ( "Parameter check failed: pNetworkContext is NULL." ) );
    }
    else
    {
        if( pNetworkContext->pSsl != NULL )
        {
            /* SSL shutdown should be called twice: once to send "close notify" and
             * once more to receive the peer's "close notify". */
            if( SSL_shutdown( pNetworkContext->pSsl ) == 0 )
            {
                ( void ) SSL_shutdown( pNetworkContext->pSsl );
            }

            SSL_free( pNetworkContext->pSsl );
        }

        /* Tear down the socket connection, pNetworkContext != NULL here. */
        socketStatus = Sockets_Disconnect( pNetworkContext->socketDescriptor );
    }

    return convertToOpensslStatus( socketStatus );
}
/*-----------------------------------------------------------*/

int32_t Openssl_Recv( const NetworkContext_t * pNetworkContext,
                      void * pBuffer,
                      size_t bytesToRecv )
{
    int32_t bytesReceived = 0;
    int32_t sslError = 0;

    if( pNetworkContext == NULL )
    {
        LogError( ( "Parameter check failed: pNetworkContext is NULL." ) );
    }
    else if( pNetworkContext->pSsl != NULL )
    {
        /* SSL read of data. */
        bytesReceived = ( int32_t ) SSL_read( pNetworkContext->pSsl,
                                              pBuffer,
                                              ( int32_t ) bytesToRecv );

        /* Handle error return status if transport read did not succeed. */
        if( bytesReceived <= 0 )
        {
            sslError = SSL_get_error( pNetworkContext->pSsl, bytesReceived );

            if( sslError == SSL_ERROR_WANT_READ )
            {
                /* There is no data to receive at this time. */
                bytesReceived = 0;
            }
            else
            {
                LogError( ( "Failed to receive data over network: SSL_read failed: "
                            "ErrorStatus=%s.", ERR_reason_error_string( sslError ) ) );
            }
        }
    }
    else
    {
        LogError( ( "Failed to receive data over network: "
                    "SSL object in network context is NULL." ) );
    }

    return bytesReceived;
}
/*-----------------------------------------------------------*/

int32_t Openssl_Send( const NetworkContext_t * pNetworkContext,
                      const void * pBuffer,
                      size_t bytesToSend )
{
    int32_t bytesSent = 0;
    int32_t sslError = 0;

    /* Unused parameter when logs are disabled. */
    ( void ) sslError;

    if( pNetworkContext == NULL )
    {
        LogError( ( "Parameter check failed: pNetworkContext is NULL." ) );
    }
    else if( pNetworkContext->pSsl != NULL )
    {
        /* SSL write of data. */
        bytesSent = ( int32_t ) SSL_write( pNetworkContext->pSsl,
                                           pBuffer,
                                           ( int32_t ) bytesToSend );

        if( bytesSent <= 0 )
        {
            sslError = SSL_get_error( pNetworkContext->pSsl, bytesSent );

            LogError( ( "Failed to send data over network: SSL_write of OpenSSL failed: "
                        "ErrorStatus=%s.", ERR_reason_error_string( sslError ) ) );
        }
    }
    else
    {
        LogError( ( "Failed to send data over network: "
                    "SSL object in network context is NULL." ) );
    }

    return bytesSent;
}
/*-----------------------------------------------------------*/
