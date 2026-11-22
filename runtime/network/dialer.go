// Package network owns every outbound connection made for the guest network.
package network

import (
	"bufio"
	"context"
	"crypto/tls"
	"encoding/base64"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/netip"
	"net/url"
	"strconv"
	"strings"
	"sync/atomic"
	"time"
)

type Config struct {
	ProxyEnabled        bool     `json:"proxyEnabled"`
	ProxyURL            string   `json:"proxyURL"`
	Interface           string   `json:"interface"`
	DNSServers          []string `json:"dnsServers"`
	BootstrapDNSServers []string `json:"bootstrapDNSServers"`
}

type Dialer struct {
	direct           net.Dialer
	proxy            *url.URL
	dns              []string
	dnsNext          atomic.Uint64
	bootstrapDNS     []string
	bootstrapDNSNext atomic.Uint64
	resolver         *net.Resolver
}

// New requires an explicitly selected physical interface. Failure never falls
// back to the host's default route, which may be a VPN or TUN interface.
func New(config Config) (*Dialer, error) {
	iface, err := net.InterfaceByName(config.Interface)
	if err != nil {
		return nil, fmt.Errorf("physical network interface is unavailable: %w", err)
	}
	if iface.Flags&net.FlagUp == 0 || iface.Flags&(net.FlagLoopback|net.FlagPointToPoint) != 0 {
		return nil, errors.New("selected network interface must be an active physical interface")
	}
	d := &Dialer{direct: net.Dialer{Timeout: 15 * time.Second}}
	d.direct.ControlContext = interfaceControl(iface.Index)
	d.dns, err = dnsAddresses(config.DNSServers)
	if err != nil {
		return nil, err
	}
	if len(d.dns) == 0 {
		return nil, errors.New("no DNS server is available for the selected network")
	}
	bootstrapServers := config.BootstrapDNSServers
	if bootstrapServers == nil {
		// Unless specified separately, both paths use the configured DNS list.
		// An explicitly empty list means no bootstrap DNS is available.
		bootstrapServers = config.DNSServers
	}
	d.bootstrapDNS, err = dnsAddresses(bootstrapServers)
	if err != nil {
		return nil, fmt.Errorf("bootstrap DNS: %w", err)
	}
	// Proxy hostnames are bootstrapped over the physical interface. Guest DNS
	// uses a separate resolver below and follows the selected proxy mode.
	d.direct.Resolver = &net.Resolver{PreferGo: true, Dial: d.dialDirectDNS}
	if config.ProxyEnabled {
		d.proxy, err = parseProxy(config.ProxyURL)
		if err != nil {
			return nil, err
		}
	}
	d.resolver = &net.Resolver{PreferGo: true, Dial: d.dialDNS}
	return d, nil
}

func dnsAddresses(servers []string) ([]string, error) {
	addresses := make([]string, 0, len(servers))
	for _, server := range servers {
		address, err := dnsAddress(server)
		if err != nil {
			return nil, err
		}
		addresses = append(addresses, address)
	}
	return addresses, nil
}

func dnsAddress(server string) (string, error) {
	if ip, err := netip.ParseAddr(server); err == nil {
		return net.JoinHostPort(ip.String(), "53"), nil
	}
	host, port, err := net.SplitHostPort(server)
	_, ipErr := netip.ParseAddr(host)
	if err != nil || ipErr != nil || !validPort(port) {
		return "", errors.New("DNS servers must be IP addresses with an optional port")
	}
	return net.JoinHostPort(host, port), nil
}

func validPort(port string) bool {
	n, err := strconv.Atoi(port)
	return err == nil && n > 0 && n <= 65535
}

func parseProxy(raw string) (*url.URL, error) {
	u, err := url.Parse(raw)
	if err != nil {
		// url.Parse errors can contain the complete URL, including credentials.
		return nil, errors.New("invalid proxy URL")
	}
	u.Scheme = strings.ToLower(u.Scheme)
	if u.Scheme != "socks5" && u.Scheme != "http" && u.Scheme != "https" {
		return nil, errors.New("proxy URL must use socks5, http, or https")
	}
	if u.Hostname() == "" || u.Opaque != "" || u.Path != "" || u.RawQuery != "" || u.Fragment != "" {
		return nil, errors.New("proxy URL must contain only a host, port, and optional credentials")
	}
	port := u.Port()
	if !validPort(port) {
		return nil, errors.New("proxy URL requires a port between 1 and 65535")
	}
	u.Host = net.JoinHostPort(u.Hostname(), port)
	if u.User != nil {
		password, _ := u.User.Password()
		if len(u.User.Username()) > 255 || len(password) > 255 {
			return nil, errors.New("proxy credentials exceed 255 bytes")
		}
	}
	return u, nil
}

func (d *Dialer) Resolver() *net.Resolver { return d.resolver }

func (d *Dialer) dialDirectDNS(ctx context.Context, network, _ string) (net.Conn, error) {
	return d.dialDNSServers(ctx, network, false)
}

func (d *Dialer) dialDNS(ctx context.Context, network, _ string) (net.Conn, error) {
	if d.proxy != nil {
		// A stream connection makes net.Resolver use DNS's TCP framing, even
		// when its first attempt requested UDP. HTTP CONNECT cannot carry UDP.
		network = "tcp"
	}
	return d.dialDNSServers(ctx, network, true)
}

func (d *Dialer) dialDNSServers(ctx context.Context, network string, guest bool) (net.Conn, error) {
	servers, next := d.dns, &d.dnsNext
	if !guest {
		servers, next = d.bootstrapDNS, &d.bootstrapDNSNext
	}
	if len(servers) == 0 {
		return nil, errors.New("no bootstrap DNS server is available for the selected physical network")
	}
	var lastErr error
	start := int((next.Add(1) - 1) % uint64(len(servers)))
	for offset := range servers {
		// Rotate even when dialing succeeded but the DNS query later timed
		// out, so the Go resolver's next attempt reaches another server.
		server := servers[(start+offset)%len(servers)]
		var conn net.Conn
		if guest {
			conn, lastErr = d.DialContext(ctx, network, server)
		} else {
			conn, lastErr = d.direct.DialContext(ctx, network, server)
		}
		if lastErr == nil {
			return conn, nil
		}
	}
	return nil, lastErr
}

func (d *Dialer) DialContext(ctx context.Context, network, address string) (net.Conn, error) {
	if network != "tcp" && network != "tcp4" && network != "tcp6" && network != "udp" && network != "udp4" && network != "udp6" {
		return nil, errors.New("guest network supports only TCP and UDP")
	}
	if d.proxy == nil {
		return d.direct.DialContext(ctx, network, address)
	}
	ctx, cancel := context.WithTimeout(ctx, 15*time.Second)
	defer cancel()
	if strings.HasPrefix(network, "udp") {
		if d.proxy.Scheme != "socks5" {
			return nil, errors.New("HTTP/HTTPS proxies do not support guest UDP; use a SOCKS5 proxy with UDP support")
		}
		return d.dialSOCKSUDP(ctx, address)
	}
	if d.proxy.Scheme == "socks5" {
		conn, _, err := d.socksRequest(ctx, 1, address)
		return conn, err
	}
	return d.dialHTTP(ctx, address)
}

// handshakeDeadline also interrupts a handshake when the caller cancels.
func handshakeDeadline(ctx context.Context, conn net.Conn) func() {
	if deadline, ok := ctx.Deadline(); ok {
		_ = conn.SetDeadline(deadline)
	}
	stop := context.AfterFunc(ctx, func() { _ = conn.Close() })
	return func() {
		stop()
		_ = conn.SetDeadline(time.Time{})
	}
}

func (d *Dialer) dialHTTP(ctx context.Context, address string) (net.Conn, error) {
	conn, err := d.direct.DialContext(ctx, "tcp", d.proxy.Host)
	if err != nil {
		return nil, fmt.Errorf("connect to proxy: %w", err)
	}
	finish := handshakeDeadline(ctx, conn)
	defer finish()
	failed := true
	defer func() {
		if failed {
			_ = conn.Close()
		}
	}()
	if d.proxy.Scheme == "https" {
		tlsConn := tls.Client(conn, &tls.Config{ServerName: d.proxy.Hostname(), MinVersion: tls.VersionTLS12})
		if err := tlsConn.HandshakeContext(ctx); err != nil {
			return nil, fmt.Errorf("proxy TLS handshake: %w", err)
		}
		conn = tlsConn
	}
	req := &http.Request{Method: http.MethodConnect, URL: &url.URL{Opaque: address}, Host: address, Header: make(http.Header)}
	if d.proxy.User != nil {
		password, _ := d.proxy.User.Password()
		credentials := d.proxy.User.Username() + ":" + password
		req.Header.Set("Proxy-Authorization", "Basic "+base64.StdEncoding.EncodeToString([]byte(credentials)))
	}
	if err := req.Write(conn); err != nil {
		return nil, fmt.Errorf("send proxy CONNECT: %w", err)
	}
	reader := bufio.NewReader(conn)
	response, err := http.ReadResponse(reader, req)
	if err != nil {
		return nil, errors.New("invalid HTTP proxy CONNECT response")
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return nil, fmt.Errorf("HTTP proxy rejected CONNECT with status %d", response.StatusCode)
	}
	failed = false
	// A proxy can send tunneled bytes in the same packet as its response.
	return &bufferedConn{Conn: conn, reader: reader}, nil
}

type bufferedConn struct {
	net.Conn
	reader *bufio.Reader
}

func (c *bufferedConn) Read(p []byte) (int, error) { return c.reader.Read(p) }

func (c *bufferedConn) CloseWrite() error {
	if conn, ok := c.Conn.(interface{ CloseWrite() error }); ok {
		return conn.CloseWrite()
	}
	return c.Conn.Close()
}
