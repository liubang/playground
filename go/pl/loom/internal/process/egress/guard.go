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
	"context"
	"errors"
	"fmt"
	"net"
	"net/netip"
	"strings"
	"sync"
	"time"
)

// addressGuard implements the resolved-address guard: the domain policy
// decides by name, but whoever controls a permitted name's DNS decides
// what it resolves to — so every resolved address is checked against
// the denied classes below before the proxy dials it. Exemption is only
// ever earned by an explicit allow rule's literal (Decision.ExemptLiterals);
// decoding an embedded IPv4 form only ever ADDS denials, never one.
type addressGuard struct {
	lookupIP   func(ctx context.Context, host string) ([]netip.Addr, error)
	dial       func(ctx context.Context, addr netip.AddrPort) (net.Conn, error)
	localAddrs func() ([]netip.Addr, error)
	selfPort   int

	localMu     sync.Mutex
	localCached []netip.Addr
	localExpiry time.Time
}

// localAddrsTTL bounds how long a host-address snapshot is reused.
// InterfaceAddrs costs ~45µs (a getifaddrs probe) and resolve() would
// pay it on EVERY connection; interface changes (VPN up/down, DHCP
// renewals) are far slower than this TTL. The denied classes are
// re-checked per connection regardless — only a host address ADDED
// within the window can briefly slip the "host's own addresses" class.
const localAddrsTTL = 5 * time.Second

// local returns this host's interface addresses, cached for
// localAddrsTTL. A probe failure reuses the previous snapshot (possibly
// nil) instead of failing the connection — the same posture the callers
// had when they ignored the error.
func (g *addressGuard) local() []netip.Addr {
	g.localMu.Lock()
	defer g.localMu.Unlock()
	if time.Now().Before(g.localExpiry) {
		return g.localCached
	}
	if addrs, err := g.localAddrs(); err == nil {
		g.localCached = addrs
	}
	g.localExpiry = time.Now().Add(localAddrsTTL)
	return g.localCached
}

// deniedClass is a named set of prefixes a resolved address must not
// fall into. The name is the only detail a refusal reports — the
// sandboxed client must not learn internal topology from a denial.
type deniedClass struct {
	reason   string
	prefixes []netip.Prefix
}

// cloudMetadata are instance-metadata / platform endpoints outside
// link-local space (169.254.0.0/16 already covers the common ones).
var cloudMetadata = []string{
	"100.100.100.200",    // Alibaba Cloud
	"168.63.129.16",      // Azure WireServer / host agent
	"192.0.0.192",        // Oracle Cloud Infrastructure Classic
	"fd00:ec2::/32",      // AWS IPv6 service block (IMDS, EKS Pod Identity, DNS, NTP)
	"fd20:ce::254",       // Google Cloud IPv6-only instances
	"fd00:c1::a9fe:a9fe", // Oracle Cloud Infrastructure IPv6
	"fd00:42::42",        // Scaleway IPv6
	"fd00:a9fe:a9fe::1",  // Akamai / Linode IPv6
	"fd00:100::100:200",  // Alibaba Cloud IPv6
}

var deniedClasses = []deniedClass{
	{"a loopback address", mustPrefixes("127.0.0.0/8", "::1/128")},
	{"an unspecified address", mustPrefixes("0.0.0.0/8", "::/128")},
	{"a link-local address", mustPrefixes("169.254.0.0/16", "fe80::/10")},
	{"a multicast address", mustPrefixes("224.0.0.0/4", "ff00::/8")},
	{"the broadcast address", mustPrefixes("255.255.255.255/32")},
	{"a cloud metadata address", mustPrefixes(cloudMetadata...)},
}

// errTargetIsProxy refuses connections aimed at the proxy's own
// listener: an exempt loopback literal must never let the proxy
// connect to itself and build a recursive tunnel.
var errTargetIsProxy = errors.New("target is the egress proxy itself")

// guardDeniedError marks refusals by the guard (403 to the client),
// distinguished from resolution/dial failures (502).
type guardDeniedError struct {
	host   string
	reason string
}

func (e *guardDeniedError) Error() string {
	return fmt.Sprintf("connection to %s blocked: resolved to %s", e.host, e.reason)
}

func newAddressGuard(cfg Config) *addressGuard {
	g := &addressGuard{
		lookupIP:   cfg.LookupIP,
		dial:       cfg.Dial,
		localAddrs: cfg.LocalAddrs,
	}
	if g.lookupIP == nil {
		g.lookupIP = defaultLookupIP
	}
	if g.dial == nil {
		g.dial = defaultDial
	}
	if g.localAddrs == nil {
		g.localAddrs = defaultLocalAddrs
	}
	return g
}

func defaultLookupIP(ctx context.Context, host string) ([]netip.Addr, error) {
	ips, err := net.DefaultResolver.LookupIP(ctx, "ip", host)
	if err != nil {
		return nil, err
	}
	addrs := make([]netip.Addr, 0, len(ips))
	for _, ip := range ips {
		if addr, ok := netip.AddrFromSlice(ip); ok {
			addrs = append(addrs, addr)
		}
	}
	return addrs, nil
}

func defaultDial(ctx context.Context, addr netip.AddrPort) (net.Conn, error) {
	dialer := &net.Dialer{Timeout: dialTimeout}
	return dialer.DialContext(ctx, "tcp", addr.String())
}

func defaultLocalAddrs() ([]netip.Addr, error) {
	ifaces, err := net.InterfaceAddrs()
	if err != nil {
		return nil, err
	}
	addrs := make([]netip.Addr, 0, len(ifaces))
	for _, iface := range ifaces {
		ipNet, ok := iface.(*net.IPNet)
		if !ok {
			continue
		}
		if addr, ok := netip.AddrFromSlice(ipNet.IP); ok {
			addrs = append(addrs, addr.WithZone(""))
		}
	}
	return addrs, nil
}

func mustPrefixes(cidrs ...string) []netip.Prefix {
	prefixes := make([]netip.Prefix, 0, len(cidrs))
	for _, cidr := range cidrs {
		if strings.Contains(cidr, "/") {
			prefixes = append(prefixes, netip.MustParsePrefix(cidr))
			continue
		}
		addr := netip.MustParseAddr(cidr)
		prefixes = append(prefixes, netip.PrefixFrom(addr, addr.BitLen()))
	}
	return prefixes
}

// embeddedIPv4 decodes the IPv4 address an IPv6 answer actually
// delivers to: IPv4-mapped, the deprecated IPv4-compatible form, NAT64
// (64:ff9b::/96), and 6to4 (2002::/16). Decoding only ever adds
// denials — the caller must never let the decoded form earn an
// exemption the original spelling did not have.
func embeddedIPv4(addr netip.Addr) (netip.Addr, bool) {
	if addr.Is4() {
		return netip.Addr{}, false
	}
	if addr.Is4In6() {
		return addr.Unmap(), true
	}
	b := addr.As16()
	v4 := func(from int) netip.Addr { return netip.AddrFrom4([4]byte{b[from], b[from+1], b[from+2], b[from+3]}) }
	allZero := func(to int) bool {
		for i := 0; i < to; i++ {
			if b[i] != 0 {
				return false
			}
		}
		return true
	}
	switch {
	// NAT64 well-known prefix 64:ff9b::/96.
	case b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b && b[4] == 0 && b[5] == 0 &&
		b[6] == 0 && b[7] == 0 && b[8] == 0 && b[9] == 0 && b[10] == 0 && b[11] == 0:
		return v4(12), true
	// 6to4 2002::/16 embeds the IPv4 in bytes 2..6.
	case b[0] == 0x20 && b[1] == 0x02:
		return v4(2), true
	// IPv4-compatible ::a.b.c.d (first 96 bits zero).
	case allZero(12):
		return v4(12), true
	}
	return netip.Addr{}, false
}

func inExempt(addr netip.Addr, exempt []netip.Addr) bool {
	for _, e := range exempt {
		if addr == e || addr.Unmap() == e {
			return true
		}
	}
	return false
}

// checkAddr returns the denial reason for one resolved address, or ""
// if it may be dialed. Exemption is checked first and only against the
// original spelling; every denied class is then applied to both the
// address and its embedded IPv4 form, as are this host's own interface
// addresses (a service bound to 0.0.0.0 answers on those exactly as on
// loopback).
func (g *addressGuard) checkAddr(addr netip.Addr, exempt []netip.Addr, local []netip.Addr) string {
	addr = addr.WithZone("")
	if inExempt(addr, exempt) {
		return ""
	}
	v4, hasV4 := embeddedIPv4(addr)
	matches := func(prefixes []netip.Prefix) bool {
		for _, p := range prefixes {
			if p.Contains(addr) || (hasV4 && p.Contains(v4)) {
				return true
			}
		}
		return false
	}
	for _, class := range deniedClasses {
		if matches(class.prefixes) {
			return class.reason
		}
	}
	for _, l := range local {
		if addr == l || (hasV4 && v4 == l) {
			return "one of this host's addresses"
		}
	}
	return ""
}

// resolve applies the guard to a hostname target and returns the
// surviving addresses. A name with no surviving address fails with
// guardDeniedError; a name with no answer at all fails like the
// resolver did.
func (g *addressGuard) resolve(ctx context.Context, host string, exempt []netip.Addr) ([]netip.Addr, error) {
	addrs, err := g.lookupIP(ctx, host)
	if err != nil {
		return nil, err
	}
	local := g.local()
	loopbackName := isLoopbackName(host)
	var kept []netip.Addr
	reasons := map[string]struct{}{}
	for _, addr := range addrs {
		addr = addr.WithZone("")
		// RFC 6761 names resolve to loopback (or an exempt literal) and
		// nothing else.
		if loopbackName && !addr.IsLoopback() && !inExempt(addr, exempt) {
			reasons["a non-loopback address"] = struct{}{}
			continue
		}
		if reason := g.checkAddr(addr, exempt, local); reason != "" {
			reasons[reason] = struct{}{}
			continue
		}
		kept = append(kept, addr)
	}
	if len(kept) > 0 {
		return kept, nil
	}
	if len(reasons) > 0 {
		classes := make([]string, 0, len(reasons))
		for r := range reasons {
			classes = append(classes, r)
		}
		return nil, &guardDeniedError{host: host, reason: strings.Join(classes, " / ")}
	}
	return nil, fmt.Errorf("no addresses for %s", host)
}

// checkLiteral applies the guard to an IP-literal target. Literals
// matched by an explicit allow rule (the policy said Matched && Allow)
// are exempt; every other literal faces the denied classes — the
// unmatched-allow posture must never let 169.254.169.254 ride through.
func (g *addressGuard) checkLiteral(addr netip.Addr, dec Decision) error {
	if dec.Matched && dec.Allow {
		return nil
	}
	local := g.local()
	if reason := g.checkAddr(addr, nil, local); reason != "" {
		return &guardDeniedError{host: addr.String(), reason: reason}
	}
	return nil
}

// dialGuarded resolves (when needed), filters, and dials the first
// reachable surviving address — resolution and dial are one step, with
// no second lookup in between (rebinding TOCTOU). It returns the dialed
// connection and the address actually used.
func (g *addressGuard) dialGuarded(ctx context.Context, dec Decision, host string, port int) (net.Conn, string, error) {
	var candidates []netip.Addr
	if literal, err := netip.ParseAddr(host); err == nil {
		if err := g.checkLiteral(literal, dec); err != nil {
			return nil, "", err
		}
		candidates = []netip.Addr{literal.WithZone("")}
	} else {
		surviving, err := g.resolve(ctx, host, dec.ExemptLiterals)
		if err != nil {
			return nil, "", err
		}
		candidates = surviving
	}
	var lastErr error
	for _, addr := range candidates {
		addrPort := netip.AddrPortFrom(addr, uint16(port))
		if g.isSelf(addrPort) {
			lastErr = errTargetIsProxy
			continue
		}
		conn, err := g.dial(ctx, addrPort)
		if err != nil {
			lastErr = err
			continue
		}
		return conn, addr.String(), nil
	}
	if lastErr == nil {
		lastErr = fmt.Errorf("no usable address for %s", host)
	}
	return nil, "", lastErr
}

// setSelfPort wires the listener port for the self-connect refusal
// (NewServer binds :0, so the port is unknown at guard construction).
func (g *addressGuard) setSelfPort(port int) { g.selfPort = port }

// isSelf reports whether addrPort is this proxy's own listener —
// loopback on our port, since we only ever bind 127.0.0.1.
func (g *addressGuard) isSelf(addrPort netip.AddrPort) bool {
	return g.selfPort != 0 && addrPort.Addr().IsLoopback() && int(addrPort.Port()) == g.selfPort
}
