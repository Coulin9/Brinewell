package network

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net"
	"testing"
	"time"
)

func acceptSOCKS(conn net.Conn, auth bool) (byte, string, error) {
	buffer := make([]byte, 2)
	if _, err := io.ReadFull(conn, buffer); err != nil {
		return 0, "", err
	}
	methods := make([]byte, buffer[1])
	if _, err := io.ReadFull(conn, methods); err != nil {
		return 0, "", err
	}
	method := byte(0)
	if auth {
		method = 2
	}
	if _, err := conn.Write([]byte{5, method}); err != nil {
		return 0, "", err
	}
	if auth {
		if _, err := io.ReadFull(conn, buffer); err != nil {
			return 0, "", err
		}
		username := make([]byte, buffer[1])
		if _, err := io.ReadFull(conn, username); err != nil {
			return 0, "", err
		}
		if _, err := io.ReadFull(conn, buffer[:1]); err != nil {
			return 0, "", err
		}
		password := make([]byte, buffer[0])
		if _, err := io.ReadFull(conn, password); err != nil {
			return 0, "", err
		}
		if string(username) != "alice" || string(password) != "secret" {
			return 0, "", errors.New("wrong credentials")
		}
		_, _ = conn.Write([]byte{1, 0})
	}
	header := make([]byte, 3)
	if _, err := io.ReadFull(conn, header); err != nil {
		return 0, "", err
	}
	address, err := readSOCKSAddress(conn)
	return header[1], address, err
}

func TestSOCKSTCPAuthenticationAndDestination(t *testing.T) {
	ln := listenTCP(t)
	result := make(chan error, 1)
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			result <- err
			return
		}
		defer conn.Close()
		command, address, err := acceptSOCKS(conn, true)
		if err != nil {
			result <- err
			return
		}
		if command != 1 || address != "example.test:443" {
			result <- errors.New("incorrect SOCKS CONNECT request")
			return
		}
		_, _ = conn.Write([]byte{5, 0, 0, 1, 127, 0, 0, 1, 0, 0})
		_, _ = io.Copy(conn, conn)
		result <- nil
	}()
	d := testDialer(t, "socks5://alice:secret@"+ln.Addr().String())
	conn, err := d.DialContext(context.Background(), "tcp", "example.test:443")
	if err != nil {
		t.Fatal(err)
	}
	_ = conn.SetDeadline(time.Now().Add(time.Second))
	_, _ = conn.Write([]byte("ping"))
	buffer := make([]byte, 4)
	if _, err := io.ReadFull(conn, buffer); err != nil || string(buffer) != "ping" {
		t.Fatalf("SOCKS echo = %q, %v", buffer, err)
	}
	_ = conn.Close()
	if err := <-result; err != nil {
		t.Fatal(err)
	}
}

func TestSOCKSUDPFramingAndControlLifetime(t *testing.T) {
	ln := listenTCP(t)
	relay, err := net.ListenUDP("udp", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1)})
	if err != nil {
		t.Fatal(err)
	}
	defer relay.Close()
	controlReady := make(chan net.Conn, 1)
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		command, _, err := acceptSOCKS(conn, false)
		if err != nil || command != 3 {
			_ = conn.Close()
			return
		}
		port := relay.LocalAddr().(*net.UDPAddr).Port
		// Wildcard relay addresses must be replaced by the proxy peer IP.
		_, _ = conn.Write([]byte{5, 0, 0, 1, 0, 0, 0, 0, byte(port >> 8), byte(port)})
		controlReady <- conn
	}()
	d := testDialer(t, "socks5://"+ln.Addr().String())
	conn, err := d.DialContext(context.Background(), "udp", "198.51.100.8:27015")
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	control := <-controlReady
	defer control.Close()
	if n, err := conn.Write([]byte("game packet")); err != nil || n != 11 {
		t.Fatalf("UDP write = %d, %v", n, err)
	}
	packet := make([]byte, 100)
	_ = relay.SetReadDeadline(time.Now().Add(time.Second))
	n, sender, err := relay.ReadFromUDP(packet)
	if err != nil {
		t.Fatal(err)
	}
	header, _ := socksAddress("198.51.100.8:27015")
	want := append(append([]byte{0, 0, 0}, header...), []byte("game packet")...)
	if !bytes.Equal(packet[:n], want) {
		t.Fatalf("SOCKS UDP frame = %v; want %v", packet[:n], want)
	}
	packet[2] = 1
	_, _ = relay.WriteToUDP(packet[:n], sender) // unsupported fragment, discarded
	packet[2] = 0
	_, _ = relay.WriteToUDP(packet[:n], sender)
	_ = conn.SetReadDeadline(time.Now().Add(time.Second))
	buffer := make([]byte, 100)
	n, err = conn.Read(buffer)
	if err != nil || string(buffer[:n]) != "game packet" {
		t.Fatalf("decoded UDP response = %q, %v", buffer[:n], err)
	}
	_ = conn.SetReadDeadline(time.Now().Add(3 * time.Second))
	_ = control.Close()
	start := time.Now()
	_, err = conn.Read(buffer)
	if err == nil || time.Since(start) > time.Second {
		t.Fatalf("UDP association survived closed TCP control channel: %v", err)
	}
}

func TestSOCKSAddressRoundTrip(t *testing.T) {
	for _, address := range []string{"192.0.2.1:53", "[2001:db8::1]:443", "example.test:27015"} {
		encoded, err := socksAddress(address)
		if err != nil {
			t.Fatal(err)
		}
		decoded, err := readSOCKSAddress(bytes.NewReader(encoded))
		if err != nil || decoded != address {
			t.Fatalf("address round trip = %s, %v", decoded, err)
		}
	}
}
