//go:build !darwin

package network

import (
	"context"
	"errors"
	"syscall"
)

func interfaceControl(_ int) func(context.Context, string, string, syscall.RawConn) error {
	return func(context.Context, string, string, syscall.RawConn) error {
		return errors.New("physical-interface routing requires macOS")
	}
}
