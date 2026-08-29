# ESP32 固件改动请求 — 切到 base64 拉单

> 把本文整篇交给在 `treysiz/esp32-printer-client` 仓库里工作的 Claude。
> 它是自包含的：不需要看后端代码。
>
> 后端侧改动已合并（`ai-phone-saas-backend` PR #78），**已上线且向后兼容** ——
> 不传新参数时行为与现在逐字一致，所以固件可以从容改。

---

## 背景：固件没错，是后端的载荷太大

现固件（v2.0.0，HTTP 拉单）与后端契约**完全对得上**，已用真实回环验证：
造一个含全部 256 种字节值的任务走真实接口，`scan_decode()` 还原后
**256/256 逐字节一致**。端点、Bearer、done/failed、`esp_crt_bundle` 都正确。

问题在后端把 ESC/POS 字节以 **latin1-in-JSON** 发出来：

小票是**光栅位图**，大量白像素是 `0x00`，而 JSON 把每个 `0x00` 转义成
`\u0000` —— **一字节变六字节**；`0x80–0xFF` 又各变 2 字节（UTF-8）。

535 张真实小票实测：

| 分位 | 原始 ESC/POS | latin1 JSON 响应 | 膨胀 |
|---|---|---|---|
| p50 | 66 KB | **371 KB** | 5.6x |
| p99 | 133 KB | 743 KB | 5.6x |

**98.9% 的小票单张就超过 `HTTP_RESP_PREFERRED`（256 KB）**，
固件只能走 `overflow` 分支整批丢弃 —— 一张也打不出来。

> 顺带说：固件那条日志
> `"(white-pixel 0x00 bytes expand 6x as \u0000 in JSON)"`
> 诊断得完全正确，只是修复得在后端。

后端还有个洞：`GET /jobs` 原本返回**全部** pending，实测积压 535 条时
单次响应 **207 MB**。现已加默认 `limit=1`。

---

## 后端新增的两个查询参数（都可选，不传=旧行为）

```
GET /printer-api/jobs?limit=1&encoding=base64
```

| 参数 | 说明 |
|---|---|
| `limit=N` | 最多返回 N 条。**默认 1**，上限 20。非法值收敛到默认值（不会返回 400） |
| `encoding=base64` | `content` 原样发 **base64**，不解码。**体积小 4.2 倍** |

响应每个 job 现在多一个字段 **`contentEncoding`**，值为 `"base64"` 或 `"latin1"`，
客户端据此选解码方式，**不用靠猜**。

响应结构其余部分不变：

```json
{ "success": true,
  "data": [ { "id": "...", "content": "<base64>", "contentEncoding": "base64",
              "copies": 1, "orderNumber": "...", ... } ] }
```

### 实测效果（同一个 535 条积压的真实库）

| 请求 | 响应体 | 条数 |
|---|---|---|
| 无参数（现状） | 580 KB | 1 |
| `?limit=1&encoding=base64` | **137 KB** | 1 |
| `?limit=999&encoding=base64` | 1711 KB | 20（封顶生效） |

base64 之后：中位 **137 KB**、最大约 **210 KB** —— 装得进 256 KB 的 PSRAM 缓冲区。

---

## 要改的地方

### 改动 1 — 拉单 URL 加参数

`do_poll_jobs()` 里：

```c
/* 改前 */
http_do(HTTP_METHOD_GET, "/printer-api/jobs", ...);

/* 改后 */
http_do(HTTP_METHOD_GET, "/printer-api/jobs?limit=1&encoding=base64", ...);
```

`limit=1` 与后端默认值一致，**显式写出来**是为了不依赖后端默认值哪天变化。

> 如果 `http_do()` 的 path 参数会被拼接/转义，确认 `?` 和 `&` 原样传出去。

### 改动 2 — 用 cJSON + base64 取 content，删掉平行游标

这是**主要收益**：base64 是纯 ASCII，cJSON 可以安全地直接给出字符串，
现在这套「用平行原始游标扫 `"content"` 键、靠文档顺序与 cJSON 数组对齐」的
技巧**整个可以删掉**（约 120 行，包括 `scan_decode()` 和 `next_content()`）。

在 `cJSON_ArrayForEach` 循环里改成：

```c
cJSON *enc_item = cJSON_GetObjectItemCaseSensitive(elem, "contentEncoding");
cJSON *c_item   = cJSON_GetObjectItemCaseSensitive(elem, "content");
const char *enc = cJSON_IsString(enc_item) ? enc_item->valuestring : "latin1";

if (strcmp(enc, "base64") == 0 && cJSON_IsString(c_item)) {
    size_t b64_len = strlen(c_item->valuestring);
    size_t need = 0;
    /* 先问需要多大：传 NULL/0 时 mbedtls 会把所需长度写进 need */
    mbedtls_base64_decode(NULL, 0, &need,
                          (const unsigned char *)c_item->valuestring, b64_len);
    uint8_t *buf = big_malloc(need ? need : 1);
    size_t out_len = 0;
    int rc = mbedtls_base64_decode(buf, need, &out_len,
                                   (const unsigned char *)c_item->valuestring, b64_len);
    /* rc == 0 → buf/out_len 就是原始 ESC/POS 字节，直接甩给 9100 端口 */
}
```

- `mbedtls/base64.h` 是 **ESP-IDF 自带**的，无需新增依赖。
- `big_malloc()` 沿用现有的 PSRAM 优先逻辑。

**建议保留 latin1 那条路作为兜底**：当 `contentEncoding != "base64"`
（比如对上了老后端）时仍走现有的 `scan_decode()`。那套代码已经写好且验证过，
留着不花什么成本，却能让固件对新旧后端都工作。
如果你更想要简洁，也可以直接删掉 latin1 路径 —— 但那样固件就**只能**配新后端。

### 改动 3 — 缓冲区注释更新

`HTTP_RESP_PREFERRED`（256 KB）**保持不变**，够用。
但把注释里的判断更新一下：改用 base64 后不再需要按 6 倍膨胀预留。

> ⚠ **PSRAM 仍是硬性要求**：base64 后单张仍有 137–210 KB，
> `HTTP_RESP_INTERNAL_MAX`（48 KB，无 PSRAM 时的上限）**永远装不下**。
> 无 PSRAM 的板子跑不了这套，这一点值得在 README 里写明。

---

## 不要动的东西

以下都已验证正确，**别在这次改动里顺手重构**：

- `POST /printer-api/heartbeat` 每 30 秒 —— 契约不变
- `POST /printer-api/jobs/:id/done` 和 `/failed` —— 契约不变
- `Authorization: Bearer <token>` —— 每个请求都要带
- 最近 20 个 job id 的**去重**逻辑（`ORDER_DEDUP_SIZE`）—— 后端在你回报 done
  之前那条任务仍是 `pending`，轮询会再看到它，去重必须留着
- 失败重试 ≤3 次后回报 `/failed`
- `esp_crt_bundle_attach` —— 生产走 HTTPS（Caddy 签的 Let's Encrypt 证书），
  这个根证书包能覆盖，别改成跳过证书校验
- `overflow` 时整批丢弃 + 报错的防御 —— 处理方式是对的，保留
- 无 PSRAM 时缩小缓冲区的降级逻辑 —— 防止 TLS 握手 OOM，保留

---

## 验收标准

改完请确认：

1. **字节无损**：随便找一张真实小票，base64 解出来的字节与后端
   `print_jobs.content`（去掉 `b64:` 前缀后 base64 解码）**逐字节相同**。
   尤其确认 `0x00` / `0x1B` / `0x1D` / `0x80–0xFF` 都没被改动。
2. **中文正常**：菜名是**光栅图**的一部分，只要字节无损就必然正确。
   如果中文乱码，说明某处把 content 当文本处理了 —— 回去查。
3. **单张 137 KB 左右的小票能完整走通**：拉取 → 打印 → `/done`，
   后端那条任务变成 `done` 且不再出现在下次轮询里。
4. **积压能自动排空**：连续造 5 单，固件应当一次拉一张、逐张打完，
   顺序与下单顺序一致（后端按 `createdAt` 先进先出）。
5. **断网恢复**：拔网线 → 造几单 → 插回。任务一条不丢，全部补打
   （后端不会丢弃 pending 任务，也没有过期时间）。

---

## 附：后端侧相关代码位置

（只在需要核对契约时看，改动不涉及后端）

- 路由：`src/modules/printer-api/routes.ts` — `GET /jobs` 的注释里有完整实测数据
- 入队：`src/adapters/printing/wifi.adapter.ts` — 为什么库里存的是 base64
- 契约文档：`docs/esp32-printer-integration.md`
- 守卫测试：`src/tests/printer-api-payload.test.ts`
