#include "sd_discovery.h"
#include "someip_server.h"

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int)
{
    g_stop = 1;
}

}  // namespace

int main(int argc, char* argv[])
{
    // 用法：plantpulse_server [TCP 端口] [SD 端口]
    std::uint16_t tcp_port = 9555;
    std::uint16_t sd_port = 30490;

    if (argc > 1) {
        const int value = std::atoi(argv[1]);
        if (value <= 0 || value > 65535) {
            std::fprintf(stderr, "TCP 端口非法: %s\n", argv[1]);
            return 1;
        }
        tcp_port = static_cast<std::uint16_t>(value);
    }
    if (argc > 2) {
        const int value = std::atoi(argv[2]);
        if (value <= 0 || value > 65535) {
            std::fprintf(stderr, "SD 端口非法: %s\n", argv[2]);
            return 1;
        }
        sd_port = static_cast<std::uint16_t>(value);
    }

    try {
        lab::SomeipServer tcp_server(tcp_port);
        // 多播场景下通告真实网卡地址，避免对端拿到 127.0.0.1
        lab::someip::sd::SdResponder responder(
            sd_port, tcp_server.port(), 0x1234, 0xFFFF,
            lab::someip::sd::local_ipv4_address());
        responder.enable_multicast("224.244.224.245", std::chrono::milliseconds(1000));

        std::thread tcp_thread([&tcp_server] { tcp_server.run(); });
        std::thread sd_thread([&responder] { responder.run(); });

        std::printf("SOME/IP 服务端已启动\n");
        std::printf("  TCP 业务端口: %u\n", tcp_server.port());
        std::printf("  SD  发现端口: %u\n", responder.port());
        std::printf("  通告地址: %s\n", lab::someip::sd::local_ipv4_address().c_str());
        std::printf("  多播组: 224.244.224.245:%u（每 1s 广播 OfferService）\n",
                    responder.port());
        std::printf("按 Ctrl+C 退出\n");
        std::fflush(stdout);

        std::signal(SIGINT, handle_signal);
        std::signal(SIGTERM, handle_signal);

        while (g_stop == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        tcp_server.stop();
        responder.stop();
        tcp_thread.join();
        sd_thread.join();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "启动失败: %s\n", error.what());
        return 1;
    }

    std::printf("已退出\n");
    return 0;
}