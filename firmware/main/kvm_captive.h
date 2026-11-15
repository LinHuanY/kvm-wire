#pragma once

/* 启动强制门户（Captive Portal）辅助：
 * - UDP 53 DNS 劫持：所有 A 查询都返回 AP 的 IP
 * - 配合 kvm_http 里的探测 URL 302 跳转，让手机/电脑连上 AP 后自动弹配置页
 */
void kvm_captive_start(void);
