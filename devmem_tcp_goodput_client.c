#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <linux/socket.h>

// devmem TCP用の構造体定義
struct dmabuf_tx_cmsg {
    __u32 dmabuf_id;
};

// 時間測定用のユーティリティ関数
static inline long long get_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000LL + tv.tv_usec;
}

// グッドプット測定クライアント
int main(int argc, char *argv[]) {
    int client_fd;
    struct sockaddr_in server_addr;
    char *server_ip = "127.0.0.1";
    int port = 5201;
    int data_size = 1024 * 1024; // 1MB per send
    int test_duration = 10; // 測定時間（秒）
    int use_devmem = 0; // devmem送信を使用するか
    char *interface_name = "eth1"; // デフォルトのインターフェース名
    
    // 統計情報
    long long total_bytes = 0;
    long long total_packets = 0;
    long long start_time, end_time;
    
    if (argc > 1) {
        server_ip = argv[1];
    }
    if (argc > 2) {
        port = atoi(argv[2]);
    }
    if (argc > 3) {
        data_size = atoi(argv[3]);
    }
    if (argc > 4) {
        test_duration = atoi(argv[4]);
    }
    if (argc > 5) {
        use_devmem = atoi(argv[5]);
    }
    if (argc > 6) {
        interface_name = argv[6];
    }
    
    printf("devmem TCP goodput client\n");
    printf("Server: %s:%d\n", server_ip, port);
    printf("Data size per send: %d bytes\n", data_size);
    printf("Test duration: %d seconds\n", test_duration);
    printf("Use devmem: %s\n", use_devmem ? "Yes" : "No");
    printf("Interface: %s\n", interface_name);
    
    // ソケット作成
    client_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (client_fd < 0) {
        perror("socket creation failed");
        return 1;
    }
    
    // devmem送信を使用する場合の設定
    if (use_devmem) {
        int opt = 1;
        if (setsockopt(client_fd, SOL_SOCKET, SO_ZEROCOPY, &opt, sizeof(opt)) < 0) {
            perror("SO_ZEROCOPY failed");
            close(client_fd);
            return 1;
        }
        
        // デバイスバインディング（実際の実装では適切なインターフェース名を使用）
        // TODO: 実際のネットワークインターフェース名を動的に取得する
        if (setsockopt(client_fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name, strlen(interface_name) + 1) < 0) {
            perror("SO_BINDTODEVICE failed");
            // 警告として継続
        }
    }
    
    // サーバーアドレス設定
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(client_fd);
        return 1;
    }
    
    // サーバーに接続
    if (connect(client_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect failed");
        close(client_fd);
        return 1;
    }
    
    printf("Connected to server\n");
    
    // 送信データの準備
    char *data = malloc(data_size);
    if (!data) {
        perror("malloc failed");
        close(client_fd);
        return 1;
    }
    
    // テストパターンでデータを初期化
    for (int i = 0; i < data_size; i++) {
        data[i] = i % 256;
    }
    
    // 測定開始
    start_time = get_time_us();
    long long measurement_end = start_time + test_duration * 1000000LL;
    
    printf("Starting data transmission...\n");
    
    if (use_devmem) {
        // devmem送信モード（実際の実装では適切なdmabuf_idを使用）
        char ctrl_data[CMSG_SPACE(sizeof(struct dmabuf_tx_cmsg))];
        struct dmabuf_tx_cmsg ddmabuf;
        struct msghdr msg = {};
        struct cmsghdr *cmsg;
        struct iovec iov;
        
        // 仮のdmabuf_id（実際の実装では適切な値を設定）
        // TODO: dmabuf_helper.cから取得した実際のdmabuf_idを使用する
        __u32 tx_dmabuf_id = 0;
        
        // メッセージ構造体設定
        // TODO: 実際のdmabuf内のオフセットを計算する
        iov.iov_base = (void*)0; // dmabuf内のオフセット
        iov.iov_len = data_size;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = ctrl_data;
        msg.msg_controllen = sizeof(ctrl_data);
        
        // 制御メッセージ設定
        cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_DEVMEM_DMABUF;
        cmsg->cmsg_len = CMSG_LEN(sizeof(struct dmabuf_tx_cmsg));
        ddmabuf.dmabuf_id = tx_dmabuf_id;
        *((struct dmabuf_tx_cmsg *)CMSG_DATA(cmsg)) = ddmabuf;
        
        // 送信ループ
        while (get_time_us() < measurement_end) {
            ssize_t bytes_sent = sendmsg(client_fd, &msg, MSG_ZEROCOPY);
            if (bytes_sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000); // 1ms待機
                    continue;
                }
                perror("sendmsg failed");
                break;
            }
            
            total_bytes += bytes_sent;
            total_packets++;
            
            // 送信完了通知を処理（MSG_ZEROCOPY使用時）
            // TODO: 実際の実装では適切なエラーキュー処理を行う
            
            // 1秒ごとに進捗を表示
            static long long last_report = 0;
            long long current_time = get_time_us();
            if (current_time - last_report >= 1000000) {
                double elapsed = (current_time - start_time) / 1000000.0;
                double current_goodput = total_bytes / elapsed / 1024.0 / 1024.0 * 8.0;
                printf("Elapsed: %.1fs, Goodput: %.2f Mbps, Packets: %lld\n",
                       elapsed, current_goodput, total_packets);
                last_report = current_time;
            }
        }
    } else {
        // 通常の送信モード
        while (get_time_us() < measurement_end) {
            ssize_t bytes_sent = send(client_fd, data, data_size, 0);
            if (bytes_sent < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    usleep(1000); // 1ms待機
                    continue;
                }
                perror("send failed");
                break;
            }
            
            total_bytes += bytes_sent;
            total_packets++;
            
            // 1秒ごとに進捗を表示
            static long long last_report = 0;
            long long current_time = get_time_us();
            if (current_time - last_report >= 1000000) {
                double elapsed = (current_time - start_time) / 1000000.0;
                double current_goodput = total_bytes / elapsed / 1024.0 / 1024.0 * 8.0;
                printf("Elapsed: %.1fs, Goodput: %.2f Mbps, Packets: %lld\n",
                       elapsed, current_goodput, total_packets);
                last_report = current_time;
            }
        }
    }
    
    end_time = get_time_us();
    
    // 結果の計算と表示
    double duration = (end_time - start_time) / 1000000.0;
    double goodput_mbps = total_bytes / duration / 1024.0 / 1024.0 * 8.0;
    double packet_rate = total_packets / duration;
    
    printf("\n=== Transmission Results ===\n");
    printf("Duration: %.3f seconds\n", duration);
    printf("Total bytes sent: %lld bytes\n", total_bytes);
    printf("Total packets sent: %lld packets\n", total_packets);
    printf("Goodput: %.2f Mbps\n", goodput_mbps);
    printf("Packet rate: %.2f packets/sec\n", packet_rate);
    printf("Average packet size: %.1f bytes\n", 
           total_packets > 0 ? (double)total_bytes / total_packets : 0);
    
    // クリーンアップ
    free(data);
    close(client_fd);
    
    return 0;
}