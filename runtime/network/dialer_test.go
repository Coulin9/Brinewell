package network

import (
	"bufio"
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func testDialer(t *testing.T, proxy string) *Dialer {
	t.Helper()
	u, err := parseProxy(proxy)
	if err != nil {
		t.Fatal(err)
	}
	d := &Dialer{proxy: u, dns: []string{"203.0.113.53:53"}, direct: net.Dialer{Timeout: time.Second}}
	d.resolver = &net.Resolver{PreferGo: true, Dial: d.dialDNS}
	return d
}

func listenTCP(t *testing.T) net.Listener {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = ln.Close() })
	return ln
}

func TestHTTPConnectPreservesBufferedPayloadAndAuthentication(t *testing.T) {
	ln := listenTCP(t)
	requests := make(chan *http.Request, 1)
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		request, err := http.ReadRequest(bufio.NewReader(conn))
		if err != nil {
			return
		}
		requests <- request
		_, _ = io.WriteString(conn, "HTTP/1.1 200 Connection Established\r\n\r\nhello")
	}()
	d := testDialer(t, "http://alice:secret@"+ln.Addr().String())
	conn, err := d.DialContext(context.Background(), "tcp", "198.51.100.8:443")
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	payload := make([]byte, 5)
	if _, err := io.ReadFull(conn, payload); err != nil || string(payload) != "hello" {
		t.Fatalf("tunneled payload = %q, %v", payload, err)
	}
	request := <-requests
	if request.Method != "CONNECT" || request.Host != "198.51.100.8:443" || request.Header.Get("Proxy-Authorization") != "Basic YWxpY2U6c2VjcmV0" {
		t.Fatal("CONNECT destination or authentication did not reach proxy")
	}
}

func TestHTTPUDPIsRejectedWithoutConnecting(t *testing.T) {
	for _, scheme := range []string{"http", "https"} {
		d := testDialer(t, scheme+"://127.0.0.1:1")
		_, err := d.DialContext(context.Background(), "udp", "127.0.0.1:9")
		if err == nil || !strings.Contains(err.Error(), "do not support guest UDP") {
			t.Fatalf("%s UDP error = %v", scheme, err)
		}
	}
}

func TestHTTPSProxyVerifiesCertificate(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {}))
	defer server.Close()
	d := testDialer(t, server.URL)
	_, err := d.DialContext(context.Background(), "tcp", "198.51.100.8:443")
	if err == nil || !strings.Contains(err.Error(), "proxy TLS handshake") {
		t.Fatalf("untrusted HTTPS proxy certificate was accepted: %v", err)
	}
}

func TestProxyErrorsDoNotExposeCredentials(t *testing.T) {
	for _, raw := range []string{"http://alice:secret@localhost:abc", "socks4://alice:secret@localhost:1080", "http://alice:secret@localhost:99999"} {
		_, err := parseProxy(raw)
		if err == nil || strings.Contains(err.Error(), "secret") || strings.Contains(err.Error(), "alice") {
			t.Fatalf("unsafe validation result: %v", err)
		}
	}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusProxyAuthRequired)
	}))
	defer server.Close()
	d := testDialer(t, strings.Replace(server.URL, "://", "://alice:secret@", 1))
	_, err := d.DialContext(context.Background(), "tcp", "198.51.100.8:443")
	if err == nil || !strings.Contains(err.Error(), "407") || strings.Contains(err.Error(), "secret") {
		t.Fatalf("unsafe CONNECT failure: %v", err)
	}
}

func TestProxyURLValidation(t *testing.T) {
	for _, raw := range []string{"http://localhost", "http://localhost:80/", "http://localhost:80?query=1", "http://localhost:80#fragment"} {
		if _, err := parseProxy(raw); err == nil {
			t.Fatalf("accepted invalid proxy URL %s", raw)
		}
	}
	proxy, err := parseProxy("HTTPS://localhost:443")
	if err != nil || proxy.Scheme != "https" {
		t.Fatalf("uppercase proxy URL = %v, %v", proxy, err)
	}
	if address, err := dnsAddress("fe80::1%en0"); err != nil || address != "[fe80::1%en0]:53" {
		t.Fatalf("scoped DNS address = %s, %v", address, err)
	}
}

func TestProxyFailureNeverConnectsDirectly(t *testing.T) {
	for _, scheme := range []string{"http", "https", "socks5"} {
		t.Run(scheme, func(t *testing.T) {
			target := listenTCP(t).(*net.TCPListener)
			proxy := listenTCP(t)
			proxyAddress := proxy.Addr().String()
			_ = proxy.Close()
			d := testDialer(t, scheme+"://"+proxyAddress)
			if _, err := d.DialContext(context.Background(), "tcp", target.Addr().String()); err == nil {
				t.Fatal("connection succeeded with a closed proxy")
			}
			_ = target.SetDeadline(time.Now().Add(30 * time.Millisecond))
			if conn, err := target.Accept(); err == nil {
				_ = conn.Close()
				t.Fatal("proxy failure leaked a direct connection to the target")
			}
		})
	}
}

func TestDNSRetriesRotateServers(t *testing.T) {
	first := listenTCP(t)
	second := listenTCP(t)
	d := &Dialer{bootstrapDNS: []string{first.Addr().String(), second.Addr().String()}}
	for _, expected := range []string{first.Addr().String(), second.Addr().String(), first.Addr().String()} {
		conn, err := d.dialDirectDNS(context.Background(), "tcp", "ignored:53")
		if err != nil {
			t.Fatal(err)
		}
		if conn.RemoteAddr().String() != expected {
			t.Fatalf("DNS retry connected to %s; want %s", conn.RemoteAddr(), expected)
		}
		_ = conn.Close()
	}
}

func TestProxyBootstrapDNSIsSeparateFromGuestDNS(t *testing.T) {
	ln := listenTCP(t)
	bootstrap, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer bootstrap.Close()
	bootstrapQueries := make(chan string, 8)
	go func() {
		packet := make([]byte, 1500)
		for {
			n, address, err := bootstrap.ReadFromUDP(packet)
			if err != nil {
				return
			}
			response, name := testDNSReply(packet[:n], net.IPv4(127, 0, 0, 1))
			bootstrapQueries <- name
			_, _ = bootstrap.WriteToUDP(response, address)
		}
	}()
	destination := make(chan string, 1)
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		reader := bufio.NewReader(conn)
		request, err := http.ReadRequest(reader)
		if err != nil {
			return
		}
		destination <- request.Host
		_, _ = io.WriteString(conn, "HTTP/1.1 200 OK\r\n\r\n")
		length := make([]byte, 2)
		if _, err := io.ReadFull(reader, length); err != nil {
			return
		}
		query := make([]byte, binary.BigEndian.Uint16(length))
		if _, err := io.ReadFull(reader, query); err != nil {
			return
		}
		response, _ := testDNSReply(query, net.IPv4(198, 51, 100, 42))
		_, _ = conn.Write(binary.BigEndian.AppendUint16(nil, uint16(len(response))))
		_, _ = conn.Write(response)
	}()
	_, port, _ := net.SplitHostPort(ln.Addr().String())
	d := testDialer(t, "http://proxy.brinewell.test:"+port)
	d.bootstrapDNS = []string{bootstrap.LocalAddr().String()}
	d.direct.Resolver = &net.Resolver{PreferGo: true, Dial: d.dialDirectDNS}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	ips, err := d.Resolver().LookupIP(ctx, "ip4", "brinewell.example")
	if err != nil || len(ips) != 1 || !ips[0].Equal(net.IPv4(198, 51, 100, 42)) {
		t.Fatalf("proxied DNS response = %v, %v", ips, err)
	}
	if got := <-destination; got != "203.0.113.53:53" {
		t.Fatalf("proxy DNS target = %s", got)
	}
	if len(bootstrapQueries) == 0 {
		t.Fatal("proxy hostname was not resolved through bootstrap DNS")
	}
	for len(bootstrapQueries) > 0 {
		if name := <-bootstrapQueries; name != "proxy.brinewell.test" {
			t.Fatalf("guest DNS query leaked to bootstrap DNS: %s", name)
		}
	}
}

func testDNSReply(query []byte, ip net.IP) ([]byte, string) {
	// Retain the question, remove optional EDNS data, and answer A queries.
	end := 12
	var labels []string
	for end < len(query) && query[end] != 0 {
		length := int(query[end])
		if end+1+length > len(query) {
			return nil, ""
		}
		labels = append(labels, string(query[end+1:end+1+length]))
		end += length + 1
	}
	end += 5
	if end > len(query) {
		return nil, ""
	}
	response := append([]byte(nil), query[:end]...)
	binary.BigEndian.PutUint16(response[2:4], 0x8180)
	binary.BigEndian.PutUint16(response[6:8], 0)
	binary.BigEndian.PutUint16(response[10:12], 0)
	if binary.BigEndian.Uint16(query[end-4:end-2]) == 1 {
		binary.BigEndian.PutUint16(response[6:8], 1)
		response = append(response, 0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 30, 0, 4)
		response = append(response, ip.To4()...)
	}
	return response, strings.Join(labels, ".")
}

func TestProxyHandshakeCancellation(t *testing.T) {
	for _, scheme := range []string{"http", "socks5"} {
		t.Run(scheme, func(t *testing.T) {
			ln := listenTCP(t)
			accepted := make(chan net.Conn, 1)
			go func() {
				conn, err := ln.Accept()
				if err == nil {
					accepted <- conn
				}
			}()
			d := testDialer(t, fmt.Sprintf("%s://%s", scheme, ln.Addr()))
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			result := make(chan error, 1)
			go func() {
				_, err := d.DialContext(ctx, "tcp", "198.51.100.8:443")
				result <- err
			}()
			conn := <-accepted
			defer conn.Close()
			cancel()
			select {
			case err := <-result:
				if err == nil {
					t.Fatal("cancelled handshake succeeded")
				}
			case <-time.After(time.Second):
				t.Fatal("cancelled handshake remained blocked")
			}
		})
	}
}
