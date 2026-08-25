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
// Created: 2026/08/23

// Semantic derivation for network tools. The headline payoff: when the
// target URL is statically present in argv, the egress requirement is an
// ENUMERATED host set — so a host-granular capability package can cover
// exactly that destination (the policy-layer shape of domain-level
// network authorization). Credential-path arguments on an egress tool
// keep their standing exfiltration indicator.
package permission

import (
	"net/url"
	"strings"
)

// credentialPathHints are path fragments whose presence in a network
// tool's argv suggests credential exfiltration.
var credentialPathHints = []string{
	".ssh", ".aws", ".kube", ".gnupg", ".azure", "id_rsa", "id_ed25519",
	"credentials", "/etc/shadow", "/etc/passwd", ".netrc", ".git-credentials",
}

// exfilIndicator renders the standing indicator for an egress tool whose
// arguments reference a credential path.
func exfilIndicator(program, arg string) string {
	return program + " with a credential-path argument (" + arg + ") looks like secret exfiltration"
}

// credentialArgs returns the arguments referencing credential paths.
func credentialArgs(args []string) []string {
	var out []string
	for _, arg := range args {
		lower := strings.ToLower(arg)
		for _, hint := range credentialPathHints {
			if strings.Contains(lower, hint) {
				out = append(out, arg)
				break
			}
		}
	}
	return out
}

// hostOfURL extracts the canonical host from a URL-ish token. ok=false
// when the token does not parse as a URL with a host.
func hostOfURL(raw string) (string, bool) {
	if !strings.Contains(raw, "://") {
		raw = "https://" + raw // curl accepts scheme-less hosts
	}
	u, err := url.Parse(raw)
	if err != nil || u.Hostname() == "" {
		return "", false
	}
	return strings.ToLower(u.Hostname()), true
}

// curlOpts covers curl's common option grammar. Unknown options fail the
// parse (unprovable), never guessed.
var curlOpts = OptTable{
	Long: map[string]bool{
		"url": true, "request": true, "data": true, "data-raw": true,
		"data-binary": true, "data-ascii": true, "data-urlencode": true,
		"header": true, "output": true, "remote-name": false,
		"remote-name-all": false, "user": true, "user-agent": true,
		"referer": true, "cookie": true, "cookie-jar": true,
		"upload-file": true, "form": true, "form-string": true,
		"proxy": true, "proxy-user": true, "max-time": true,
		"connect-timeout": true, "retry": true, "retry-delay": true,
		"retry-max-time": true, "cacert": true, "cert": true, "key": true,
		"capath": true, "resolve": true, "write-out": true,
		"dump-header": true, "limit-rate": true, "request-target": true,
		"interface": true, "local-port": true, "http1.1": false,
		"http2": false, "http3": false, "get": false, "head": false,
		"silent": false, "show-error": false, "verbose": false,
		"location": false, "location-trusted": false, "insecure": false,
		"include": false, "fail": false, "fail-with-body": false,
		"compressed": false, "progress-bar": false, "no-progress-meter": false,
		"ipv4": false, "ipv6": false, "globoff": false, "raw": false,
		"keepalive-time": true, "max-redirs": true, "post301": false,
		"post302": false, "post303": false, "basic": false, "digest": false,
		"ntlm": false, "negotiate": false, "anyauth": false,
		"sasl-ir": false, "ssl": false, "ssl-reqd": false, "tlsv1.2": false,
		"tlsv1.3": false, "tcp-nodelay": false, "trace": true,
		"trace-ascii": true, "trace-time": false, "use-ascii": false,
		"remote-time": false, "create-dirs": false, "continue-at": true,
		"range": true, "time-cond": true, "ftp-create-dirs": false,
		"list-only": false, "netrc": false, "netrc-file": true,
		"netrc-optional": false, "parallel": false, "parallel-max": true,
		"json": true, "variable": true, "oauth2-bearer": true,
		"aws-sigv4": true, "expect100-timeout": true, "help": false,
		"version": false, "manual": false, "disable": false,
		"no-clobber": false, "remove-on-error": false, "skip-existing": false,
		"stderr": true, "styled-output": false, "test-event": false,
		"unix-socket": true, "abstract-unix-socket": true,
	},
	Short: map[rune]bool{
		's': false, 'S': false, 'v': false, 'L': false, 'k': false,
		'i': false, 'I': false, 'G': false, 'g': false, 'f': false,
		'N': false, 'q': false, 'O': false, 'J': false, 'n': false,
		'V': false, 'h': false, 'M': false, '#': false, 'a': false,
		'l': false, 'j': false, '0': false, '1': false, '2': false,
		'3': false, '4': false, '6': false, 'K': true, 'd': true,
		'H': true, 'o': true, 'u': true, 'A': true, 'e': true,
		'b': true, 'c': true, 'X': true, 'T': true, 'F': true,
		'x': true, 'm': true, 'U': true, 'E': true, 'w': true,
		'D': true, 'C': true, 'r': true, 'z': true, 'Q': true,
		'Y': true, 'y': true, 't': true, 'B': true, 'Z': false,
	},
}

// semDeriveCurl derives a curl invocation: enumerated hosts from the URL
// arguments, exfiltration indicators for credential-path arguments.
// Options that redirect or widen the real egress (proxy, config file,
// DNS overrides) degrade the requirement to Any: the URL in argv is
// then not the whole story.
func semDeriveCurl(argv []string) (Effect, bool) {
	opts, ok := ParseOpts(argv[1:], curlOpts)
	if !ok {
		return Effect{}, false
	}
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "curl"}
	for _, pos := range opts.Positional {
		if host, isURL := hostOfURL(pos); isURL {
			// Loopback targets are sandbox-permitted: not a network
			// requirement, but kept in NamedHosts for deny matching.
			if !isLoopbackHost(host) {
				e.Network.Hosts = unionStrings(e.Network.Hosts, []string{host})
			}
			e.NamedHosts = unionStrings(e.NamedHosts, []string{host})
		}
	}
	if opts.Has("-x", "--proxy", "-K", "--config", "--resolve", "--connect-to") {
		// The real egress goes through a proxy or a config file's
		// additional URLs — the coverage requirement widens to Any,
		// but NamedHosts keeps the argv-visible targets for deny
		// matching.
		e.Network = HostSet{Any: true}
	} else if len(e.NamedHosts) == 0 && !opts.Has("--version", "-V", "--help", "-h", "--manual", "-M") {
		// curl without a URL reads config/stdin for targets — the
		// destination is not statically provable.
		e.Network = HostSet{Any: true}
	}
	for _, arg := range credentialArgs(argv[1:]) {
		e.Indicators = append(e.Indicators, exfilIndicator("curl", arg))
	}
	return e, true
}

// wgetOpts covers wget's common option grammar.
var wgetOpts = OptTable{
	Long: map[string]bool{
		"output-document": true, "output-file": true, "directory-prefix": true,
		"post-data": true, "post-file": true, "method": true,
		"header": true, "user": true, "password": true, "ask-password": false,
		"http-user": true, "http-password": true, "load-cookies": true,
		"save-cookies": true, "keep-session-cookies": false,
		"recursive": false, "level": true, "mirror": false, "page-requisites": false,
		"convert-links": false, "no-clobber": false, "continue": false,
		"timestamping": false, "quiet": false, "verbose": false,
		"no-verbose": false, "spider": false, "wait": true, "timeout": true,
		"tries": true, "retry-connrefused": false, "user-agent": true,
		"referer": true, "no-check-certificate": false, "certificate": true,
		"private-key": true, "ca-certificate": true, "ca-directory": true,
		"bind-address": true, "limit-rate": true, "dns-cache": false,
		"inet4-only": false, "inet6-only": false, "execute": true,
		"input-file": true, "force-html": false, "config": true,
		"no-hsts": false, "content-disposition": false, "auth-no-challenge": false,
		"secure-protocol": true, "https-only": false, "no-http-keep-alive": false,
		"max-redirect": true, "proxy-user": true, "proxy-password": true,
		"no-proxy": false, "domains": true, "exclude-domains": true,
		"accept": true, "reject": true, "accept-regex": true, "reject-regex": true,
		"ignore-length": false, "progress": true, "show-progress": false,
		"report-speed": true, "delete-after": false, "backup-converted": false,
		"adjust-extension": false, "server-response": false, "save-headers": false,
		"cookies": false, "cache": false, "random-wait": false,
		"waitretry": true, "dns-timeout": true, "connect-timeout": true,
		"read-timeout": true, "prefer-family": true, "retry-on-http-error": true,
		"compression": true, "help": false, "version": false,
	},
	Short: map[rune]bool{
		'O': true, 'o': true, 'a': true, 'P': true, 'q': false,
		'v': false, 'r': false, 'l': true, 'm': false, 'p': false,
		'k': false, 'N': false, 'c': false, 'S': false,
		'n': false, 'V': false, 'h': false, 'b': false, 'e': true,
		'i': true, 'F': false, 'B': true, 't': true, 'w': true,
		'T': true, 'U': true, 'R': true, 'A': true, 'D': true,
		'x': false, '4': false, '6': false, 'E': false, 'H': false,
		'K': false, 'L': false, 'X': true, 'I': true, 'G': true,
		'Q': true, 'd': false,
	},
}

// semDeriveWget derives a wget invocation (same shape as curl).
func semDeriveWget(argv []string) (Effect, bool) {
	opts, ok := ParseOpts(argv[1:], wgetOpts)
	if !ok {
		return Effect{}, false
	}
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "wget"}
	for _, pos := range opts.Positional {
		if host, isURL := hostOfURL(pos); isURL {
			if !isLoopbackHost(host) {
				e.Network.Hosts = unionStrings(e.Network.Hosts, []string{host})
			}
			e.NamedHosts = unionStrings(e.NamedHosts, []string{host})
		}
	}
	if opts.Has("--config", "-e", "--execute", "--proxy-user") {
		// A config file / command file can inject additional URLs.
		e.Network = HostSet{Any: true}
	} else if len(e.NamedHosts) == 0 && !opts.Has("--version", "-V", "--help", "-h") {
		e.Network = HostSet{Any: true}
	}
	for _, arg := range credentialArgs(argv[1:]) {
		e.Indicators = append(e.Indicators, exfilIndicator("wget", arg))
	}
	return e, true
}

// semDeriveNetcat classifies nc/ncat: arbitrary TCP egress, plus a
// standing indicator for the -e/--exec reverse-shell form.
func semDeriveNetcat(argv []string) (Effect, bool) {
	e := Effect{
		Proven:      true,
		Consequence: ConsequenceConfined,
		Network:     HostSet{Any: true},
		Reason:      programBase(argv[0]) + " opens arbitrary TCP connections",
	}
	for _, arg := range argv[1:] {
		if arg == "-e" || arg == "--exec" || strings.HasPrefix(arg, "--exec=") {
			e.Indicators = append(e.Indicators,
				programBase(argv[0])+" "+arg+" runs a program with network-connected stdio (shell pattern)")
		}
	}
	return e, true
}

// semDeriveScp classifies scp: any remote endpoint makes it network-Any;
// identity-file and credential-path arguments keep the indicator.
func semDeriveScp(argv []string) (Effect, bool) {
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "scp"}
	for _, arg := range argv[1:] {
		if strings.Contains(arg, ":") && !strings.HasPrefix(arg, "-") {
			e.Network = HostSet{Any: true}
			break
		}
	}
	for _, arg := range credentialArgs(argv[1:]) {
		e.Indicators = append(e.Indicators, exfilIndicator("scp", arg))
	}
	return e, true
}

// semDeriveRsync classifies rsync: remote endpoints make it network-Any;
// --delete on a remote target destroys remote data (shared-destructive).
func semDeriveRsync(argv []string) (Effect, bool) {
	e := Effect{Proven: true, Consequence: ConsequenceConfined, Reason: "rsync"}
	remote := false
	for _, arg := range argv[1:] {
		if !strings.HasPrefix(arg, "-") &&
			(strings.Contains(arg, ":") || strings.Contains(arg, "::")) {
			remote = true
			break
		}
	}
	if remote {
		e.Network = HostSet{Any: true}
		for _, arg := range argv[1:] {
			if arg == "--delete" || strings.HasPrefix(arg, "--delete-") {
				e.Consequence = ConsequenceSharedDestructive
				e.Reason = "rsync --delete removes files at the remote target"
			}
		}
	}
	for _, arg := range credentialArgs(argv[1:]) {
		e.Indicators = append(e.Indicators, exfilIndicator("rsync", arg))
	}
	return e, true
}

// semDeriveSSH classifies ssh: remote execution over an arbitrary host.
// The remote command runs OUTSIDE loom's sandbox, so ssh carries a
// standing indicator.
func semDeriveSSH(argv []string) (Effect, bool) {
	return Effect{
		Proven:      true,
		Consequence: ConsequenceConfined,
		Network:     HostSet{Any: true},
		Reason:      "ssh executes commands on a remote host, outside any local sandbox",
		Indicators:  []string{"ssh runs commands on a remote host beyond the sandbox's reach"},
	}, true
}
