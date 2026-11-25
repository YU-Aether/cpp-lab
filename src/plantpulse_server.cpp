#include "someip_server.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>

int main(int argc, char* argv[])
{
    // 用法：plantpulse_server [端口]，默认 9555
    std::uint16_t port = 9555;
    if (argc > 1) {
        const int value = std::atoi(argv[1]);
        if (value <= 0 || value > 65535) {
            std::fprintf(stderr, "端口非法: %s\n", argv[1]);
            return 1;
        }
        port = static_cast<std::uint16_t>(value);
    }

    try {
        lab::SomeipServer server(port);
        std::printf("SOME/IP 服务端已启动，监听 127.0.0.1:%u\n", server.port());
        std::printf("按 Ctrl+C 退出\n");
        std::fflush(stdout);
        server.run();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "启动失败: %s\n", error.what());
        return 1;
    }

    return 0;
}