#include <arpa/inet.h>
#include <errno.h>
#include <linux/socket.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

// devmem TCP用の構造体定義
struct dmabuf_cmsg {
  uint32_t frag_token;
  uint32_t frag_size;
  uint32_t frag_offset;
  uint32_t dmabuf_id;
};

struct dmabuf_token {
  uint32_t token_start;
  uint32_t token_count;
};

// 時間測定用のユーティリティ関数
static inline long long get_time_us(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000000LL + tv.tv_usec;
}

// TODO: correct?
#define NETDEV_CMD_BIND_RX 1

// グッドプット測定サーバー
int main(int argc, char *argv[]) {
  int server_fd, client_fd;
  struct sockaddr_in server_addr, client_addr;
  socklen_t client_len = sizeof(client_addr);
  int port = 5201;
  int measurement_duration = 10; // 測定時間（秒）

  // 統計情報
  long long total_bytes = 0;
  long long total_packets = 0;
  long long devmem_bytes = 0;
  long long linear_bytes = 0;
  long long start_time, end_time;

  if (argc > 1) {
    port = atoi(argv[1]);
  }
  if (argc > 2) {
    measurement_duration = atoi(argv[2]);
  }

  // ソケット作成
  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket creation failed");
    return 1;
  }

  // SO_REUSEADDRオプション設定
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  // アドレス設定
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);

  // バインド
  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) <
      0) {
    perror("bind failed");
    close(server_fd);
    return 1;
  }

  // リスニング
  if (listen(server_fd, 1) < 0) {
    perror("listen failed");
    close(server_fd);
    return 1;
  }

  printf("devmem TCP goodput server listening on port %d\n", port);
  printf("Measurement duration: %d seconds\n", measurement_duration);

  // クライアント接続を待機
  client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
  if (client_fd < 0) {
    perror("accept failed");
    close(server_fd);
    return 1;
  }

  printf("Client connected from %s\n", inet_ntoa(client_addr.sin_addr));

  // 測定開始
  start_time = get_time_us();
  long long measurement_end = start_time + measurement_duration * 1000000LL;

  // 受信バッファとメッセージ構造体
  char buffer[65536];
  char ctrl_buffer[CMSG_SPACE(sizeof(struct dmabuf_cmsg))];
  struct msghdr msg;
  struct iovec iov;
  struct cmsghdr *cmsg;

  // メッセージ構造体初期化
  memset(&msg, 0, sizeof(msg));
  iov.iov_base = buffer;
  iov.iov_len = sizeof(buffer);
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = ctrl_buffer;
  msg.msg_controllen = sizeof(ctrl_buffer);

  printf("Starting measurement...\n");

  // メインループ
  while (get_time_us() < measurement_end) {
    // MSG_SOCK_DEVMEMフラグを使用してdevmemデータを受信
    ssize_t bytes_received = recvmsg(client_fd, &msg, MSG_SOCK_DEVMEM);

    if (bytes_received <= 0) {
      if (bytes_received == 0) {
        printf("Connection closed by client\n");
        break;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        continue;
      }
      perror("recvmsg failed");
      break;
    }

    total_bytes += bytes_received;
    total_packets++;

    // 制御メッセージを解析
    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
      if (cmsg->cmsg_level != SOL_SOCKET) {
        continue;
      }

      struct dmabuf_cmsg *dmabuf_cmsg = (struct dmabuf_cmsg *)CMSG_DATA(cmsg);

      if (cmsg->cmsg_type == SCM_DEVMEM_DMABUF) {
        // デバイスメモリに受信されたフラグメント
        devmem_bytes += dmabuf_cmsg->frag_size;

        printf("Devmem frag: dmabuf_id=%u, offset=%u, size=%u, token=%u\n",
               dmabuf_cmsg->dmabuf_id, dmabuf_cmsg->frag_offset,
               dmabuf_cmsg->frag_size, dmabuf_cmsg->frag_token);

        // フラグメントを解放
        struct dmabuf_token token;
        token.token_start = dmabuf_cmsg->frag_token;
        token.token_count = 1;

        int ret = setsockopt(client_fd, SOL_SOCKET, SO_DEVMEM_DONTNEED, &token,
                             sizeof(token));
        if (ret < 0) {
          perror("SO_DEVMEM_DONTNEED failed");
        }
      } else if (cmsg->cmsg_type == SCM_DEVMEM_LINEAR) {
        // リニアバッファに受信されたフラグメント
        linear_bytes += dmabuf_cmsg->frag_size;
        printf("Linear frag: size=%u\n", dmabuf_cmsg->frag_size);
      }
    }

    // 1秒ごとに進捗を表示
    static long long last_report = 0;
    long long current_time = get_time_us();
    if (current_time - last_report >= 1000000) {
      double elapsed = (current_time - start_time) / 1000000.0;
      double current_goodput = total_bytes / elapsed / 1024.0 / 1024.0 * 8.0;
      printf("Elapsed: %.1fs, Goodput: %.2f Mbps, Packets: %lld\n", elapsed,
             current_goodput, total_packets);
      last_report = current_time;
    }
  }

  end_time = get_time_us();

  // 結果の計算と表示
  double duration = (end_time - start_time) / 1000000.0;
  double goodput_mbps = total_bytes / duration / 1024.0 / 1024.0 * 8.0;
  double packet_rate = total_packets / duration;

  printf("\n=== Measurement Results ===\n");
  printf("Duration: %.3f seconds\n", duration);
  printf("Total bytes received: %lld bytes\n", total_bytes);
  printf("Total packets received: %lld packets\n", total_packets);
  printf("Device memory bytes: %lld bytes (%.1f%%)\n", devmem_bytes,
         devmem_bytes * 100.0 / total_bytes);
  printf("Linear buffer bytes: %lld bytes (%.1f%%)\n", linear_bytes,
         linear_bytes * 100.0 / total_bytes);
  printf("Goodput: %.2f Mbps\n", goodput_mbps);
  printf("Packet rate: %.2f packets/sec\n", packet_rate);
  printf("Average packet size: %.1f bytes\n",
         total_packets > 0 ? (double)total_bytes / total_packets : 0);

  // クリーンアップ
  close(client_fd);
  close(server_fd);

  return 0;
}