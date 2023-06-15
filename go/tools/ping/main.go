package main

import (
	"fmt"
	"net"
	"time"
)

// 计算校验和
func checkSum(msg []byte) uint16 {
	sum := 0
	length := len(msg)
	for i := 0; i < length-1; i += 2 {
		sum += int(msg[i])*256 + int(msg[i+1])
	}
	if length%2 == 1 {
		sum += int(msg[length-1]) * 256
	}
	sum = (sum >> 16) + (sum & 0xffff)
	sum += sum >> 16
	return uint16(^sum)
}

// this method should use root user to run.
func main() {
	host := "github.com" // 要ping的主机地址

	// 创建一个icmpConn对象
	icmpConn, err := net.Dial("ip4:icmp", host)
	if err != nil {
		fmt.Println("无法连接到主机：", err)
		return
	}
	defer icmpConn.Close()

	// 创建ICMP消息
	msg := make([]byte, 64)
	msg[0] = 8     // ICMP Echo Request类型
	msg[1] = 0     // ICMP Echo Request代码
	msg[2] = 0     // 校验和高位字节
	msg[3] = 0     // 校验和低位字节
	msg[4] = 0     // 标识符高位字节
	msg[5] = 0     // 标识符低位字节
	msg[6] = 0     // 序列号高位字节
	msg[7] = 0     // 序列号低位字节
	msg[8] = 0x3b  // 数据开始位置的ASCII码字符 ';'
	msg[9] = 0x7b  // 数据开始位置的ASCII码字符 '{'
	msg[10] = 0x7b // 数据开始位置的ASCII码字符 '{'
	msg[11] = 0x7b // 数据开始位置的ASCII码字符 '{'

	// 计算校验和
	checkSum := checkSum(msg)
	msg[2] = byte(checkSum >> 8)   // 设置校验和高位字节
	msg[3] = byte(checkSum & 0xff) // 设置校验和低位字节

	// 发送ICMP消息并记录发送时间
	start := time.Now()
	_, err = icmpConn.Write(msg)
	if err != nil {
		fmt.Println("发送ICMP消息失败：", err)
		return
	}

	// 设置读取超时时间
	icmpConn.SetReadDeadline(time.Now().Add(3 * time.Second))

	// 接收ICMP回复消息
	reply := make([]byte, 1024)
	_, err = icmpConn.Read(reply)
	if err != nil {
		fmt.Println("接收ICMP回复消息失败：", err)
		return
	}

	// 计算往返时间并输出结果
	duration := time.Since(start)
	fmt.Println("从", host, "的回复:", reply[20:])
	fmt.Println("往返时间:", duration)
}
