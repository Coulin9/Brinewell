package network

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"strconv"
	"sync"
)

func socksAddress(address string) ([]byte, error) {
	host, port, err := net.SplitHostPort(address)
	if err != nil || !validPort(port) {
		return nil, errors.New("invalid proxy destination address")
	}
	var encoded []byte
	if ip := net.ParseIP(host); ip != nil {
		if v4 := ip.To4(); v4 != nil {
			encoded = append([]byte{1}, v4...)
		} else {
			encoded = append([]byte{4}, ip.To16()...)
		}
	} else {
		if len(host) == 0 || len(host) > 255 {
			return nil, errors.New("invalid proxy destination hostname")
		}
		encoded = append([]byte{3, byte(len(host))}, host...)
	}
	n, _ := strconv.Atoi(port)
	return binary.BigEndian.AppendUint16(encoded, uint16(n)), nil
}

func readSOCKSAddress(reader io.Reader) (string, error) {
	header := make([]byte, 1)
	if _, err := io.ReadFull(reader, header); err != nil {
		return "", err
	}
	var host string
	switch header[0] {
	case 1, 4:
		length := net.IPv4len
		if header[0] == 4 {
			length = net.IPv6len
		}
		ip := make([]byte, length)
		if _, err := io.ReadFull(reader, ip); err != nil {
			return "", err
		}
		host = net.IP(ip).String()
	case 3:
		if _, err := io.ReadFull(reader, header); err != nil {
			return "", err
		}
		name := make([]byte, int(header[0]))
		if len(name) == 0 {
			return "", errors.New("empty SOCKS5 hostname")
		}
		if _, err := io.ReadFull(reader, name); err != nil {
			return "", err
		}
		host = string(name)
	default:
		return "", errors.New("invalid SOCKS5 address type")
	}
	port := make([]byte, 2)
	if _, err := io.ReadFull(reader, port); err != nil {
		return "", err
	}
	return net.JoinHostPort(host, strconv.Itoa(int(binary.BigEndian.Uint16(port)))), nil
}

func (d *Dialer) socksRequest(ctx context.Context, command byte, destination string) (net.Conn, string, error) {
	conn, err := d.direct.DialContext(ctx, "tcp", d.proxy.Host)
	if err != nil {
		return nil, "", fmt.Errorf("connect to SOCKS5 proxy: %w", err)
	}
	finish := handshakeDeadline(ctx, conn)
	defer finish()
	failed := true
	defer func() {
		if failed {
			_ = conn.Close()
		}
	}()
	greeting := []byte{5, 1, 0}
	if d.proxy.User != nil {
		greeting = []byte{5, 2, 0, 2}
	}
	if _, err := conn.Write(greeting); err != nil {
		return nil, "", err
	}
	response := make([]byte, 2)
	if _, err := io.ReadFull(conn, response); err != nil {
		return nil, "", err
	}
	if response[0] != 5 {
		return nil, "", errors.New("invalid SOCKS5 greeting")
	}
	switch response[1] {
	case 0:
	case 2:
		if d.proxy.User == nil {
			return nil, "", errors.New("SOCKS5 proxy requires credentials")
		}
		username := d.proxy.User.Username()
		password, _ := d.proxy.User.Password()
		auth := append([]byte{1, byte(len(username))}, username...)
		auth = append(auth, byte(len(password)))
		auth = append(auth, password...)
		if _, err := conn.Write(auth); err != nil {
			return nil, "", err
		}
		if _, err := io.ReadFull(conn, response); err != nil {
			return nil, "", err
		}
		if response[0] != 1 || response[1] != 0 {
			return nil, "", errors.New("SOCKS5 authentication rejected")
		}
	default:
		return nil, "", errors.New("SOCKS5 proxy rejected supported authentication methods")
	}
	var address []byte
	if command == 3 {
		// No local UDP port is known yet. RFC 1928 permits all zeroes.
		address = []byte{1, 0, 0, 0, 0, 0, 0}
	} else {
		address, err = socksAddress(destination)
		if err != nil {
			return nil, "", err
		}
	}
	if _, err := conn.Write(append([]byte{5, command, 0}, address...)); err != nil {
		return nil, "", err
	}
	reply := make([]byte, 3)
	if _, err := io.ReadFull(conn, reply); err != nil {
		return nil, "", err
	}
	if reply[0] != 5 || reply[2] != 0 {
		return nil, "", errors.New("invalid SOCKS5 reply")
	}
	if reply[1] != 0 {
		return nil, "", fmt.Errorf("SOCKS5 proxy rejected command %d with status %d", command, reply[1])
	}
	bound, err := readSOCKSAddress(conn)
	if err != nil {
		return nil, "", err
	}
	failed = false
	return conn, bound, nil
}

func (d *Dialer) dialSOCKSUDP(ctx context.Context, destination string) (net.Conn, error) {
	header, err := socksAddress(destination)
	if err != nil {
		return nil, err
	}
	control, relay, err := d.socksRequest(ctx, 3, "")
	if err != nil {
		return nil, err
	}
	host, port, err := net.SplitHostPort(relay)
	if err != nil || !validPort(port) {
		_ = control.Close()
		return nil, errors.New("invalid SOCKS5 UDP relay address")
	}
	if ip := net.ParseIP(host); ip != nil && ip.IsUnspecified() {
		// Several local proxies return 0.0.0.0 instead of their listening IP.
		host, _, _ = net.SplitHostPort(control.RemoteAddr().String())
		relay = net.JoinHostPort(host, port)
	}
	udp, err := d.direct.DialContext(ctx, "udp", relay)
	if err != nil {
		_ = control.Close()
		return nil, fmt.Errorf("connect to SOCKS5 UDP relay: %w", err)
	}
	conn := &socksUDPConn{Conn: udp, control: control, header: append([]byte{0, 0, 0}, header...)}
	go func() {
		// A UDP association exists only while its TCP control channel lives.
		_, _ = io.Copy(io.Discard, control)
		_ = conn.Close()
	}()
	return conn, nil
}

type socksUDPConn struct {
	net.Conn
	control net.Conn
	header  []byte
	once    sync.Once
}

func (c *socksUDPConn) Write(payload []byte) (int, error) {
	if len(payload)+len(c.header) > 65507 {
		return 0, errors.New("UDP datagram exceeds SOCKS5 payload size")
	}
	packet := make([]byte, len(c.header)+len(payload))
	copy(packet, c.header)
	copy(packet[len(c.header):], payload)
	n, err := c.Conn.Write(packet)
	if err != nil {
		return 0, err
	}
	if n != len(packet) {
		return 0, io.ErrShortWrite
	}
	return len(payload), nil
}

func (c *socksUDPConn) Read(payload []byte) (int, error) {
	packet := make([]byte, 65535)
	for {
		n, err := c.Conn.Read(packet)
		if err != nil {
			return 0, err
		}
		if n < 4 || packet[0] != 0 || packet[1] != 0 || packet[2] != 0 {
			// Fragmentation is optional in RFC 1928; unsupported fragments must
			// be discarded. Connected UDP also rejects non-relay senders.
			continue
		}
		reader := bytes.NewReader(packet[3:n])
		if _, err := readSOCKSAddress(reader); err != nil {
			continue
		}
		return copy(payload, packet[n-reader.Len():n]), nil
	}
}

func (c *socksUDPConn) Close() error {
	var err error
	c.once.Do(func() {
		err = c.Conn.Close()
		_ = c.control.Close()
	})
	return err
}
