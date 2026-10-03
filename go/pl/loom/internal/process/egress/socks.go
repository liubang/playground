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
	"errors"
	"io"
	"net"
	"net/netip"
)

const (
	socks5Version = 0x05

	socksAuthNone       = 0x00
	socksAuthNoAccepted = 0xff

	socksCmdConnect = 0x01

	socksAtypIPv4   = 0x01
	socksAtypDomain = 0x03
	socksAtypIPv6   = 0x04

	socksRepSucceeded        = 0x00
	socksRepFailure          = 0x01
	socksRepNotAllowed       = 0x02
	socksRepCmdNotSupported  = 0x07
	socksRepAtypNotSupported = 0x08
)

// socksProtoError carries the RFC 1928 reply code a malformed or
// unsupported request should be answered with.
type socksProtoError struct {
	rep byte
	err error
}

func (e *socksProtoError) Error() string { return e.err.Error() }

// serveSOCKS implements the SOCKS5 leg (RFC 1928): no-auth greeting,
// CONNECT only. Every read tolerates arbitrarily fragmented writes — a
// sandboxed process may drip bytes to probe the state machine, and the
// handshake deadline armed in serve() is what bounds that.
func (s *Server) serveSOCKS(conn net.Conn, br *bufio.Reader, capped *cappedReader) {
	methods, err := readSocksGreeting(br)
	if err != nil {
		return
	}
	accepted := byte(socksAuthNoAccepted)
	for _, m := range methods {
		if m == socksAuthNone {
			accepted = socksAuthNone
			break
		}
	}
	if _, err := conn.Write([]byte{socks5Version, accepted}); err != nil {
		return
	}
	if accepted == socksAuthNoAccepted {
		return
	}
	host, port, err := readSocksConnect(br)
	if err != nil {
		var protoErr *socksProtoError
		if errors.As(err, &protoErr) {
			_ = writeSOCKSReply(conn, protoErr.rep)
		}
		return
	}
	s.decideAndTunnel(conn, br, capped, "socks5", host, port)
}

// readSocksGreeting consumes VER NMETHODS METHODS... and returns the
// offered methods.
func readSocksGreeting(br *bufio.Reader) ([]byte, error) {
	head := make([]byte, 2)
	if _, err := io.ReadFull(br, head); err != nil {
		return nil, err
	}
	if head[0] != socks5Version {
		return nil, errBadAuthority
	}
	methods := make([]byte, int(head[1]))
	if _, err := io.ReadFull(br, methods); err != nil {
		return nil, err
	}
	return methods, nil
}

// readSocksConnect consumes VER CMD RSV ATYP ADDR PORT and returns the
// normalized target. Only CONNECT is supported; DOMAINNAME targets are
// validated against control characters (the field is a length-prefixed
// raw byte string with zero protocol-level validation).
func readSocksConnect(br *bufio.Reader) (string, int, error) {
	head := make([]byte, 4)
	if _, err := io.ReadFull(br, head); err != nil {
		return "", 0, err
	}
	if head[0] != socks5Version {
		return "", 0, errBadAuthority
	}
	if head[1] != socksCmdConnect {
		return "", 0, &socksProtoError{rep: socksRepCmdNotSupported, err: errBadAuthority}
	}
	var host string
	switch head[3] {
	case socksAtypIPv4:
		raw := make([]byte, net.IPv4len)
		if _, err := io.ReadFull(br, raw); err != nil {
			return "", 0, err
		}
		host = netip.AddrFrom4([4]byte(raw)).String()
	case socksAtypIPv6:
		raw := make([]byte, net.IPv6len)
		if _, err := io.ReadFull(br, raw); err != nil {
			return "", 0, err
		}
		host = netip.AddrFrom16([16]byte(raw)).String()
	case socksAtypDomain:
		length, err := br.ReadByte()
		if err != nil {
			return "", 0, err
		}
		raw := make([]byte, int(length))
		if _, err := io.ReadFull(br, raw); err != nil {
			return "", 0, err
		}
		host = string(raw)
	default:
		return "", 0, &socksProtoError{rep: socksRepAtypNotSupported, err: errBadAuthority}
	}
	portRaw := make([]byte, 2)
	if _, err := io.ReadFull(br, portRaw); err != nil {
		return "", 0, err
	}
	port := int(binary.BigEndian.Uint16(portRaw))
	host = normalizeHost(host)
	if err := validateHost(host); err != nil {
		return "", 0, err
	}
	if port < 1 {
		return "", 0, errBadAuthority
	}
	return host, port, nil
}

// writeSOCKSReply emits VER REP RSV ATYP(IPv4) 0.0.0.0:0.
func writeSOCKSReply(w io.Writer, rep byte) error {
	_, err := w.Write([]byte{socks5Version, rep, 0x00, socksAtypIPv4, 0, 0, 0, 0, 0, 0})
	return err
}
