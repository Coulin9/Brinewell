package network

import (
	"context"
	"net"
	"strings"
	"syscall"
	"testing"
)

func physicalTestInterface(t *testing.T) net.Interface {
	t.Helper()
	interfaces, err := net.Interfaces()
	if err != nil {
		t.Fatal(err)
	}
	for _, iface := range interfaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&(net.FlagLoopback|net.FlagPointToPoint) != 0 || !strings.HasPrefix(iface.Name, "en") {
			continue
		}
		addresses, _ := iface.Addrs()
		for _, address := range addresses {
			ip, _, _ := net.ParseCIDR(address.String())
			if ip.To4() != nil {
				return iface
			}
		}
	}
	t.Skip("no active physical IPv4 interface is available")
	return net.Interface{}
}

func TestDirectSocketBindsSelectedPhysicalInterface(t *testing.T) {
	iface := physicalTestInterface(t)
	d, err := New(Config{Interface: iface.Name, DNSServers: []string{"192.0.2.53"}})
	if err != nil {
		t.Fatal(err)
	}
	// Connecting a UDP socket chooses the route but transmits no packet.
	conn, err := d.DialContext(context.Background(), "udp4", "198.51.100.1:9")
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	raw, err := conn.(*net.UDPConn).SyscallConn()
	if err != nil {
		t.Fatal(err)
	}
	var bound int
	var socketErr error
	if err := raw.Control(func(fd uintptr) {
		bound, socketErr = syscall.GetsockoptInt(int(fd), syscall.IPPROTO_IP, syscall.IP_BOUND_IF)
	}); err != nil {
		t.Fatal(err)
	}
	if socketErr != nil || bound != iface.Index {
		t.Fatalf("IP_BOUND_IF = %d, %v; want %d", bound, socketErr, iface.Index)
	}
}

func TestUnavailableInterfaceFailsClosed(t *testing.T) {
	if _, err := New(Config{Interface: "brinewell-missing", DNSServers: []string{"192.0.2.53"}}); err == nil {
		t.Fatal("missing interface accepted")
	}
	d := net.Dialer{ControlContext: interfaceControl(0x7fffffff)}
	if conn, err := d.DialContext(context.Background(), "udp4", "198.51.100.1:9"); err == nil {
		_ = conn.Close()
		t.Fatal("invalid interface binding fell back to a default route")
	}
}

func TestBootstrapDNSConfiguration(t *testing.T) {
	iface := physicalTestInterface(t)
	for _, test := range []struct {
		name      string
		bootstrap []string
		want      string
	}{
		{name: "omitted uses legacy DNS", want: "192.0.2.53:53"},
		{name: "explicit physical DNS", bootstrap: []string{"192.0.2.54"}, want: "192.0.2.54:53"},
		{name: "explicit empty stays empty", bootstrap: []string{}},
	} {
		t.Run(test.name, func(t *testing.T) {
			d, err := New(Config{Interface: iface.Name, DNSServers: []string{"192.0.2.53"}, BootstrapDNSServers: test.bootstrap})
			if err != nil {
				t.Fatal(err)
			}
			if test.want == "" {
				if len(d.bootstrapDNS) != 0 {
					t.Fatal("empty physical DNS fell back to guest DNS")
				}
				if _, err := d.dialDirectDNS(context.Background(), "udp", "ignored:53"); err == nil {
					t.Fatal("missing bootstrap DNS did not fail explicitly")
				}
			} else if len(d.bootstrapDNS) != 1 || d.bootstrapDNS[0] != test.want {
				t.Fatalf("bootstrap DNS = %v; want %s", d.bootstrapDNS, test.want)
			}
			if len(d.dns) != 1 || d.dns[0] != "192.0.2.53:53" {
				t.Fatal("bootstrap configuration changed guest DNS")
			}
		})
	}
}
