package network

import (
	"context"
	"net"
	"strings"
	"syscall"
)

func interfaceControl(index int) func(context.Context, string, string, syscall.RawConn) error {
	return func(_ context.Context, network, address string, raw syscall.RawConn) error {
		host, _, _ := net.SplitHostPort(address)
		if ip := net.ParseIP(host); ip != nil && ip.IsLoopback() {
			// Local proxy listeners and the VM's host gateway must stay local.
			return nil
		}
		var socketErr error
		err := raw.Control(func(fd uintptr) {
			if strings.HasSuffix(network, "6") {
				socketErr = syscall.SetsockoptInt(int(fd), syscall.IPPROTO_IPV6, syscall.IPV6_BOUND_IF, index)
			} else {
				socketErr = syscall.SetsockoptInt(int(fd), syscall.IPPROTO_IP, syscall.IP_BOUND_IF, index)
			}
		})
		if err != nil {
			return err
		}
		return socketErr
	}
}
