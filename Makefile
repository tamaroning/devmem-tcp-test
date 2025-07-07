# Device Memory TCP グッドプット測定プログラム Makefile

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=gnu99
LIBS = -lpthread

# パッケージ設定
PKG_CONFIG = pkg-config
LIBNL_CFLAGS = $(shell $(PKG_CONFIG) --cflags libnl-3.0 libnl-genl-3.0 2>/dev/null)
LIBNL_LIBS = $(shell $(PKG_CONFIG) --libs libnl-3.0 libnl-genl-3.0 2>/dev/null)

# プログラム名
SERVER = devmem_server
CLIENT = devmem_client
DMABUF_HELPER = dmabuf_helper

# ソースファイル
SERVER_SRC = devmem_tcp_goodput_server.c
CLIENT_SRC = devmem_tcp_goodput_client.c
DMABUF_HELPER_SRC = dmabuf_helper.c

# オブジェクトファイル
SERVER_OBJ = $(SERVER_SRC:.c=.o)
CLIENT_OBJ = $(CLIENT_SRC:.c=.o)
DMABUF_HELPER_OBJ = $(DMABUF_HELPER_SRC:.c=.o)

# デフォルトターゲット
all: $(SERVER) $(CLIENT) $(DMABUF_HELPER)

# サーバープログラム
$(SERVER): $(SERVER_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# クライアントプログラム
$(CLIENT): $(CLIENT_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# dmabufヘルパープログラム
$(DMABUF_HELPER): $(DMABUF_HELPER_OBJ)
	$(CC) $(CFLAGS) $(LIBNL_CFLAGS) -o $@ $^ $(LIBS) $(LIBNL_LIBS)

# オブジェクトファイルの生成規則
%.o: %.c
	$(CC) $(CFLAGS) $(LIBNL_CFLAGS) -c $< -o $@

# インストール
install: all
	install -d $(DESTDIR)/usr/local/bin
	install -m 755 $(SERVER) $(CLIENT) $(DMABUF_HELPER) $(DESTDIR)/usr/local/bin/
	install -m 755 devmem_tcp_setup.sh $(DESTDIR)/usr/local/bin/

# クリーンアップ
clean:
	rm -f $(SERVER) $(CLIENT) $(DMABUF_HELPER)
	rm -f *.o
	rm -f core

# テスト実行
test: all
	@echo "Running basic connectivity test..."
	@echo "Starting server in background..."
	./$(SERVER) 5201 5 &
	@sleep 1
	@echo "Starting client..."
	./$(CLIENT) 127.0.0.1 5201 65536 3 0
	@echo "Test completed"

# devmem特化テスト（実際のdevmem環境が必要）
test-devmem: all
	@echo "Running devmem test (requires proper devmem setup)..."
	@echo "Starting server in background..."
	./$(SERVER) 5201 10 &
	@sleep 1
	@echo "Starting devmem client..."
	./$(CLIENT) 127.0.0.1 5201 1048576 8 1
	@echo "devmem test completed"

# システムセットアップ
setup:
	@echo "Setting up system for devmem TCP..."
	sudo ./devmem_tcp_setup.sh eth1 15

# ベンチマークスイート
benchmark: all
	@echo "Running benchmark suite..."
	@mkdir -p results
	@echo "Testing different packet sizes..."
	@for size in 1024 4096 16384 65536 262144 1048576; do \
		echo "Testing packet size: $$size bytes"; \
		./$(SERVER) 5201 10 > results/server_$$size.log 2>&1 & \
		sleep 1; \
		./$(CLIENT) 127.0.0.1 5201 $$size 8 0 > results/client_$$size.log 2>&1; \
		sleep 1; \
	done
	@echo "Benchmark completed. Results in results/ directory"

# パフォーマンス分析
profile: all
	@echo "Running performance analysis..."
	perf record -g ./$(SERVER) 5201 10 &
	sleep 1
	perf record -g ./$(CLIENT) 127.0.0.1 5201 65536 8 0
	perf report

# カーネルモジュール確認
check-kernel:
	@echo "Checking kernel support..."
	@echo "Kernel version: $(shell uname -r)"
	@echo "devmem TCP support:"
	@ls /proc/net/devmem 2>/dev/null && echo "devmem TCP supported" || echo "devmem TCP not found"
	@echo "udmabuf module:"
	@lsmod | grep udmabuf || echo "udmabuf module not loaded"
	@echo "Network interfaces:"
	@ip link show | grep -E "^[0-9]+:"

# 依存関係確認
check-deps:
	@echo "Checking dependencies..."
	@echo "GCC: $(shell gcc --version | head -1 2>/dev/null || echo 'Not found')"
	@echo "libnl-3.0: $(shell $(PKG_CONFIG) --modversion libnl-3.0 2>/dev/null || echo 'Not found')"
	@echo "libnl-genl-3.0: $(shell $(PKG_CONFIG) --modversion libnl-genl-3.0 2>/dev/null || echo 'Not found')"
	@echo "ethtool: $(shell ethtool --version 2>/dev/null || echo 'Not found')"
	@echo "iperf3: $(shell iperf3 --version 2>/dev/null | head -1 || echo 'Not found (optional)')"

# ヘルプ
help:
	@echo "Device Memory TCP グッドプット測定プログラム"
	@echo ""
	@echo "利用可能なターゲット:"
	@echo "  all          - 全プログラムをビルド"
	@echo "  clean        - ビルド成果物を削除"
	@echo "  install      - プログラムをインストール"
	@echo "  test         - 基本的な接続テストを実行"
	@echo "  test-devmem  - devmemテストを実行（適切なセットアップが必要）"
	@echo "  setup        - システムをdevmem TCP用にセットアップ"
	@echo "  benchmark    - ベンチマークスイートを実行"
	@echo "  profile      - パフォーマンス分析を実行"
	@echo "  check-kernel - カーネルサポートを確認"
	@echo "  check-deps   - 依存関係を確認"
	@echo "  help         - このヘルプを表示"
	@echo ""
	@echo "使用例:"
	@echo "  make all           # プログラムをビルド"
	@echo "  make setup         # システムセットアップ"
	@echo "  make test          # 基本テスト実行"
	@echo "  make benchmark     # ベンチマーク実行"

.PHONY: all clean install test test-devmem setup benchmark profile check-kernel check-deps help