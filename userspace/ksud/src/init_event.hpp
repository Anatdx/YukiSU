#pragma once

namespace ksud {

int on_post_data_fs();
void on_services();
void on_boot_completed();

// 运行为常驻 daemon，启动 Binder 服务
int run_daemon();

}  // namespace ksud
