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
	"net"
	"os"
	"regexp"
	"time"

	"google.golang.org/grpc"

	pb "github.com/liubang/playground/go/pl/grpc/echo/pb"
)

var (
	port     = flag.Int("port", 50051, "Server port to listen on")
	serverID = flag.String("id", "go-server", "Unique server identifier")
)

var items = []string{
	"Alpha", "Bravo", "Charlie", "Delta", "Echo",
	"Foxtrot", "Golf", "Hotel", "India", "Juliet",
}

type echoServer struct {
	pb.UnimplementedEchoServiceServer
	serverID  string
	startTime time.Time
}

func (s *echoServer) Echo(_ context.Context, req *pb.EchoRequest) (*pb.EchoResponse, error) {
	now := time.Now().UnixMicro()
	return &pb.EchoResponse{
		Message:           req.Message,
		OriginalTimestamp: req.TimestampUs,
		ServerTimestamp:   now,
		ServerId:          s.serverID,
	}, nil
}

func (s *echoServer) HealthCheck(_ context.Context, _ *pb.HealthRequest) (*pb.HealthResponse, error) {
	return &pb.HealthResponse{
		Status:        pb.HealthResponse_SERVING,
		ServerId:      s.serverID,
		Version:       "1.0.0",
		UptimeSeconds: int64(time.Since(s.startTime).Seconds()),
	}, nil
}

// --- StreamService ---

type streamServer struct {
	pb.UnimplementedStreamServiceServer
	serverID string
}

func (s *streamServer) ServerStream(req *pb.ServerStreamRequest, stream grpc.ServerStreamingServer[pb.StreamItem]) error {
	var pattern *regexp.Regexp
	if req.Pattern != "" {
		var err error
		pattern, err = regexp.Compile("(?i)" + req.Pattern)
		if err != nil {
			return fmt.Errorf("invalid regex: %w", err)
		}
	}
	limit := int(req.MaxResponses)
	if limit <= 0 {
		limit = len(items)
	}
	count := 0
	for i, content := range items {
		if count >= limit {
			break
		}
		if pattern != nil && !pattern.MatchString(content) {
			continue
		}
		if err := stream.Send(&pb.StreamItem{Index: int32(i), Content: content}); err != nil {
			return err
		}
		count++
	}
	return nil
}

func (s *streamServer) ClientStream(stream grpc.ClientStreamingServer[pb.EchoRequest, pb.EchoSummary]) error {
	var messages []string
	for {
		req, err := stream.Recv()
		if err == io.EOF {
			now := time.Now().UnixMicro()
			return stream.SendAndClose(&pb.EchoSummary{
				MessageCount:    int32(len(messages)),
				Messages:        messages,
				ServerTimestamp: now,
				ServerId:        s.serverID,
			})
		}
		if err != nil {
			return err
		}
		messages = append(messages, req.Message)
	}
}

func (s *streamServer) Chat(stream grpc.BidiStreamingServer[pb.ChatMessage, pb.ChatMessage]) error {
	for {
		msg, err := stream.Recv()
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return err
		}
		if err := stream.Send(msg); err != nil {
			return err
		}
	}
}

func runServer(port int, serverID string) error {
	lis, err := net.Listen("tcp", fmt.Sprintf(":%d", port))
	if err != nil {
		return err
	}
	s := grpc.NewServer()
	pb.RegisterEchoServiceServer(s, &echoServer{serverID: serverID, startTime: time.Now()})
	pb.RegisterStreamServiceServer(s, &streamServer{serverID: serverID})
	fmt.Printf("[Go EchoServer] Listening on port %d (id: %s)\n", port, serverID)
	return s.Serve(lis)
}

func main() {
	flag.Parse()
	if err := runServer(*port, *serverID); err != nil {
		fmt.Fprintf(os.Stderr, "server error: %v\n", err)
		os.Exit(1)
	}
}
