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
// Created: 2026/10/03

package egress

import (
	"bufio"
	"encoding/binary"
	"io"
	"net"
	"testing"
	"time"
)

// socksGreeting performs the method negotiation and asserts no-auth was
// accepted.
func socksGreeting(t *testing.T, conn net.Conn, br *bufio.Reader) {
	t.Helper()
	if _, err := conn.Write([]byte{socks5Version, 0x01, socksAuthNone}); err != nil {
		t.Fatalf("greeting write: %v", err)
	}
	reply := make([]byte, 2)
	if _, err := io.ReadFull(br, reply); err != nil {
		t.Fatalf("greeting reply: %v", err)
	}
	if reply[0] != socks5Version || reply[1] != socksAuthNone {
		t.Fatalf("greeting reply = %v, want [5 0]", reply)
	}
}

// socksConnectDomain sends a CONNECT request for a domain target.
func socksConnectDomain(t *testing.T, conn net.Conn, host string, port int) {
	t.Helper()
	req := []byte{socks5Version, socksCmdConnect, 0x00, socksAtypDomain, byte(len(host))}
	req = append(req, host...)
	var portRaw [2]byte
	binary.BigEndian.PutUint16(portRaw[:], uint16(port))
	req = append(req, portRaw[:]...)
	if _, err := conn.Write(req); err != nil {
		t.Fatalf("connect write: %v", err)
	}
}

// socksReply reads the 10-byte reply and returns REP.
func socksReply(t *testing.T, br *bufio.Reader) byte {
	t.Helper()
	reply := make([]byte, 10)
	if _, err := io.ReadFull(br, reply); err != nil {
		t.Fatalf("connect reply: %v", err)
	}
	if reply[0] != socks5Version {
		t.Fatalf("reply version = %d", reply[0])
	}
	return reply[1]
}

func TestSOCKSConnectEcho(t *testing.T) {
	echoPort := startEcho(t)
	server, logger := newTestServer(t, serverOpts{
		// The main policy denies everything; SOCKS must consult its own
		// (conservative) policy instead.
		policy:      mapPolicy{"example.test": {Allow: false, Matched: true, Reason: "deny", Rule: "builtin"}},
		socksPolicy: mapPolicy{"example.test": allowExempt()},
	})
	conn := dialProxy(t, server)
	br := bufio.NewReader(conn)
	_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	socksGreeting(t, conn, br)
	socksConnectDomain(t, conn, "example.test", echoPort)
	if rep := socksReply(t, br); rep != socksRepSucceeded {
		t.Fatalf("rep = %d, want success", rep)
	}
	if _, err := conn.Write([]byte("ping")); err != nil {
		t.Fatalf("write payload: %v", err)
	}
	buf := make([]byte, 4)
	if _, err := io.ReadFull(br, buf); err != nil || string(buf) != "ping" {
		t.Fatalf("echo = %q, %v", buf, err)
	}
	if rec := logger.last(); rec.Proto != "socks5" || !rec.Decision.Allow {
		t.Fatalf("record = %+v", rec)
	}
}

// TestSOCKSUsesOwnPolicy: a denial in the SOCKS policy is honored even
// when the HTTP policy would allow.
func TestSOCKSUsesOwnPolicy(t *testing.T) {
	echoPort := startEcho(t)
	server, _ := newTestServer(t, serverOpts{
		policy:      mapPolicy{"example.test": allowExempt()},
		socksPolicy: mapPolicy{"example.test": {Allow: false, Matched: true, Reason: "no socks", Rule: "builtin"}},
	})
	conn := dialProxy(t, server)
	br := bufio.NewReader(conn)
	_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	socksGreeting(t, conn, br)
	socksConnectDomain(t, conn, "example.test", echoPort)
	if rep := socksReply(t, br); rep != socksRepNotAllowed {
		t.Fatalf("rep = %d, want not-allowed", rep)
	}
}

// TestSOCKSFragmentedWrites: greeting and request may arrive byte by
// byte; the state machine must tolerate it.
func TestSOCKSFragmentedWrites(t *testing.T) {
	echoPort := startEcho(t)
	server, _ := newTestServer(t, serverOpts{
		socksPolicy: mapPolicy{"example.test": allowExempt()},
		handshake:   3 * time.Second,
	})
	conn := dialProxy(t, server)
	br := bufio.NewReader(conn)
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	drip := func(b []byte) {
		t.Helper()
		for _, one := range b {
			if _, err := conn.Write([]byte{one}); err != nil {
				t.Fatalf("drip write: %v", err)
			}
			time.Sleep(2 * time.Millisecond)
		}
	}
	drip([]byte{socks5Version, 0x01, socksAuthNone})
	reply := make([]byte, 2)
	if _, err := io.ReadFull(br, reply); err != nil || reply[1] != socksAuthNone {
		t.Fatalf("greeting reply = %v, %v", reply, err)
	}
	req := []byte{socks5Version, socksCmdConnect, 0x00, socksAtypDomain, byte(len("example.test"))}
	req = append(req, "example.test"...)
	var portRaw [2]byte
	binary.BigEndian.PutUint16(portRaw[:], uint16(echoPort))
	req = append(req, portRaw[:]...)
	drip(req)
	if rep := socksReply(t, br); rep != socksRepSucceeded {
		t.Fatalf("rep = %d, want success", rep)
	}
}

func TestSOCKSNoCommonMethod(t *testing.T) {
	server, _ := newTestServer(t, serverOpts{})
	conn := dialProxy(t, server)
	br := bufio.NewReader(conn)
	_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	// Offer only username/password (0x02), which we do not accept.
	if _, err := conn.Write([]byte{socks5Version, 0x01, 0x02}); err != nil {
		t.Fatalf("write: %v", err)
	}
	reply := make([]byte, 2)
	if _, err := io.ReadFull(br, reply); err != nil {
		t.Fatalf("reply: %v", err)
	}
	if reply[1] != socksAuthNoAccepted {
		t.Fatalf("reply = %v, want [5 0xff]", reply)
	}
}

// TestSOCKSDomainControlChar: a DOMAINNAME carrying CRLF must never
// reach the policy/log pipeline.
func TestSOCKSDomainControlChar(t *testing.T) {
	echoPort := startEcho(t)
	server, logger := newTestServer(t, serverOpts{socksPolicy: mapPolicy{}})
	conn := dialProxy(t, server)
	br := bufio.NewReader(conn)
	_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	socksGreeting(t, conn, br)
	socksConnectDomain(t, conn, "evil\r\nx", echoPort)
	// The connection is dropped without a success reply.
	_, err := br.ReadByte()
	if err == nil {
		t.Fatal("control-char domain got a reply")
	}
	if rec := logger.last(); rec.Host != "" {
		t.Fatalf("control-char domain reached the logger: %+v", rec)
	}
}

// TestSOCKSIPv4Target: literal IPv4 targets work (exempt loopback for
// the hermetic echo upstream).
func TestSOCKSIPv4Target(t *testing.T) {
	echoPort := startEcho(t)
	server, _ := newTestServer(t, serverOpts{
		socksPolicy: mapPolicy{"127.0.0.1": allowExempt()},
	})
	conn := dialProxy(t, server)
	br := bufio.NewReader(conn)
	_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	socksGreeting(t, conn, br)
	req := []byte{socks5Version, socksCmdConnect, 0x00, socksAtypIPv4, 127, 0, 0, 1, 0, 0}
	binary.BigEndian.PutUint16(req[len(req)-2:], uint16(echoPort))
	if _, err := conn.Write(req); err != nil {
		t.Fatalf("write: %v", err)
	}
	if rep := socksReply(t, br); rep != socksRepSucceeded {
		t.Fatalf("rep = %d, want success", rep)
	}
}
