// Copyright (c) 2026 The Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Authors: liubang (it.liubang@gmail.com)
// Created: 2026/06/21

package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"os"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	pb "github.com/liubang/playground/go/pl/grpc/echo/pb"
)

type echoClient struct {
	conn *grpc.ClientConn
	stub pb.EchoServiceClient
}

func newEchoClient(addr string) (*echoClient, error) {
	conn, err := grpc.NewClient(addr, grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, err
	}
	return &echoClient{conn: conn, stub: pb.NewEchoServiceClient(conn)}, nil
}

func (c *echoClient) close() { c.conn.Close() }

func (c *echoClient) doEcho(message string) {
	now := time.Now().UnixMicro()
	req := &pb.EchoRequest{
		Message:     message,
		TimestampUs: now,
		Headers:     map[string]string{"client": "go"},
	}
	resp, err := c.stub.Echo(context.Background(), req)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[Echo] RPC failed: %v\n", err)
		return
	}
	fmt.Printf("[Echo] response: %s | rtt_us=%d | server=%s\n",
		resp.Message, resp.ServerTimestamp-now, resp.ServerId)
}

func (c *echoClient) doServerStream(pattern string, maxResponses int32) {
	stream, err := c.stub.ServerStream(context.Background(), &pb.ServerStreamRequest{
		Pattern:      pattern,
		MaxResponses: maxResponses,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "[ServerStream] RPC failed: %v\n", err)
		return
	}
	fmt.Printf("[ServerStream] receiving items (pattern='%s', limit=%d):\n", pattern, maxResponses)
	for {
		item, err := stream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			fmt.Fprintf(os.Stderr, "[ServerStream] RPC failed: %v\n", err)
			return
		}
		fmt.Printf("  [%d] %s\n", item.Index, item.Content)
	}
}

func (c *echoClient) doClientStream(messages []string) {
	stream, err := c.stub.ClientStream(context.Background())
	if err != nil {
		fmt.Fprintf(os.Stderr, "[ClientStream] RPC failed: %v\n", err)
		return
	}
	for _, msg := range messages {
		if err := stream.Send(&pb.EchoRequest{Message: msg}); err != nil {
			fmt.Fprintf(os.Stderr, "[ClientStream] send failed: %v\n", err)
			return
		}
	}
	summary, err := stream.CloseAndRecv()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[ClientStream] RPC failed: %v\n", err)
		return
	}
	fmt.Printf("[ClientStream] summary: count=%d | server=%s\n", summary.MessageCount, summary.ServerId)
	for i, m := range summary.Messages {
		fmt.Printf("  [%d] %s\n", i, m)
	}
}

func (c *echoClient) doChat(messages []string) {
	stream, err := c.stub.Chat(context.Background())
	if err != nil {
		fmt.Fprintf(os.Stderr, "[Chat] RPC failed: %v\n", err)
		return
	}
	// Start receiver goroutine
	errCh := make(chan error, 1)
	go func() {
		for {
			msg, err := stream.Recv()
			if err == io.EOF {
				errCh <- nil
				return
			}
			if err != nil {
				errCh <- err
				return
			}
			fmt.Printf("  %s → %s\n", msg.Sender, msg.Content)
		}
	}()
	// Send messages
	fmt.Println("[Chat] round-trip:")
	for _, content := range messages {
		msg := &pb.ChatMessage{Sender: "go-client", Content: content, TimestampUs: time.Now().UnixMicro()}
		if err := stream.Send(msg); err != nil {
			fmt.Fprintf(os.Stderr, "[Chat] send failed: %v\n", err)
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	stream.CloseSend()
	if err := <-errCh; err != nil {
		fmt.Fprintf(os.Stderr, "[Chat] RPC failed: %v\n", err)
	}
}

func (c *echoClient) doHealthCheck() {
	resp, err := c.stub.HealthCheck(context.Background(), &pb.HealthRequest{})
	if err != nil {
		fmt.Fprintf(os.Stderr, "[HealthCheck] RPC failed: %v\n", err)
		return
	}
	fmt.Printf("[HealthCheck] status=%s | server=%s | version=%s | uptime=%ds\n",
		resp.Status, resp.ServerId, resp.Version, resp.UptimeSeconds)
}

func main() {
	addr := "localhost:50051"
	flag.Parse()
	if len(flag.Args()) > 0 {
		addr = flag.Arg(0)
	}
	client, err := newEchoClient(addr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "client error: %v\n", err)
		os.Exit(1)
	}
	defer client.close()

	fmt.Println("============ HealthCheck ============")
	client.doHealthCheck()
	fmt.Println("============ Unary Echo =============")
	client.doEcho("Hello from Go client!")
	fmt.Println("============ Server Stream ==========")
	client.doServerStream("[aeiou].*", 5)
	fmt.Println("============ Client Stream ==========")
	client.doClientStream([]string{"msg-1", "msg-2", "msg-3"})
	fmt.Println("============ Bidi Chat ==============")
	client.doChat([]string{"First message", "Second message", "Third message", "Fourth message"})
}
