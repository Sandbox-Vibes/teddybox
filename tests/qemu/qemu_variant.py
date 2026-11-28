import re
p='main/main.c'; t=open(p).read(); t=t.replace('void app_main(void)','void app_main_real(void)',1); open(p,'w').write(t)
p='main/wifi.c'; t=open(p).read(); t=re.sub(r'\bbool wifi_is_connected\(', 'bool wifi_is_connected_real(', t, count=1); open(p,'w').write(t)
p='main/cloud.c'; t=open(p).read()
t+='''
static esp_err_t qtest_data(void *ctx, uint8_t *data, size_t len) { *(uint32_t *)ctx += len; return ESP_OK; }
static esp_err_t qtest_closed(void *ctx) { return ESP_OK; }
esp_err_t cloud_test_get(const char *path, const char *auth, uint32_t *bytes)
{
    cloud_req_t req = {.host = CLOUD_HOST, .port = CLOUD_PORT, .path = path, .auth = auth,
                       .data_received_cbr = &qtest_data, .data_received_ctx = bytes,
                       .connection_closed_cbr = &qtest_closed, .connection_closed_ctx = bytes};
    return cloud_request(&req);
}
'''
open(p,'w').write(t)
p='main/CMakeLists.txt'; t=open(p).read()
t=t.replace('"tls_mfl.c"','"tls_mfl.c" "qemu_mem_test.c"')
# test sounds and the teddyCloud CA (setup.html is embedded gzip compressed further down)
assert 'set(embed_files)' in t and '"tls_mfl.c"' in t
t=t.replace('set(embed_files)','set(embed_files "jingle.ogg" "music.ogg" "speech.ogg" "tc_ca.der")',1)
open(p,'w').write(t)
p='CMakeLists.txt'; t=open(p).read()
t=t.replace('project(teddybox)','idf_build_set_property(COMPILE_DEFINITIONS "-DDR_REG_EMAC_BASE=0x600CD000" APPEND)\nidf_build_set_property(COMPILE_DEFINITIONS "-DETS_ETH_MAC_INTR_SOURCE=0" APPEND)\nidf_build_set_property(COMPILE_DEFINITIONS "-DTEDDYBOX_QEMU_TEST" APPEND)\nproject(teddybox)')
open(p,'w').write(t)
open('sdkconfig.defaults','a').write('\nCONFIG_ETH_USE_OPENETH=y\nCONFIG_ETH_OPENETH_DMA_RX_BUFFER_NUM=4\nCONFIG_ETH_OPENETH_DMA_TX_BUFFER_NUM=1\n')
