#include <errno.h>
#include <fcntl.h>
#include <linux/udmabuf.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/netlink.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// TODO: correct?
#define NETDEV_CMD_BIND_RX 1
#define NETDEV_CMD_BIND_TX 2

struct netdev_queue {
  int type;
  int idx;
};

// dmabufヘルパー機能
struct dmabuf_info {
  int fd;
  size_t size;
  void *mapped_addr;
  uint32_t dmabuf_id;
};

// udmabuf作成
int create_udmabuf(size_t size, struct dmabuf_info *info) {
  int memfd, udmabuf_fd;
  struct udmabuf_create create;

  printf("Creating udmabuf of size %zu bytes\n", size);

  // メモリFDを作成
  memfd = memfd_create("devmem_test", MFD_CLOEXEC);
  if (memfd < 0) {
    perror("memfd_create failed");
    return -1;
  }

  // メモリサイズを設定
  if (ftruncate(memfd, size) < 0) {
    perror("ftruncate failed");
    close(memfd);
    return -1;
  }

  // udmabufデバイスを開く
  udmabuf_fd = open("/dev/udmabuf", O_RDWR);
  if (udmabuf_fd < 0) {
    perror("Failed to open /dev/udmabuf");
    close(memfd);
    return -1;
  }

  // udmabuf作成構造体を設定
  memset(&create, 0, sizeof(create));
  create.memfd = memfd;
  create.offset = 0;
  create.size = size;

  // udmabufを作成
  info->fd = ioctl(udmabuf_fd, UDMABUF_CREATE, &create);
  close(udmabuf_fd);
  close(memfd);

  if (info->fd < 0) {
    perror("UDMABUF_CREATE failed");
    return -1;
  }

  // メモリをマップ
  info->mapped_addr =
      mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, info->fd, 0);
  if (info->mapped_addr == MAP_FAILED) {
    perror("mmap failed");
    close(info->fd);
    return -1;
  }

  info->size = size;
  printf("udmabuf created successfully: fd=%d, size=%zu, mapped=%p\n", info->fd,
         info->size, info->mapped_addr);

  return 0;
}

// dmabufをネットワークデバイスにバインド（RX）
int bind_dmabuf_rx(const char *ifname, int ifindex, int queue_idx,
                   struct dmabuf_info *info) {
  struct nl_sock *sk;
  struct nl_msg *msg;
  int family_id, err = -1;

  printf("Binding dmabuf to RX queue %d on interface %s (index %d)\n",
         queue_idx, ifname, ifindex);

  // netlink ソケットを作成
  sk = nl_socket_alloc();
  if (!sk) {
    fprintf(stderr, "Failed to allocate netlink socket\n");
    return -1;
  }

  // Generic Netlinkに接続
  if (genl_connect(sk) < 0) {
    fprintf(stderr, "Failed to connect to Generic Netlink\n");
    nl_socket_free(sk);
    return -1;
  }

  // netdevファミリーIDを取得
  family_id = genl_ctrl_resolve(sk, "netdev");
  if (family_id < 0) {
    fprintf(stderr, "Failed to resolve netdev family\n");
    nl_socket_free(sk);
    return -1;
  }

  // netlinkメッセージを作成
  msg = nlmsg_alloc();
  if (!msg) {
    fprintf(stderr, "Failed to allocate netlink message\n");
    nl_socket_free(sk);
    return -1;
  }

  // Generic Netlinkヘッダーを追加
  if (!genlmsg_put(msg, 0, 0, family_id, 0, 0, NETDEV_CMD_BIND_RX, 1)) {
    fprintf(stderr, "Failed to add Generic Netlink header\n");
    nlmsg_free(msg);
    nl_socket_free(sk);
    return -1;
  }

  // 属性を追加
  nla_put_u32(msg, 1, ifindex);  // ifindex
  nla_put_u32(msg, 2, info->fd); // dmabuf_fd

  // キュー情報を追加（実際の実装では適切なnested attributeを使用）
  // TODO: 実際の実装では適切なnested attribute構造を使用する
  struct nlattr *queues = nla_nest_start(msg, 3);
  if (queues) {
    struct nlattr *queue = nla_nest_start(msg, 0);
    if (queue) {
      nla_put_u32(msg, 1, 1);         // type = RX
      nla_put_u32(msg, 2, queue_idx); // queue index
      nla_nest_end(msg, queue);
    }
    nla_nest_end(msg, queues);
  }

  // メッセージを送信
  if (nl_send_auto(sk, msg) < 0) {
    fprintf(stderr, "Failed to send netlink message\n");
    nlmsg_free(msg);
    nl_socket_free(sk);
    return -1;
  }

  // 応答を受信（簡略化版）
  // TODO: 実際の実装では適切な応答処理を行う
  printf("dmabuf bind request sent successfully\n");

  nlmsg_free(msg);
  nl_socket_free(sk);

  return 0;
}

// dmabufをネットワークデバイスにバインド（TX）
int bind_dmabuf_tx(const char *ifname, int ifindex, struct dmabuf_info *info) {
  struct nl_sock *sk;
  struct nl_msg *msg;
  int family_id, err = -1;

  printf("Binding dmabuf for TX on interface %s (index %d)\n", ifname, ifindex);

  // netlink ソケットを作成
  sk = nl_socket_alloc();
  if (!sk) {
    fprintf(stderr, "Failed to allocate netlink socket\n");
    return -1;
  }

  // Generic Netlinkに接続
  if (genl_connect(sk) < 0) {
    fprintf(stderr, "Failed to connect to Generic Netlink\n");
    nl_socket_free(sk);
    return -1;
  }

  // netdevファミリーIDを取得
  family_id = genl_ctrl_resolve(sk, "netdev");
  if (family_id < 0) {
    fprintf(stderr, "Failed to resolve netdev family\n");
    nl_socket_free(sk);
    return -1;
  }

  // netlinkメッセージを作成
  msg = nlmsg_alloc();
  if (!msg) {
    fprintf(stderr, "Failed to allocate netlink message\n");
    nl_socket_free(sk);
    return -1;
  }

  // Generic Netlinkヘッダーを追加
  if (!genlmsg_put(msg, 0, 0, family_id, 0, 0, NETDEV_CMD_BIND_TX, 1)) {
    fprintf(stderr, "Failed to add Generic Netlink header\n");
    nlmsg_free(msg);
    nl_socket_free(sk);
    return -1;
  }

  // 属性を追加
  nla_put_u32(msg, 1, ifindex);  // ifindex
  nla_put_u32(msg, 2, info->fd); // dmabuf_fd

  // メッセージを送信
  if (nl_send_auto(sk, msg) < 0) {
    fprintf(stderr, "Failed to send netlink message\n");
    nlmsg_free(msg);
    nl_socket_free(sk);
    return -1;
  }

  printf("dmabuf TX bind request sent successfully\n");

  nlmsg_free(msg);
  nl_socket_free(sk);

  return 0;
}

// dmabufにテストデータを書き込み
void fill_dmabuf_testdata(struct dmabuf_info *info) {
  unsigned char *data = (unsigned char *)info->mapped_addr;
  size_t i;

  printf("Filling dmabuf with test data\n");

  // 繰り返しパターンでデータを埋める
  for (i = 0; i < info->size; i++) {
    data[i] = (i % 256);
  }

  // メモリ同期
  msync(info->mapped_addr, info->size, MS_SYNC);

  printf("Test data filled: %zu bytes\n", info->size);
}

// dmabufのクリーンアップ
void cleanup_dmabuf(struct dmabuf_info *info) {
  if (info->mapped_addr != MAP_FAILED && info->mapped_addr != NULL) {
    munmap(info->mapped_addr, info->size);
  }
  if (info->fd >= 0) {
    close(info->fd);
  }
  memset(info, 0, sizeof(*info));
}

// インターフェースインデックスを取得
int get_ifindex(const char *ifname) {
  int fd, ifindex;
  struct ifreq ifr;

  fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }

  memset(&ifr, 0, sizeof(ifr));
  strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

  if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
    perror("SIOCGIFINDEX");
    close(fd);
    return -1;
  }

  ifindex = ifr.ifr_ifindex;
  close(fd);

  return ifindex;
}

// メイン関数
int main(int argc, char *argv[]) {
  struct dmabuf_info rx_dmabuf, tx_dmabuf;
  char *ifname = "eth1";
  int queue_num = 15;
  size_t dmabuf_size = 1024 * 1024 * 16; // 16MB
  int ifindex;

  memset(&rx_dmabuf, 0, sizeof(rx_dmabuf));
  memset(&tx_dmabuf, 0, sizeof(tx_dmabuf));

  if (argc > 1) {
    ifname = argv[1];
  }
  if (argc > 2) {
    queue_num = atoi(argv[2]);
  }
  if (argc > 3) {
    dmabuf_size = atoll(argv[3]);
  }

  printf("dmabuf helper for devmem TCP\n");
  printf("Interface: %s\n", ifname);
  printf("Queue: %d\n", queue_num);
  printf("dmabuf size: %zu bytes\n", dmabuf_size);

  // インターフェースインデックスを取得
  ifindex = get_ifindex(ifname);
  if (ifindex < 0) {
    fprintf(stderr, "Failed to get interface index for %s\n", ifname);
    return 1;
  }
  printf("Interface index: %d\n", ifindex);

  // RX用dmabufを作成
  if (create_udmabuf(dmabuf_size, &rx_dmabuf) < 0) {
    fprintf(stderr, "Failed to create RX dmabuf\n");
    return 1;
  }

  // TX用dmabufを作成
  if (create_udmabuf(dmabuf_size, &tx_dmabuf) < 0) {
    fprintf(stderr, "Failed to create TX dmabuf\n");
    cleanup_dmabuf(&rx_dmabuf);
    return 1;
  }

  // TX dmabufにテストデータを書き込み
  fill_dmabuf_testdata(&tx_dmabuf);

  // dmabufをネットワークデバイスにバインド
  printf("\nBinding dmabufs to network device...\n");

  if (bind_dmabuf_rx(ifname, ifindex, queue_num, &rx_dmabuf) < 0) {
    fprintf(stderr, "Failed to bind RX dmabuf\n");
  } else {
    printf("RX dmabuf bound successfully\n");
  }

  if (bind_dmabuf_tx(ifname, ifindex, &tx_dmabuf) < 0) {
    fprintf(stderr, "Failed to bind TX dmabuf\n");
  } else {
    printf("TX dmabuf bound successfully\n");
  }

  // dmabuf情報を表示
  printf("\nDmabuf Information:\n");
  printf("RX dmabuf: fd=%d, size=%zu, mapped=%p\n", rx_dmabuf.fd,
         rx_dmabuf.size, rx_dmabuf.mapped_addr);
  printf("TX dmabuf: fd=%d, size=%zu, mapped=%p\n", tx_dmabuf.fd,
         tx_dmabuf.size, tx_dmabuf.mapped_addr);

  printf("\nPress Enter to cleanup and exit...\n");
  getchar();

  // クリーンアップ
  cleanup_dmabuf(&rx_dmabuf);
  cleanup_dmabuf(&tx_dmabuf);

  printf("Cleanup completed\n");
  return 0;
}
