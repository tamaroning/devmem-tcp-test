#!/bin/bash

# Device Memory TCP セットアップスクリプト
# 使用方法: ./devmem_tcp_setup.sh <interface_name> <queue_number>

set -e

INTERFACE=${1:-eth1}
QUEUE_NUM=${2:-15}

echo "Setting up Device Memory TCP for interface: $INTERFACE, queue: $QUEUE_NUM"

# 必要なパッケージのチェック
check_dependencies() {
    echo "Checking dependencies..."
    
    # ethtoolの存在確認
    if ! command -v ethtool &> /dev/null; then
        echo "Error: ethtool is not installed"
        exit 1
    fi
    
    # カーネルバージョンの確認
    KERNEL_VERSION=$(uname -r)
    echo "Kernel version: $KERNEL_VERSION"
    
    # devmem TCPサポートの確認
    if [ ! -f /proc/net/devmem ]; then
        echo "Warning: /proc/net/devmem not found. devmem TCP may not be supported."
    fi
}

# NIC設定
setup_nic() {
    echo "Setting up NIC: $INTERFACE"
    
    # インターフェースの存在確認
    if ! ip link show $INTERFACE &> /dev/null; then
        echo "Error: Interface $INTERFACE does not exist"
        exit 1
    fi
    
    # インターフェースをアップ
    echo "Bringing up interface $INTERFACE..."
    ip link set $INTERFACE up
    
    # ヘッダー分割を有効化
    echo "Enabling header split..."
    ethtool -G $INTERFACE tcp-data-split on || {
        echo "Warning: Header split may not be supported on this NIC"
    }
    
    # フロー制御を有効化
    echo "Enabling flow steering..."
    ethtool -K $INTERFACE ntuple on || {
        echo "Warning: Flow steering may not be supported on this NIC"
    }
    
    # RSS設定
    echo "Configuring RSS to exclude queue $QUEUE_NUM..."
    ethtool --set-rxfh-indir $INTERFACE equal $QUEUE_NUM || {
        echo "Warning: RSS configuration failed"
    }
    
    # 現在の設定を表示
    echo "Current NIC configuration:"
    ethtool -g $INTERFACE 2>/dev/null || echo "Could not get ring parameters"
    ethtool -k $INTERFACE | grep -E "(tcp-data-split|ntuple)" 2>/dev/null || echo "Could not get feature information"
}

# dmabuf設定（テスト用）
setup_dmabuf() {
    echo "Setting up dmabuf..."
    
    # udmabufモジュールの確認
    if ! lsmod | grep -q udmabuf; then
        echo "Loading udmabuf module..."
        modprobe udmabuf 2>/dev/null || {
            echo "Warning: udmabuf module not available"
        }
    fi
    
    # /dev/udmabufの確認
    if [ -c /dev/udmabuf ]; then
        echo "udmabuf device found: /dev/udmabuf"
    else
        echo "Warning: /dev/udmabuf not found"
    fi
}

# ファイアウォール設定
setup_firewall() {
    echo "Configuring firewall..."
    
    # iptablesの確認
    if command -v iptables &> /dev/null; then
        # テストポート5201を開放
        iptables -I INPUT -p tcp --dport 5201 -j ACCEPT 2>/dev/null || {
            echo "Warning: Could not configure iptables"
        }
    fi
    
    # systemdファイアウォールの確認
    if command -v firewall-cmd &> /dev/null; then
        firewall-cmd --add-port=5201/tcp --permanent 2>/dev/null || {
            echo "Warning: Could not configure firewall-cmd"
        }
        firewall-cmd --reload 2>/dev/null || true
    fi
}

# システム設定の最適化
optimize_system() {
    echo "Optimizing system settings..."
    
    # TCP設定の最適化
    sysctl -w net.core.rmem_max=134217728 2>/dev/null || true
    sysctl -w net.core.wmem_max=134217728 2>/dev/null || true
    sysctl -w net.ipv4.tcp_rmem="4096 87380 134217728" 2>/dev/null || true
    sysctl -w net.ipv4.tcp_wmem="4096 65536 134217728" 2>/dev/null || true
    sysctl -w net.core.netdev_max_backlog=5000 2>/dev/null || true
    
    # CPU scaling governor設定
    if [ -d /sys/devices/system/cpu/cpu0/cpufreq ]; then
        echo "Setting CPU governor to performance..."
        echo performance | tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor 2>/dev/null || true
    fi
    
    # IRQ affinity設定
    echo "Setting IRQ affinity for $INTERFACE..."
    IRQ_NUM=$(grep $INTERFACE /proc/interrupts | awk -F: '{print $1}' | head -1 | tr -d ' ')
    if [ ! -z "$IRQ_NUM" ]; then
        echo "Setting IRQ $IRQ_NUM affinity to CPU 0"
        echo 1 > /proc/irq/$IRQ_NUM/smp_affinity 2>/dev/null || true
    fi
}

# フロー制御ルールの設定
setup_flow_steering() {
    echo "Setting up flow steering rules..."
    
    # 既存のルールをクリア
    ethtool -N $INTERFACE delete 0 2>/dev/null || true
    ethtool -N $INTERFACE delete 1 2>/dev/null || true
    
    # TCP フロー用のルール（ポート5201）
    echo "Adding flow steering rule for TCP port 5201 to queue $QUEUE_NUM..."
    ethtool -N $INTERFACE flow-type tcp4 dst-port 5201 action $QUEUE_NUM 2>/dev/null || {
        echo "Warning: Flow steering rule configuration failed"
    }
    
    # 現在のフロー制御ルールを表示
    echo "Current flow steering rules:"
    ethtool -n $INTERFACE 2>/dev/null || echo "Could not retrieve flow rules"
}

# 設定の確認
verify_setup() {
    echo "Verifying setup..."
    
    # インターフェース状態
    echo "Interface $INTERFACE status:"
    ip link show $INTERFACE
    
    # ethtool設定確認
    echo -e "\nNIC features:"
    ethtool -k $INTERFACE | grep -E "(tcp-data-split|ntuple|rx-checksum|tx-checksum)" 2>/dev/null || true
    
    # キューの確認
    echo -e "\nRX/TX queue configuration:"
    ethtool -l $INTERFACE 2>/dev/null || echo "Could not get queue information"
    
    # RSS設定確認
    echo -e "\nRSS configuration:"
    ethtool -x $INTERFACE 2>/dev/null || echo "Could not get RSS information"
    
    # フロー制御ルール確認
    echo -e "\nFlow steering rules:"
    ethtool -n $INTERFACE 2>/dev/null || echo "Could not get flow rules"
    
    # システム設定確認
    echo -e "\nSystem configuration:"
    echo "TCP rmem_max: $(sysctl net.core.rmem_max 2>/dev/null || echo 'N/A')"
    echo "TCP wmem_max: $(sysctl net.core.wmem_max 2>/dev/null || echo 'N/A')"
    echo "Netdev max backlog: $(sysctl net.core.netdev_max_backlog 2>/dev/null || echo 'N/A')"
}

# テスト用dmabuf作成プログラムのコンパイル
compile_test_programs() {
    echo "Compiling test programs..."
    
    # 必要なヘッダーファイルとライブラリの確認
    if ! gcc --version &> /dev/null; then
        echo "Warning: gcc not found. Test programs will not be compiled."
        return
    fi
    
    # libnlの確認
    if ! pkg-config --exists libnl-3.0 libnl-genl-3.0; then
        echo "Warning: libnl development packages not found"
        echo "Install with: apt-get install libnl-3-dev libnl-genl-3-dev"
    fi
    
    echo "Test programs should be compiled with:"
    echo "gcc -o devmem_server devmem_tcp_goodput_server.c -lpthread"
    echo "gcc -o devmem_client devmem_tcp_goodput_client.c -lpthread"
}

# メイン実行部分
main() {
    echo "=== Device Memory TCP Setup ==="
    echo "Interface: $INTERFACE"
    echo "Target queue: $QUEUE_NUM"
    echo "================================"
    
    # 権限チェック
    if [ "$EUID" -ne 0 ]; then
        echo "Warning: This script should be run as root for full functionality"
    fi
    
    check_dependencies
    setup_nic
    setup_dmabuf
    setup_firewall
    optimize_system
    setup_flow_steering
    verify_setup
    compile_test_programs
    
    echo -e "\n=== Setup Complete ==="
    echo "You can now run the test programs:"
    echo "Server: ./devmem_server [port] [duration_sec]"
    echo "Client: ./devmem_client [server_ip] [port] [data_size] [duration_sec] [use_devmem]"
    echo ""
    echo "Example usage:"
    echo "Server side: ./devmem_server 5201 30"
    echo "Client side: ./devmem_client 192.168.1.100 5201 1048576 30 0"
    echo ""
    echo "Note: devmem functionality requires proper dmabuf setup and kernel support"
}

# スクリプトの実行
main "$@"
