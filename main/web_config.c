/*
 * web_config.c - Web Configuration UI Module Implementation
 * ESP32 Printer Client System
 */

#include "web_config.h"
#include "config.h"
#include "printer.h"
#include "wifi_manager.h"
#include "wifi_http.h"
#include "wifi_portal.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "http_client.h"
#include "cJSON.h"

#include <string.h>

static const char *TAG = "WEB_CFG";
static web_config_mode_t s_mode;
static QueueHandle_t s_order_queue = NULL;
static bool s_printer_checked = false;
static bool s_printer_reachable = false;

/* ── HTML Content ───────────────────────────────────────────────────── */
static const char *html_page = 
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>PrinterBox</title>"
"<style>"
":root { --p: #007bff; --s: #28a745; --d: #dc3545; --bg: #f4f7f6; --c: #fff; --t: #333; }"
"body { font-family: -apple-system, sans-serif; background: var(--bg); color: var(--t); margin: 0; padding: 15px; }"
".card { background: var(--c); border-radius: 12px; padding: 20px; max-width: 500px; margin: 0 auto 15px; box-shadow: 0 4px 12px rgba(0,0,0,0.05); }"
"h2, h3 { text-align: center; color: var(--t); margin-top: 0; }"
".lang-switch { text-align: right; margin-bottom: 10px; }"
".lang-switch span { cursor: pointer; color: var(--p); font-weight: bold; padding: 5px; }"
".status-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin-bottom: 15px; }"
".st-box { background: var(--bg); padding: 12px; border-radius: 8px; text-align: center; font-size: 14px; }"
".st-box div { font-size: 12px; color: #666; margin-bottom: 5px; }"
".st-val { font-weight: bold; font-size: 16px; }"
".c-green { color: var(--s); } .c-red { color: var(--d); } .c-blue { color: var(--p); }"
".step { border-left: 4px solid var(--p); padding-left: 15px; margin-bottom: 25px; }"
".step h4 { margin: 0 0 10px 0; color: var(--p); }"
".group { margin-bottom: 15px; }"
"label { display: block; margin-bottom: 5px; font-weight: bold; font-size: 14px; color: #555; }"
"input { width: 100%; padding: 12px; border: 1px solid #ddd; border-radius: 6px; box-sizing: border-box; font-size: 16px; background: #fafafa; }"
"input[type=checkbox] { width: auto; transform: scale(1.3); margin-right: 10px; }"
".btn { width: 100%; padding: 14px; background: var(--p); color: white; border: none; border-radius: 6px; font-size: 16px; font-weight: bold; cursor: pointer; margin-top: 10px; }"
".btn:active { opacity: 0.8; }"
".btn-green { background: var(--s); }"
".btn-red { background: var(--d); }"
".btn-gray { background: #6c757d; }"
".adv-toggle { text-align: center; color: #888; font-size: 14px; margin: 15px 0; cursor: pointer; text-decoration: underline; }"
".hidden { display: none !important; }"
".test-row { display: flex; gap: 10px; margin-top: 15px; }"
".test-row .btn { margin-top: 0; font-size: 14px; padding: 10px; }"
".info-row { display: flex; justify-content: space-between; font-size: 13px; color: #666; margin-bottom: 5px; border-bottom: 1px dashed #eee; padding-bottom: 5px; }"
".ssid-row { display:flex; gap:8px; align-items:stretch; }"
".ssid-row select { flex:1; min-width:0; padding:12px; border:1px solid #ddd; border-radius:6px; font-size:16px; background:#fafafa; }"
".scan-btn { flex:0 0 92px; padding:0 10px; border:1px solid #ccc; border-radius:6px; background:#fff; color:#333; font-size:15px; font-weight:700; cursor:pointer; white-space:nowrap; }"
".scan-btn:disabled { opacity:.75; cursor:wait; background:#eee; }"
"</style></head><body>"
"<div class='card'>"
"  <div class='lang-switch'><span onclick='setLang(\"zh\")'>中文</span> | <span onclick='setLang(\"en\")'>EN</span></div>"
"  <h2 id='t_title'>PrinterBox 打印盒设置</h2>"
"  <div class='status-grid'>"
"    <div class='st-box'><div id='t_st_wifi'>WiFi状态</div><div class='st-val' id='v_wifi'>-</div></div>"
"    <div class='st-box'><div id='t_st_cloud'>云端状态</div><div class='st-val' id='v_cloud'>-</div></div>"
"    <div class='st-box'><div id='t_st_printer'>打印机连通</div><div class='st-val' id='v_printer_st'>-</div></div>"
"    <div class='st-box'><div id='t_st_ip'>局域网 IP</div><div class='st-val c-blue' id='v_ip'>-</div></div>"
"  </div>"
"  <div class='test-row'>"
"    <button class='btn btn-gray' id='t_btn_test_net' onclick='testNet()'>测网络</button>"
"    <button class='btn btn-gray' id='t_btn_test_print' onclick='testPrint()'>测打印</button>"
"  </div>"
"</div>"
"<div class='card'>"
"  <h3 id='t_info'>设备信息</h3>"
"  <div class='info-row'><span>MAC:</span><strong id='v_mac'>-</strong></div>"
"  <div class='info-row'><span id='t_ver'>固件版本:</span><strong id='v_ver'>-</strong></div>"
"  <div class='info-row'><span id='t_uptime'>运行时长:</span><strong id='v_up'>-</strong></div>"
"</div>"
"<div class='card' id='setup-form'>"
"  <h3 id='t_cfg_title'>配置向导</h3>"
"  <div class='step'>"
"    <h4 id='t_step1'>第一步：连接店内 WiFi</h4>"
"    <div class='group'><label id='t_ssid'>WiFi 名称</label>"
"      <div class='ssid-row'>"
"        <select id='wifi_ssid'></select>"
"        <button type='button' onclick='scanWifi()' class='scan-btn' id='t_btn_scan'>扫描</button>"
"      </div>"
"      <input type='text' id='wifi_ssid_manual' placeholder='或手动输入SSID' style='margin-top:8px'>"
"    </div>"
"    <div class='group'><label id='t_pass'>WiFi 密码</label><input type='text' id='wifi_pass' placeholder='********'></div>"
"    <div id='wifi_st_box' style='background:#f0f7ff;padding:10px;border-radius:6px;margin-top:8px;font-size:13px'></div>"
"  </div>"
"  <div class='step'>"
"    <h4 id='t_step2'>第二步：连接打印机</h4>"
"    <div class='group'><label id='t_pip'>打印机 IP</label><input type='text' id='printer_ip'></div>"
"    <div class='group'><label id='t_pport'>打印机端口 (默认9100)</label><input type='number' id='printer_port'></div>"
"  </div>"
"  <div class='step'>"
"    <h4 id='t_step3'>第三步：连接后台 (WiFi 拉单)</h4>"
"    <div class='group'><label id='t_surl'>后台地址</label><input type='text' id='server_url' placeholder='http://192.168.1.50:3000'></div>"
"    <div class='group'><label id='t_tok'>API Token</label><input type='text' id='api_token' placeholder='后台新增 WiFi 打印机时生成'></div>"
"  </div>"
"  <div class='adv-toggle' id='t_adv_toggle' onclick='toggleAdv()'>展开高级设置 (固定IP)</div>"
"  <div id='adv-box' class='hidden'>"
"    <div class='group'><label><input type='checkbox' id='use_static_ip' onchange='toggleStatic()'><span id='t_use_static'>使用固定 IP</span></label></div>"
"    <div id='static-fields' class='hidden'>"
"      <div class='group'><label>Static IP</label><input type='text' id='static_ip'></div>"
"      <div class='group'><label>Gateway</label><input type='text' id='gateway'></div>"
"      <div class='group'><label>Netmask</label><input type='text' id='netmask'></div>"
"      <div class='group'><label>DNS</label><input type='text' id='dns'></div>"
"    </div>"
"  </div>"
"  <button class='btn btn-green' id='t_btn_save' onclick='saveConfig()'>保存并重启</button>"
"  <button class='btn btn-red' id='t_btn_reset' onclick='clearConfig()'>恢复出厂设置</button>"
"</div>"
"<script>"
"const dict = {"
"  zh: { title:'PrinterBox 打印盒设置', st_wifi:'WiFi状态', st_cloud:'后台状态', st_printer:'打印机连通', st_ip:'局域网IP', btn_test_net:'测网络', btn_test_print:'测打印', cfg_title:'配置向导', info:'设备信息', ver:'固件版本:', uptime:'运行时长:', step1:'第一步：连接店内 WiFi', ssid:'WiFi 名称', pass:'WiFi 密码 (为空则不修改)', step2:'第二步：连接打印机', pip:'打印机 IP', pport:'打印机端口', step3:'第三步：连接后台 (WiFi 拉单)', surl:'后台地址', tok:'API Token (为空则不修改)', adv_toggle:'展开高级设置 (固定IP)', use_static:'使用固定 IP', btn_save:'保存并重启', btn_reset:'恢复出厂设置', msg_reset:'确定要清空所有设置并恢复出厂吗？', msg_reset_ok:'已清空，设备正在重启...' },"
"  en: { title:'PrinterBox Settings', st_wifi:'WiFi Status', st_cloud:'Backend Status', st_printer:'Printer Link', st_ip:'LAN IP', btn_test_net:'Test Net', btn_test_print:'Test Print', cfg_title:'Setup Wizard', info:'Device Info', ver:'Firmware:', uptime:'Uptime:', step1:'Step 1: Connect WiFi', ssid:'WiFi Name', pass:'WiFi Password (leave blank to keep)', step2:'Step 2: Connect Printer', pip:'Printer IP', pport:'Printer Port', step3:'Step 3: Connect Backend (WiFi Pull)', surl:'Backend URL', tok:'API Token (leave blank to keep)', adv_toggle:'Advanced Settings (Static IP)', use_static:'Use Static IP', btn_save:'Save & Reboot', btn_reset:'Factory Reset', msg_reset:'Erase all settings and factory reset?', msg_reset_ok:'Erased! Rebooting...' }"
"};"
"let lang = 'zh';"
"function setLang(l) { lang = l; for(let k in dict[l]) { let el = document.getElementById('t_'+k); if(el) el.innerText = dict[l][k]; } document.getElementById('wifi_pass').placeholder = dict[l].pass; renderStatus(); }"
"let currentStatus = null;"
"function renderStatus() {"
"  if(!currentStatus) return;"
"  const d = currentStatus;"
"  const elWifi = document.getElementById('v_wifi');"
"  if(d.wifi_connected){ elWifi.innerText = lang==='zh'?'已连接':'Connected'; elWifi.className='st-val c-green'; } else { elWifi.innerText = lang==='zh'?'未连接':'Disconnected'; elWifi.className='st-val c-red'; }"
"  const elCloud = document.getElementById('v_cloud');"
"  if(d.ws_connected){ elCloud.innerText = lang==='zh'?'在线':'Online'; elCloud.className='st-val c-green'; } else { elCloud.innerText = lang==='zh'?'离线':'Offline'; elCloud.className='st-val c-red'; }"
"  const elPrt = document.getElementById('v_printer_st');"
"  if(!d.printer_checked){ elPrt.innerText = '-'; elPrt.className='st-val'; } else if(d.printer_reachable){ elPrt.innerText = lang==='zh'?'正常':'OK'; elPrt.className='st-val c-green'; } else { elPrt.innerText = lang==='zh'?'异常':'Failed'; elPrt.className='st-val c-red'; }"
"  document.getElementById('v_ip').innerText = d.ip || '-';"
"  document.getElementById('v_mac').innerText = d.mac || '-';"
"  document.getElementById('v_ver').innerText = d.fw || '-';"
"  document.getElementById('v_up').innerText = Math.floor(d.uptime/60) + ' min';"
"}"
"function loadData() {"
"  fetch('/api/status').then(r=>r.json()).then(d => {"
"    currentStatus = d; renderStatus();"
"    document.getElementById('wifi_ssid_manual').value = d.cfg.wifi_ssid || '';"
"    document.getElementById('server_url').value = d.cfg.server_url || '';"
"    document.getElementById('api_token').value = d.cfg.api_token || '';"
"    document.getElementById('printer_ip').value = d.cfg.printer_ip || '';"
"    document.getElementById('printer_port').value = d.cfg.printer_port || '';"
"    document.getElementById('use_static_ip').checked = d.cfg.use_static_ip;"
"    document.getElementById('static_ip').value = d.cfg.static_ip || '';"
"    document.getElementById('gateway').value = d.cfg.gateway || '';"
"    document.getElementById('netmask').value = d.cfg.netmask || '';"
"    document.getElementById('dns').value = d.cfg.dns || '';"
"    toggleStatic();"
"  });"
"}"
"function toggleAdv() { document.getElementById('adv-box').classList.toggle('hidden'); }"
"function toggleStatic() { document.getElementById('static-fields').classList.toggle('hidden', !document.getElementById('use_static_ip').checked); }"
"function saveConfig() {"
"  stopPolling();"
"  const p = {"
"    wifi_ssid: getSelectedSSID(),"
"    wifi_pass: document.getElementById('wifi_pass').value,"
"    server_url: document.getElementById('server_url').value,"
"    api_token: document.getElementById('api_token').value,"
"    printer_ip: document.getElementById('printer_ip').value,"
"    printer_port: parseInt(document.getElementById('printer_port').value),"
"    use_static_ip: document.getElementById('use_static_ip').checked,"
"    static_ip: document.getElementById('static_ip').value,"
"    gateway: document.getElementById('gateway').value,"
"    netmask: document.getElementById('netmask').value,"
"    dns: document.getElementById('dns').value"
"  };"
"  fetch('/api/save', { method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(p) }).then(async r=>{"
"    if(!r.ok){ const err = await r.json(); alert((lang==='zh'?'保存失败: ':'Error: ') + err.error); }"
"    else{ alert(lang==='zh'?'保存成功，正在重启...':'Saved! Rebooting...'); setTimeout(()=>location.reload(), 3000); }"
"  });"
"}"
"function testNet() { "
"  fetch('/api/test_server', {method:'POST'}).then(async r=>{"
"     if(!r.ok){ const err=await r.text(); alert((lang==='zh'?'服务器拨测失败: ':'Server Ping Failed: ')+err); }"
"     else alert(lang==='zh'?'拨测成功！服务器已连通。':'Server Ping OK!');"
"  });"
"}"
"function testPrint() { "
"  fetch('/api/test_print', { method:'POST' }).then(async r=>{"
"     if(!r.ok){ const err=await r.text(); alert((lang==='zh'?'打印机连接失败: ':'Printer Connect Failed: ')+err); }"
"     else alert(lang==='zh'?'打印测试已发送！':'Test print sent!');"
"  });"
"}"
"function clearConfig() { if(confirm(dict[lang].msg_reset)) { fetch('/api/clear', { method:'POST' }).then(()=> { alert(dict[lang].msg_reset_ok); setTimeout(()=>location.reload(), 3000); }); } }"
"let scanningWifi=false;"
"async function scanWifi() {"
"  if(scanningWifi) return;"
"  scanningWifi=true;"
"  const btn=document.getElementById('t_btn_scan'); btn.disabled=true; btn.innerText=lang==='zh'?'扫描中':'Scanning';"
"  try {"
"    const r=await fetch('/wifi_scan'); const d=await r.json();"
"    const sel=document.getElementById('wifi_ssid');"
"    sel.innerHTML='<option value=\"\">'+(lang==='zh'?'选择WiFi':'Select WiFi')+'</option>';"
"    d.forEach(ap=>{ const o=document.createElement('option'); o.value=ap.ssid; o.text=ap.ssid+' ('+ap.rssi+'dBm)'; sel.appendChild(o); });"
"  } catch(e){ console.error(e); alert(lang==='zh'?'扫描失败，请稍后重试':'Scan failed, please retry'); }"
"  scanningWifi=false;"
"  btn.disabled=false; btn.innerText=lang==='zh'?'扫描':'Scan';"
"}"
"function getSelectedSSID() {"
"  const m=document.getElementById('wifi_ssid_manual').value;"
"  return m||document.getElementById('wifi_ssid').value;"
"}"
"function updateWifiStatus() {"
"  fetch('/wifi_status').then(r=>r.json()).then(s=>{"
"    const box=document.getElementById('wifi_st_box');"
"    if(s.connected) box.innerHTML='<span style=\"color:green\">✓ '+(lang==='zh'?'已连接':'Connected')+': '+s.ssid+' ('+s.rssi+'dBm, '+s.quality+') IP: '+s.ip+'</span>';"
"    else box.innerHTML='<span style=\"color:orange\">'+(lang==='zh'?'未连接WiFi':'WiFi Disconnected')+'</span>';"
"  }).catch(()=>{});"
"}"
"let pollStatus=null,pollWifi=null;"
"function stopPolling(){ if(pollStatus) clearInterval(pollStatus); if(pollWifi) clearInterval(pollWifi); }"
"function refreshStatus(){ fetch('/api/status').then(r=>r.json()).then(d=>{currentStatus=d;renderStatus();}).catch(()=>{}); }"
"function initWifiSelect(){ document.getElementById('wifi_ssid').innerHTML='<option value=\"\">'+(lang==='zh'?'点击扫描获取 WiFi':'Tap Scan for WiFi')+'</option>'; }"
"window.onload = function() { setLang('zh'); initWifiSelect(); loadData(); updateWifiStatus(); pollStatus=setInterval(refreshStatus, 10000); pollWifi=setInterval(updateWifiStatus, 15000); };"
"</script></body></html>";

/* ── Helpers ───────────────────────────────────────────────────────── */

static bool test_server_reachable(const char *url) {
    if (!url || strlen(url) == 0) return false;
    char host[128] = {0};
    uint16_t port = 80;
    
    const char *p = strstr(url, "://");
    if (p) p += 3;
    else p = url;
    
    if (strncmp(url, "wss://", 6) == 0 || strncmp(url, "https://", 8) == 0) port = 443;
    
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');
    
    if (colon && (!slash || colon < slash)) {
        int len = colon - p;
        if (len >= sizeof(host)) len = sizeof(host) - 1;
        strncpy(host, p, len);
        port = atoi(colon + 1);
    } else {
        int len = slash ? (slash - p) : strlen(p);
        if (len >= sizeof(host)) len = sizeof(host) - 1;
        strncpy(host, p, len);
    }
    
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res;
    char port_str[16];
    sprintf(port_str, "%u", port);
    
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return false;
    
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return false; }
    
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    int err = connect(sock, res->ai_addr, res->ai_addrlen);
    if (sock >= 0) close(sock);
    freeaddrinfo(res);
    return (err == 0);
}

/* ── HTTP Handlers ──────────────────────────────────────────────────── */

/* GET / */
static esp_err_t get_index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* GET /api/status */
static esp_err_t get_status_handler(httpd_req_t *req)
{
    const device_config_t *cfg = config_get();
    cJSON *root = cJSON_CreateObject();
    
    cJSON_AddStringToObject(root, "mode", s_mode == WEB_CONFIG_MODE_AP ? "ap" : "sta");
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_mgr_is_connected());
    /* "ws_connected" key kept for UI compatibility; now means backend online. */
    cJSON_AddBoolToObject(root, "ws_connected", http_client_is_connected());
    cJSON_AddBoolToObject(root, "printer_checked", s_printer_checked);
    cJSON_AddBoolToObject(root, "printer_reachable", s_printer_reachable);

    /* Get IP */
    char ip_str[16] = "";
    if (wifi_mgr_is_connected()) {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t ip_info;
            if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
                esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            }
        }
    }
    cJSON_AddStringToObject(root, "ip", ip_str);

    /* Device Info */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_str[18];
    sprintf(mac_str, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddStringToObject(root, "fw", FIRMWARE_VERSION);
    cJSON_AddNumberToObject(root, "uptime", (double)(esp_timer_get_time() / 1000000ULL));

    cJSON *cfg_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg_obj, "wifi_ssid", cfg->wifi_ssid);
    /* Mask the password if it's set */
    cJSON_AddStringToObject(cfg_obj, "wifi_pass", strlen(cfg->wifi_pass) > 0 ? "********" : "");
    cJSON_AddStringToObject(cfg_obj, "server_url", cfg->server_url);
    /* Mask the token; UI submits "********" to keep the stored one unchanged. */
    cJSON_AddStringToObject(cfg_obj, "api_token", strlen(cfg->api_token) > 0 ? "********" : "");
    cJSON_AddStringToObject(cfg_obj, "printer_ip", cfg->printer_ip);
    cJSON_AddNumberToObject(cfg_obj, "printer_port", cfg->printer_port);
    cJSON_AddBoolToObject(cfg_obj, "use_static_ip", cfg->use_static_ip);
    cJSON_AddStringToObject(cfg_obj, "static_ip", cfg->static_ip);
    cJSON_AddStringToObject(cfg_obj, "gateway", cfg->gateway);
    cJSON_AddStringToObject(cfg_obj, "netmask", cfg->netmask);
    cJSON_AddStringToObject(cfg_obj, "dns", cfg->dns);

    cJSON_AddItemToObject(root, "cfg", cfg_obj);

    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);

    cJSON_Delete(root);
    free(json_str);
    return ESP_OK;
}

/* POST /api/save */
static esp_err_t post_save_handler(httpd_req_t *req)
{
    char buf[1024];
    int ret, remaining = req->content_len;

    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Payload too large");
        return ESP_FAIL;
    }

    int received = 0;
    while (remaining > 0) {
        ret = httpd_req_recv(req, buf + received, remaining);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            return ESP_FAIL;
        }
        received += ret;
        remaining -= ret;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    device_config_t new_cfg;
    memset(&new_cfg, 0, sizeof(new_cfg));

    cJSON *item;
    if ((item = cJSON_GetObjectItem(root, "wifi_ssid")) && cJSON_IsString(item)) 
        strncpy(new_cfg.wifi_ssid, item->valuestring, sizeof(new_cfg.wifi_ssid) - 1);
    
    /* Only save pass if not empty and not the placeholder ******** */
    if ((item = cJSON_GetObjectItem(root, "wifi_pass")) && cJSON_IsString(item)) {
        if (strlen(item->valuestring) > 0 && strcmp(item->valuestring, "********") != 0) {
            strncpy(new_cfg.wifi_pass, item->valuestring, sizeof(new_cfg.wifi_pass) - 1);
        } else {
            /* Keep existing password */
            const device_config_t *old_cfg = config_get();
            strncpy(new_cfg.wifi_pass, old_cfg->wifi_pass, sizeof(new_cfg.wifi_pass) - 1);
        }
    }
        
    if ((item = cJSON_GetObjectItem(root, "server_url")) && cJSON_IsString(item))
        strncpy(new_cfg.server_url, item->valuestring, sizeof(new_cfg.server_url) - 1);

    /* Only overwrite the token if a new one is supplied (not the ******** mask). */
    if ((item = cJSON_GetObjectItem(root, "api_token")) && cJSON_IsString(item)) {
        if (strlen(item->valuestring) > 0 && strcmp(item->valuestring, "********") != 0) {
            strncpy(new_cfg.api_token, item->valuestring, sizeof(new_cfg.api_token) - 1);
        } else {
            const device_config_t *old_cfg = config_get();
            strncpy(new_cfg.api_token, old_cfg->api_token, sizeof(new_cfg.api_token) - 1);
        }
    }

    if ((item = cJSON_GetObjectItem(root, "printer_ip")) && cJSON_IsString(item)) 
        strncpy(new_cfg.printer_ip, item->valuestring, sizeof(new_cfg.printer_ip) - 1);
        
    if ((item = cJSON_GetObjectItem(root, "printer_port")) && cJSON_IsNumber(item)) 
        new_cfg.printer_port = item->valueint;
        
    if ((item = cJSON_GetObjectItem(root, "use_static_ip")) && cJSON_IsBool(item)) 
        new_cfg.use_static_ip = cJSON_IsTrue(item);
        
    if ((item = cJSON_GetObjectItem(root, "static_ip")) && cJSON_IsString(item)) 
        strncpy(new_cfg.static_ip, item->valuestring, sizeof(new_cfg.static_ip) - 1);
        
    if ((item = cJSON_GetObjectItem(root, "gateway")) && cJSON_IsString(item)) 
        strncpy(new_cfg.gateway, item->valuestring, sizeof(new_cfg.gateway) - 1);
        
    if ((item = cJSON_GetObjectItem(root, "netmask")) && cJSON_IsString(item)) 
        strncpy(new_cfg.netmask, item->valuestring, sizeof(new_cfg.netmask) - 1);
        
    if ((item = cJSON_GetObjectItem(root, "dns")) && cJSON_IsString(item)) 
        strncpy(new_cfg.dns, item->valuestring, sizeof(new_cfg.dns) - 1);

    cJSON_Delete(root);

    /* Validate missing fields (store/device IDs are no longer required —
     * the backend identifies the printer by its API token). */
    const char* missing = NULL;
    if (strlen(new_cfg.wifi_ssid) == 0) missing = "WiFi SSID";
    else if (strlen(new_cfg.server_url) == 0) missing = "Server URL";
    else if (strlen(new_cfg.printer_ip) == 0) missing = "Printer IP";
    else if (strlen(new_cfg.api_token) == 0) missing = "API Token";

    if (missing) {
        char err_json[128];
        sprintf(err_json, "{\"error\":\"%s cannot be empty\"}", missing);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_send(req, err_json, HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    /* Save to NVS */
    config_save(&new_cfg);

    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);

    ESP_LOGW(TAG, "Config saved. Restarting in 1s...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

/* POST /api/test_print */
static esp_err_t post_test_print_handler(httpd_req_t *req)
{
    const device_config_t *cfg = config_get();
    if (!printer_test_connection(cfg->printer_ip, cfg->printer_port)) {
        s_printer_checked = true;
        s_printer_reachable = false;
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Printer unreachable");
        return ESP_OK;
    }
    s_printer_checked = true;
    s_printer_reachable = true;

    if (s_order_queue) {
        /* Plain ESC/POS text is fine for a self-test (no Chinese, no raster).
         * Build a binary-safe pointer order like the HTTP client does. */
        static const char test_text[] =
            "\x1b\x40"                /* ESC @  : init printer */
            "\n\n*** TEST PRINT ***\nPrinter connection is successful!\n\n\n\n"
            "\x1d\x56\x00";           /* GS V 0 : full cut */
        size_t len = sizeof(test_text) - 1;

        print_order_t *order = print_order_alloc("TEST-0001", len, 1);
        if (!order) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
            return ESP_OK;
        }
        memcpy(order->content, test_text, len);

        if (xQueueSend(s_order_queue, &order, pdMS_TO_TICKS(500)) == pdTRUE) {
            httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
        } else {
            print_order_free(order);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Queue full");
        }
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No queue");
    }
    return ESP_OK;
}

/* POST /api/test_server */
static esp_err_t post_test_server_handler(httpd_req_t *req)
{
    const device_config_t *cfg = config_get();
    if (test_server_reachable(cfg->server_url)) {
        httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Server TCP Ping Failed");
    }
    return ESP_OK;
}

/* POST /api/clear */
static esp_err_t post_clear_handler(httpd_req_t *req)
{
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(500));
    config_clear(); /* This will restart */
    return ESP_OK;
}

/* ── Public API ─────────────────────────────────────────────────────── */

esp_err_t web_config_server_start(web_config_mode_t mode, QueueHandle_t order_queue)
{
    s_mode = mode;
    s_order_queue = order_queue;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    httpd_handle_t server = NULL;

    ESP_LOGI(TAG, "Starting HTTP server on port: '%d'", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_get_index = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = get_index_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_get_index);

        httpd_uri_t uri_get_status = {
            .uri      = "/api/status",
            .method   = HTTP_GET,
            .handler  = get_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_get_status);

        httpd_uri_t uri_post_save = {
            .uri      = "/api/save",
            .method   = HTTP_POST,
            .handler  = post_save_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_post_save);

        httpd_uri_t uri_post_test_print = {
            .uri      = "/api/test_print",
            .method   = HTTP_POST,
            .handler  = post_test_print_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_post_test_print);

        httpd_uri_t uri_post_test_server = {
            .uri      = "/api/test_server",
            .method   = HTTP_POST,
            .handler  = post_test_server_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_post_test_server);

        httpd_uri_t uri_post_clear = {
            .uri      = "/api/clear",
            .method   = HTTP_POST,
            .handler  = post_clear_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_post_clear);

        /* Register WiFi scan/status/connect endpoints */
        wifi_http_register_handlers(server);

        /* Register captive portal redirects (must be last) */
        if (mode == WEB_CONFIG_MODE_AP) {
            wifi_portal_http_register(server);
        }

        return ESP_OK;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return ESP_FAIL;
}
