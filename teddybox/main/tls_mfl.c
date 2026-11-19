/*
 * Ask TLS servers for records of at most 4 KB (RFC 6066 max_fragment_length).
 * teddyCloud honours it; with CONFIG_MBEDTLS_DYNAMIC_BUFFER the receive
 * buffer then only takes 4 KB instead of 16 KB per record (measured in QEMU:
 * a running download needs 25 KB instead of 31 KB). A server that ignores the
 * extension still works, records of up to 16 KB are accepted
 * (CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384; with 8192 teddyCloud's content
 * downloads failed after the HTTP header).
 *
 * esp-tls has no option for it. Every connection of cloud.c passes a CA, so
 * esp-tls calls mbedtls_ssl_conf_ca_chain() while building the client
 * config, before mbedtls_ssl_setup(); that call is wrapped
 * (-Wl,--wrap=mbedtls_ssl_conf_ca_chain in main/CMakeLists.txt).
 * mbedtls_ssl_setup() itself can not be used, IDF already wraps it for the
 * dynamic buffers.
 */
#include "mbedtls/ssl.h"

void __real_mbedtls_ssl_conf_ca_chain(mbedtls_ssl_config *conf, mbedtls_x509_crt *ca_chain, mbedtls_x509_crl *ca_crl);

void __wrap_mbedtls_ssl_conf_ca_chain(mbedtls_ssl_config *conf, mbedtls_x509_crt *ca_chain, mbedtls_x509_crl *ca_crl)
{
    __real_mbedtls_ssl_conf_ca_chain(conf, ca_chain, ca_crl);
    if (conf->endpoint == MBEDTLS_SSL_IS_CLIENT)
    {
        /* needs CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN >= 4096; 2048 would be
           smaller but teddyCloud's certificate message (2.6 KB) would then have
           to be split over two records, which mbedTLS 2.x does not support */
        mbedtls_ssl_conf_max_frag_len(conf, MBEDTLS_SSL_MAX_FRAG_LEN_4096);
    }
}
