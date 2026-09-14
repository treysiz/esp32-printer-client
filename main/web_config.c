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
/* 拉单任务起没起 —— 由 main.c 在启动后告知，供 /api/status 如实上报 */
static bool s_poller_running = false;

void web_config_set_poller_running(bool running) { s_poller_running = running; }

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
/* 锁定态：灰底 + 禁用光标，一眼看得出"这块现在不能改"。
   ⚠ 用 readonly 不用 disabled —— disabled 的字段**不会被提交**，
     而保存 handler 是从零拼配置的，字段没传就等于清零，
     锁反而会把后台地址抹掉（正好是它要保护的东西）。*/
".locked input { background:#e9ecef; color:#666; cursor:not-allowed; }"
".lockbar { display:flex; align-items:center; gap:8px; font-size:13px; margin-bottom:10px;"
"           background:#fff8e1; border:1px solid #ffe0a3; border-radius:6px; padding:8px 10px; }"
".lockbar button { flex:0 0 auto; padding:4px 12px; border:1px solid #ccc; border-radius:5px;"
"                  background:#fff; font-size:13px; cursor:pointer; }"
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
/* ── 今日订单 / 补打 ──────────────────────────────────────────────────
   用户 2026-09-13：员工在这里点一下就能补单，不用跑去登后台。
   ⚠ 单号/时间/金额全部来自后端 —— 板子不缓存、不自己数号，
     「每天从 1 开始」的真值源在后端 store_order_sequences。*/
"<div class='card'>"
"  <h3 id='t_orders'>今日订单</h3>"
"  <button type='button' class='btn btn-gray' id='t_btn_orders' onclick='loadOrders()'>刷新列表</button>"
"  <div id='orders_box' style='margin-top:10px;font-size:14px'></div>"
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
"    <button type='button' class='btn btn-gray' id='t_btn_wifitry' onclick='tryWifi()' style='margin-top:8px'>测试连接</button>"
"    <div id='wifi_st_box' style='background:#f0f7ff;padding:10px;border-radius:6px;margin-top:8px;font-size:13px'></div>"
"  </div>"
"  <div class='step' id='step2box'>"
"    <h4 id='t_step2'>第二步：连接打印机</h4>"
/* 锁提示条：第二/三步共用同一个状态，点哪个都一样。
   放两处是为了"在看的地方就能看见"，不是两把独立的锁。*/
"    <div class='lockbar' id='lockbar2'><span id='t_lockmsg2'></span>"
"      <button type='button' onclick='toggleLock()' id='t_lockbtn2'></button></div>"
"    <div class='group'><label id='t_pip'>打印机 IP</label><input type='text' id='printer_ip'></div>"
"    <div class='group'><label id='t_pport'>打印机端口 (默认9100)</label><input type='number' id='printer_port'></div>"
"  </div>"
"  <div class='step' id='step3box'>"
"    <h4 id='t_step3'>第三步：连接后台 (WiFi 拉单)</h4>"
"    <div class='lockbar' id='lockbar3'><span id='t_lockmsg3'></span>"
"      <button type='button' onclick='toggleLock()' id='t_lockbtn3'></button></div>"
/* ⚠ 占位符用**生产**写法。原来写的是 dev 那种 `http://IP:3000`，
 *   等于在引导人往 https 地址上也加端口 —— 2026-09-13 真配错过一次
 *   （填成 https://app.zhifood.com:3000，3000 不对外开 ⇒ 连不上）。 */
"    <div class='group'><label id='t_surl'>后台地址</label><input type='text' id='server_url' placeholder='https://app.example.com'>"
/* ⚠ setLang 用 innerText 覆盖，别放 HTML 标签 —— 会被抹掉。文案走下面的词典。 */
"      <div style='font-size:12px;color:#6c757d;margin-top:4px' id='t_surl_hint'></div></div>"
"    <div class='group'><label id='t_tok'>API Token</label><input type='text' id='api_token' placeholder='后台新增 WiFi 打印机时生成'></div>"
"    <button type='button' class='btn btn-gray' id='t_btn_bktry' onclick='tryBackend()' style='margin-top:8px'>测试后台连接</button>"
"    <div id='backend_st_box' style='background:#f0f7ff;padding:10px;border-radius:6px;margin-top:8px;font-size:13px'></div>"
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
"  zh: { title:'PrinterBox 打印盒设置', st_wifi:'WiFi状态', st_cloud:'后台状态', st_printer:'打印机连通', st_ip:'局域网IP', btn_test_net:'测网络', btn_test_print:'测打印', btn_wifitry:'测试连接', btn_bktry:'测试后台连接', cfg_title:'配置向导', info:'设备信息', orders:'今日订单', btn_orders:'刷新列表', ver:'固件版本:', uptime:'运行时长:', step1:'第一步：连接店内 WiFi', ssid:'WiFi 名称', pass:'WiFi 密码 (为空则不修改)', step2:'第二步：连接打印机', lock_on:'已锁定 · 装机时设好的，平时别动', lock_off:'⚠ 已解锁 —— 改错会收不到订单或打不出票', lock_unlock:'解锁', lock_lock:'锁上', lock_confirm:'这些是装机时设好的。改错会导致收不到订单、或者打不出小票。确定要解锁吗？', pip:'打印机 IP', pport:'打印机端口', step3:'第三步：连接后台 (WiFi 拉单)', surl:'后台地址', surl_hint:'生产：https://域名（不要带端口）  本地开发：http://IP:3000', tok:'API Token (为空则不修改)', adv_toggle:'展开高级设置 (固定IP)', use_static:'使用固定 IP', btn_save:'保存并重启', btn_reset:'恢复出厂设置', msg_reset:'确定要清空所有设置并恢复出厂吗？', msg_reset_ok:'已清空，设备正在重启...' },"
"  en: { title:'PrinterBox Settings', st_wifi:'WiFi Status', st_cloud:'Backend Status', st_printer:'Printer Link', st_ip:'LAN IP', btn_test_net:'Test Net', btn_test_print:'Test Print', btn_wifitry:'Test Connect', btn_bktry:'Test Backend', cfg_title:'Setup Wizard', info:'Device Info', orders:'Today Orders', btn_orders:'Refresh', ver:'Firmware:', uptime:'Uptime:', step1:'Step 1: Connect WiFi', ssid:'WiFi Name', pass:'WiFi Password (leave blank to keep)', step2:'Step 2: Connect Printer', lock_on:'🔒 Locked - set at install, leave alone', lock_off:'⚠ Unlocked - wrong values break orders or printing', lock_unlock:'Unlock', lock_lock:'Lock', lock_confirm:'These were set at install. Wrong values stop orders coming in, or stop receipts printing. Unlock anyway?', pip:'Printer IP', pport:'Printer Port', step3:'Step 3: Connect Backend (WiFi Pull)', surl:'Backend URL', surl_hint:'Production: https://domain (no port).  Dev only: http://IP:3000', tok:'API Token (leave blank to keep)', adv_toggle:'Advanced Settings (Static IP)', use_static:'Use Static IP', btn_save:'Save & Reboot', btn_reset:'Factory Reset', msg_reset:'Erase all settings and factory reset?', msg_reset_ok:'Erased! Rebooting...' }"
"};"
"let lang = 'zh';"
/* 今日订单：拉列表 + 逐行补打。
   ★ 用 DOM API 逐个建元素，不拼 HTML 字符串：
     ① 这段 JS 嵌在 C 字符串里，每多一层引号就多一个出错点
     ② textContent 天然免疫注入（客人姓名里带 < 也不会出事）
   ★ 补打按钮点下去立刻禁用 —— 员工手快连点两下会真打出两张。*/
"async function loadOrders() {"
"  var box=document.getElementById('orders_box');"
"  var btn=document.getElementById('t_btn_orders'); var o=btn.innerText;"
"  btn.disabled=true; btn.innerText=(lang==='zh'?'读取中...':'Loading...');"
"  box.textContent='';"
"  try {"
"    var r=await fetch('/api/orders_today'); var d=await r.json();"
"    if(!d.success){ box.textContent=(lang==='zh'?'读取失败':'Failed')+(d.status?(' HTTP '+d.status):''); box.style.color='#dc3545'; }"
"    else if(!d.data||!d.data.length){ box.textContent=(lang==='zh'?'今天还没有订单':'No orders today'); box.style.color='#666'; }"
"    else { box.style.color=''; d.data.forEach(function(x){ box.appendChild(orderRow(x)); }); }"
"  } catch(e){ box.textContent=(lang==='zh'?'请求失败':'Request failed'); box.style.color='#dc3545'; }"
"  btn.disabled=false; btn.innerText=o;"
"}"
"function orderRow(x) {"
"  var row=document.createElement('div');"
"  row.style.cssText='border-bottom:1px solid #eee;padding:8px 0;display:flex;align-items:center;gap:10px';"
"  var left=document.createElement('div'); left.style.cssText='flex:1;min-width:0';"
"  var head=document.createElement('div');"
"  var num=document.createElement('b'); num.textContent=x.orderNumber||'';"
"  head.appendChild(num);"
"  var t=x.createdAt?new Date(x.createdAt).toLocaleTimeString([],{hour:'numeric',minute:'2-digit'}):'';"
"  head.appendChild(document.createTextNode(' · '+t+' · $'+(x.total||'')));"
"  left.appendChild(head);"
"  var who=[x.customerName,x.customerPhone].filter(Boolean).join(' · ');"
"  if(who){ var w=document.createElement('div'); w.style.cssText='color:#666;font-size:12px'; w.textContent=who; left.appendChild(w); }"
"  if(x.deliveryAddress){ var a=document.createElement('div'); a.style.cssText='color:#666;font-size:12px'; a.textContent=x.deliveryAddress; left.appendChild(a); }"
"  row.appendChild(left);"
"  var b=document.createElement('button'); b.type='button'; b.className='scan-btn';"
"  b.textContent=(lang==='zh'?'补打':'Reprint');"
"  b.onclick=function(){ reprint(b, x.id); };"
"  row.appendChild(b);"
"  return row;"
"}"
"async function reprint(btn,id) {"
"  btn.disabled=true; btn.textContent=(lang==='zh'?'打印中':'...');"
"  try {"
"    var r=await fetch('/api/reprint',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:id})});"
"    var d=await r.json();"
"    btn.textContent = d.success ? (lang==='zh'?'已补打':'Sent') : (lang==='zh'?'失败':'Failed');"
"    if(!d.success) btn.disabled=false;"
"  } catch(e){ btn.textContent=(lang==='zh'?'失败':'Failed'); btn.disabled=false; }"
"}"
/* ── 第二/三步的锁 ────────────────────────────────────────────────────
   用户 2026-09-13：「加个按钮锁，以防老板们误删」。
   打印机 IP / 端口 / 后台地址 / API Token 都是装机时设一次的东西，
   而 WiFi 密码是真会变的 —— 所以第一步不锁。

   ★ **每次打开页面都是锁上的**，不做持久化。
     locked 是默认状态，就没有"上次忘了锁"这种情况。
   ★ 用 readOnly 不用 disabled：disabled 的字段不会被提交，
     而保存 handler 省略即清零 —— 锁反而会抹掉它要保护的值。*/
"var locked = true;"
"var LOCK_FIELDS = ['printer_ip','printer_port','server_url','api_token'];"
"function applyLock() {"
"  LOCK_FIELDS.forEach(function(id){ var el=document.getElementById(id); if(el) el.readOnly=locked; });"
"  ['step2box','step3box'].forEach(function(id){"
"    var el=document.getElementById(id); if(el) el.classList.toggle('locked', locked); });"
"  var msg = locked ? (dict[lang].lock_on) : (dict[lang].lock_off);"
"  var btn = locked ? (dict[lang].lock_unlock) : (dict[lang].lock_lock);"
"  ['2','3'].forEach(function(n){"
/* \u26A0 \u8FD9\u91CC**\u4E0D\u8981\u653E\u8868\u60C5**\uFF0C\u6587\u6848\u5168\u8D70\u8BCD\u5178\uFF08lock_on / lock_off\uFF09\u3002
   \u8FD9\u6BB5 JS \u662F\u5D4C\u5728 C \u5B57\u7B26\u4E32\u91CC\u7684\uFF1AJS \u7684\u4EE3\u7406\u5BF9\u8F6C\u4E49\u5199\u6CD5\uFF08\u53CD\u659C\u6760 u D83D \u90A3\u79CD\uFF09
   \u4F1A\u88AB C \u7F16\u8BD1\u5668\u5F53\u6210\u5B83\u81EA\u5DF1\u7684\u300C\u901A\u7528\u5B57\u7B26\u540D\u300D\uFF0C\u800C\u534A\u4E2A\u4EE3\u7406\u7801\u4F4D\u5728 C \u91CC\u975E\u6CD5\uFF0C
   \u76F4\u63A5\u7F16\u8BD1\u62A5\u9519 "is not a valid universal character"\u3002
   \u2014\u2014 \u9501\u7684\u8FA8\u8BC6\u5EA6\u9760\u5F69\u8272\u63D0\u793A\u6761 + \u6587\u5B57\uFF0C\u672C\u6765\u4E5F\u4E0D\u9700\u8981\u56FE\u6807\u3002*/
"    var m=document.getElementById('t_lockmsg'+n); if(m) m.textContent=msg;"
"    var b=document.getElementById('t_lockbtn'+n); if(b) b.textContent=btn;"
"    var bar=document.getElementById('lockbar'+n);"
"    if(bar){ bar.style.background = locked?'#fff8e1':'#fdecea'; bar.style.borderColor = locked?'#ffe0a3':'#f5c2c7'; }"
"  });"
"}"
"function toggleLock() {"
"  if (locked && !confirm(dict[lang].lock_confirm)) return;"
"  locked = !locked; applyLock();"
"}"
"function setLang(l) { lang = l; for(let k in dict[l]) { let el = document.getElementById('t_'+k); if(el) el.innerText = dict[l][k]; } document.getElementById('wifi_pass').placeholder = dict[l].pass; applyLock(); renderStatus(); }"
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
"  const sel=document.getElementById('wifi_ssid').value;"
"  const m=document.getElementById('wifi_ssid_manual').value.trim();"
"  return sel||m;"
"}"
"async function tryWifi(){"
"  const ssid=getSelectedSSID(); if(!ssid){ alert(lang==='zh'?'请先选择或输入 WiFi 名称':'Pick or enter a WiFi name first'); return; }"
"  const pass=document.getElementById('wifi_pass').value;"
"  const btn=document.getElementById('t_btn_wifitry'); const o=btn.innerText; btn.disabled=true; btn.innerText=lang==='zh'?'连接中...':'Connecting...';"
"  const box=document.getElementById('wifi_st_box');"
"  box.innerHTML=(lang==='zh'?'正在尝试连接 ':'Trying ')+ssid+' ...';"
"  try{"
"    const r=await fetch('/api/wifi_try',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:ssid,pass:pass})});"
"    const d=await r.json();"
"    if(d.ok){ box.innerHTML='<span style=\\'color:green\\'>\\u2713 '+(lang==='zh'?'连接成功，IP: ':'Connected, IP: ')+d.ip+(lang==='zh'?'（现在可以点保存）':' (you can Save now)')+'</span>'; }"
"    else{ const m={ap_not_found:(lang==='zh'?'找不到该 WiFi（名称错/不在范围/可能是5G）':'AP not found'),wrong_password:(lang==='zh'?'密码错误或认证失败':'Wrong password'),timeout:(lang==='zh'?'连接超时':'Timeout'),empty_ssid:(lang==='zh'?'SSID 为空':'Empty SSID')}; box.innerHTML='<span style=\\'color:#dc3545\\'>\\u2717 '+(lang==='zh'?'连接失败: ':'Failed: ')+(m[d.error]||d.error||'?')+'</span>'; }"
"  }catch(e){ box.innerHTML='<span style=\\'color:#dc3545\\'>'+(lang==='zh'?'请求失败，请重试':'Request failed')+'</span>'; }"
"  btn.disabled=false; btn.innerText=o;"
"}"
"async function tryBackend(){"
"  let url=document.getElementById('server_url').value.trim();"
"  let tok=document.getElementById('api_token').value;"
"  const btn=document.getElementById('t_btn_bktry'); const o=btn.innerText; btn.disabled=true; btn.innerText=lang==='zh'?'测试中...':'Testing...';"
"  const box=document.getElementById('backend_st_box');"
"  box.innerHTML=(lang==='zh'?'正在测试后台...':'Testing backend...');"
"  try{"
"    const r=await fetch('/api/backend_try',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({url:url,token:tok})});"
"    const d=await r.json();"
"    if(d.ok){ box.innerHTML='<span style=\\'color:green\\'>\\u2713 '+(lang==='zh'?'后台连接成功（在线）':'Backend OK (online)')+'</span>'; }"
"    else{ const m={bad_token:(lang==='zh'?'Token 错误或未激活':'Invalid token'),not_wifi_provider:(lang==='zh'?'该打印机不是“WiFi 拉单”类型':'Printer is not WiFi-pull'),unreachable:(lang==='zh'?'连不上后台（检查地址/同网/防火墙/先连好WiFi）':'Unreachable'),https_port:(lang==='zh'?'连不上：https 地址不要带端口。生产填 https://域名，:3000 那种写法只在本地开发用':'Unreachable: drop the port from an https:// URL. Production is https://domain — the :3000 form is dev-only'),bad_url:(lang==='zh'?'地址格式错（需 http:// 开头）':'Bad URL'),no_token:(lang==='zh'?'未填 Token':'No token')}; box.innerHTML='<span style=\\'color:#dc3545\\'>\\u2717 '+(lang==='zh'?'失败: ':'Failed: ')+(m[d.error]||d.error||('HTTP '+d.status))+'</span>'; }"
"  }catch(e){ box.innerHTML='<span style=\\'color:#dc3545\\'>'+(lang==='zh'?'请求失败，请重试':'Request failed')+'</span>'; }"
"  btn.disabled=false; btn.innerText=o;"
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

/*
 * ── 今日订单 / 补打 ──────────────────────────────────────────────────
 *
 * 用户 2026-09-13：员工在打印盒网页上直接补单，不用跑去登后台。
 *
 * ★ 网页**不直接连后端** —— 那样 Bearer token 得发给浏览器，而且跨域。
 *   这里由固件转发：token 留在设备里，网页只知道两个本地接口。
 *
 * ★ 板子**不缓存**订单列表，每次点刷新都回后端要。
 *   理由：订单号「每天从 1 开始」的真值源在后端（store_order_sequences），
 *   板子自己存一份必然会在跨营业日/断电/重启时和后端对不上。
 *   —— 这一条是设计约束，别为了「快一点」加缓存。
 */

/* 今日订单列表大小：后端上限 200 单 × 每单约 150 字节，48 KB 绰绰有余。
   ⚠ 这里**不会**出现小票内容（那是 137-210 KB/张）—— 后端那个接口只给字段。*/
#define TODAY_ORDERS_BUF  (48 * 1024)

static esp_err_t get_orders_today_handler(httpd_req_t *req)
{
    char *buf = malloc(TODAY_ORDERS_BUF);
    if (!buf) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"success\":false,\"error\":\"no_memory\"}");
        return ESP_OK;
    }
    int len = 0;
    int status = http_client_fetch_today_orders(buf, TODAY_ORDERS_BUF - 1, &len);

    httpd_resp_set_type(req, "application/json");
    if (status == 200 && len > 0) {
        buf[len] = 0;
        httpd_resp_sendstr(req, buf);      /* 后端 JSON 原样透传 */
    } else {
        /* ★ 把后端状态码带出去 —— 不然网页只能说"失败"，跟固件那次
         *   "连不上后台/检查WiFi" 一样把人往错方向引。 */
        char err[96];
        snprintf(err, sizeof(err),
                 "{\"success\":false,\"error\":\"backend\",\"status\":%d}", status);
        httpd_resp_sendstr(req, err);
    }
    free(buf);
    return ESP_OK;
}

static esp_err_t post_reprint_handler(httpd_req_t *req)
{
    char body[200];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad length");
        return ESP_OK;
    }
    int rec = 0, r;
    while (rec < total) {
        r = httpd_req_recv(req, body + rec, total - rec);
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; return ESP_FAIL; }
        rec += r;
    }
    body[rec] = 0;

    cJSON *root = cJSON_Parse(body);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_OK; }
    cJSON *it = cJSON_GetObjectItem(root, "id");
    char id[48] = {0};
    if (it && cJSON_IsString(it)) strncpy(id, it->valuestring, sizeof(id) - 1);
    cJSON_Delete(root);

    char resp[256] = {0};
    /* 字符集校验在 http_client_reprint_order 里做（path 是那边拼的，
       注入点也在那边）—— 这里只管取参数。 */
    int status = http_client_reprint_order(id, resp, sizeof(resp) - 1);

    httpd_resp_set_type(req, "application/json");
    if (status == 200) {
        httpd_resp_sendstr(req, resp[0] ? resp : "{\"success\":true}");
    } else {
        char err[96];
        snprintf(err, sizeof(err),
                 "{\"success\":false,\"error\":\"backend\",\"status\":%d}", status);
        httpd_resp_sendstr(req, err);
    }
    return ESP_OK;
}
/* GET /api/status */
static esp_err_t get_status_handler(httpd_req_t *req)
{
    const device_config_t *cfg = config_get();
    cJSON *root = cJSON_CreateObject();
    
    /* ⚠ 用**实时** WiFi 状态，不是 s_mode 那个开机瞬间的快照。
     *   快照会一直说 "ap"，而设备其实早就连上店里 WiFi 了 —— 排查时极其误导
     *   （2026-09-13：我就是先看到 mode=ap 才顺藤摸到真正的 bug）。
     *   s_mode 保留给 web_config_server_start 决定 UI 形态，不再对外报。 */
    cJSON_AddStringToObject(root, "mode", wifi_mgr_is_connected() ? "sta" : "ap");
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_mgr_is_connected());
    /* "ws_connected" key kept for UI compatibility; now means backend online. */
    cJSON_AddBoolToObject(root, "ws_connected", http_client_is_connected());
    /* ★ 拉单任务到底起没起 —— 以前它可能被静默跳过而外面完全看不出来。
     *   ws_connected 顶不上：那只说明"上次请求通了"，任务没起时它恒 false，
     *   和"后台挂了"长得一模一样。 */
    cJSON_AddBoolToObject(root, "poller_running", s_poller_running);
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
        
    /*
     * ⚠ 空值 = **保留原值**，和 wifi_pass / api_token 同一套规矩。
     *
     * 这个 handler 是 memset 从零拼配置的，所以「字段没传」或「传了空串」
     * 默认等于**清零**。后台地址一旦被抹空，订单再也拉不下来，
     * 而网页上一切正常 —— 又一个「看着好好的、什么都不工作」。
     *
     * 页面上把第二/三步锁起来只是防手滑；**真正的防线在这里**：
     * 任何客户端（包括我自己写的脚本）都不该有能力把它清空。
     * 要改就得传一个非空的新值。
     */
    if ((item = cJSON_GetObjectItem(root, "server_url")) && cJSON_IsString(item)
        && strlen(item->valuestring) > 0) {
        strncpy(new_cfg.server_url, item->valuestring, sizeof(new_cfg.server_url) - 1);
    } else {
        const device_config_t *old_cfg = config_get();
        strncpy(new_cfg.server_url, old_cfg->server_url, sizeof(new_cfg.server_url) - 1);
    }

    /* Only overwrite the token if a new one is supplied (not the ******** mask). */
    if ((item = cJSON_GetObjectItem(root, "api_token")) && cJSON_IsString(item)) {
        if (strlen(item->valuestring) > 0 && strcmp(item->valuestring, "********") != 0) {
            strncpy(new_cfg.api_token, item->valuestring, sizeof(new_cfg.api_token) - 1);
        } else {
            const device_config_t *old_cfg = config_get();
            strncpy(new_cfg.api_token, old_cfg->api_token, sizeof(new_cfg.api_token) - 1);
        }
    }

    /* 同上：空值保留原值 —— 打印机 IP 被抹空 = 订单照进、票一张不出 */
    if ((item = cJSON_GetObjectItem(root, "printer_ip")) && cJSON_IsString(item)
        && strlen(item->valuestring) > 0) {
        strncpy(new_cfg.printer_ip, item->valuestring, sizeof(new_cfg.printer_ip) - 1);
    } else {
        const device_config_t *old_cfg = config_get();
        strncpy(new_cfg.printer_ip, old_cfg->printer_ip, sizeof(new_cfg.printer_ip) - 1);
    }

    /* 端口 0 是非法值，当"没传"处理 —— 否则清零后连不上打印机 */
    if ((item = cJSON_GetObjectItem(root, "printer_port")) && cJSON_IsNumber(item)
        && item->valueint > 0) {
        new_cfg.printer_port = item->valueint;
    } else {
        new_cfg.printer_port = config_get()->printer_port;
    }
        
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

/* POST /api/wifi_try  {"ssid":"..","pass":".."}
 *   -> {"ok":true,"ip":"x.x.x.x"} | {"ok":false,"error":"wrong_password"|...}
 * Tries the given creds live (AP portal stays up); lets the user confirm a
 * working connection BEFORE saving. Nothing is written to NVS here. */
static esp_err_t post_wifi_try_handler(httpd_req_t *req)
{
    char buf[300];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad length");
        return ESP_OK;
    }
    int rec = 0, r;
    while (rec < total) {
        r = httpd_req_recv(req, buf + rec, total - rec);
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; return ESP_FAIL; }
        rec += r;
    }
    buf[rec] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_OK; }
    char ssid[33] = {0}, pass[65] = {0};
    cJSON *it;
    if ((it = cJSON_GetObjectItem(root, "ssid")) && cJSON_IsString(it))
        strncpy(ssid, it->valuestring, sizeof(ssid) - 1);
    if ((it = cJSON_GetObjectItem(root, "pass")) && cJSON_IsString(it))
        strncpy(pass, it->valuestring, sizeof(pass) - 1);
    cJSON_Delete(root);

    esp_netif_ip_info_t ip = {0};
    char err[48] = {0};
    esp_err_t e = wifi_mgr_try_connect(ssid, pass, 12000, &ip, err, sizeof(err));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", e == ESP_OK);
    if (e == ESP_OK) {
        char ipstr[16] = {0};
        esp_ip4addr_ntoa(&ip.ip, ipstr, sizeof(ipstr));
        cJSON_AddStringToObject(resp, "ip", ipstr);
    } else {
        cJSON_AddStringToObject(resp, "error", err);
    }
    char *js = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, js ? js : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    cJSON_Delete(resp);
    if (js) free(js);
    return ESP_OK;
}

/* POST /api/backend_try  {"url":"http://..","token":".."}
 *   -> {"ok":true} | {"ok":false,"status":N,"error":"bad_token"|...}
 * Tests the typed backend URL+token live (real heartbeat). Blank/masked values
 * fall back to the saved config so "keep current" still works. No NVS write. */
static esp_err_t post_backend_try_handler(httpd_req_t *req)
{
    char buf[400];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad length");
        return ESP_OK;
    }
    int rec = 0, r;
    while (rec < total) {
        r = httpd_req_recv(req, buf + rec, total - rec);
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; return ESP_FAIL; }
        rec += r;
    }
    buf[rec] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON"); return ESP_OK; }
    char url[MAX_URL_LEN + 1] = {0}, token[MAX_TOKEN_LEN + 1] = {0};
    cJSON *it;
    if ((it = cJSON_GetObjectItem(root, "url")) && cJSON_IsString(it))
        strncpy(url, it->valuestring, sizeof(url) - 1);
    if ((it = cJSON_GetObjectItem(root, "token")) && cJSON_IsString(it))
        strncpy(token, it->valuestring, sizeof(token) - 1);
    cJSON_Delete(root);

    /* Fall back to saved values for blank / masked input. */
    const device_config_t *cfg = config_get();
    if (strlen(url) == 0)   strncpy(url, cfg->server_url, sizeof(url) - 1);
    if (strlen(token) == 0 || strcmp(token, "********") == 0)
        strncpy(token, cfg->api_token, sizeof(token) - 1);

    char err[64] = {0};
    int status = http_client_test_backend(url, token, err, sizeof(err));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", status == 200);
    cJSON_AddNumberToObject(resp, "status", status);
    if (status != 200) cJSON_AddStringToObject(resp, "error", err);
    char *js = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, js ? js : "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    cJSON_Delete(resp);
    if (js) free(js);
    return ESP_OK;
}

esp_err_t web_config_server_start(web_config_mode_t mode, QueueHandle_t order_queue)
{
    s_mode = mode;
    s_order_queue = order_queue;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 15;   /* allow long handlers (backend HTTPS test) */
    config.send_wait_timeout = 15;
    /* The "test backend" handler performs an HTTPS (mbedTLS) request inline;
     * the default 4 KB httpd task stack is too small for a TLS handshake, so
     * give it room. */
    config.stack_size = 12288;
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

        httpd_uri_t uri_get_orders_today = {
            .uri      = "/api/orders_today",
            .method   = HTTP_GET,
            .handler  = get_orders_today_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_get_orders_today);

        httpd_uri_t uri_post_reprint = {
            .uri      = "/api/reprint",
            .method   = HTTP_POST,
            .handler  = post_reprint_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_post_reprint);

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

        httpd_uri_t uri_post_wifi_try = {
            .uri      = "/api/wifi_try",
            .method   = HTTP_POST,
            .handler  = post_wifi_try_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_post_wifi_try);

        httpd_uri_t uri_post_backend_try = {
            .uri      = "/api/backend_try",
            .method   = HTTP_POST,
            .handler  = post_backend_try_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_post_backend_try);

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
