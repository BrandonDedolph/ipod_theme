package cli

import (
	"strings"
	"testing"

	"github.com/spf13/cobra"
)

func findCmd(t *testing.T, path ...string) *cobra.Command {
	t.Helper()
	cmd, _, err := Root().Find(path)
	if err != nil {
		t.Fatalf("Find(%v): %v", path, err)
	}
	if cmd.Name() != path[len(path)-1] {
		t.Fatalf("Find(%v) resolved to %q", path, cmd.Name())
	}
	return cmd
}

// TestNoUnimplementedSecurityFlags pins the removal of --verify and
// --sign. Both defaulted to true on commands that return "not yet
// implemented", which reads as a security guarantee: there is no
// signing key, no embedded public key, and no verification code
// anywhere in this module. A flag that claims to verify a signature
// while verifying nothing is worse than no flag.
func TestNoUnimplementedSecurityFlags(t *testing.T) {
	for _, tc := range []struct{ cmd, flag string }{
		{"update", "verify"},
		{"release", "sign"},
	} {
		c := findCmd(t, tc.cmd)
		if f := c.Flags().Lookup(tc.flag); f != nil {
			t.Errorf("%s still defines --%s (default %q); the code it implies does not exist",
				tc.cmd, tc.flag, f.DefValue)
		}
	}
}

// TestDeviceFlagExists: internal/ipod defines ErrMultipleDevices
// ("specify which one") and there was no way to specify.
func TestDeviceFlagExists(t *testing.T) {
	root := Root()
	f := root.PersistentFlags().Lookup("device")
	if f == nil {
		t.Fatal("root has no persistent --device flag")
	}
	if f.DefValue != "" {
		t.Errorf("--device default = %q, want empty", f.DefValue)
	}
	// Persistent root flags must be inherited by every subcommand.
	for _, name := range []string{"install", "flash", "recover", "info"} {
		c := findCmd(t, name)
		if c.InheritedFlags().Lookup("device") == nil {
			t.Errorf("%s cannot see the global --device flag", name)
		}
	}
}

func TestDeviceFlagIsReadable(t *testing.T) {
	root := Root()
	root.SetArgs([]string{"install", "--device", "/dev/sdz"})
	var got string
	install, _, err := root.Find([]string{"install"})
	if err != nil {
		t.Fatalf("find install: %v", err)
	}
	install.RunE = func(cmd *cobra.Command, args []string) error {
		got = deviceFlag(cmd)
		return nil
	}
	root.SetOut(&strings.Builder{})
	root.SetErr(&strings.Builder{})
	if err := root.Execute(); err != nil {
		t.Fatalf("execute: %v", err)
	}
	if got != "/dev/sdz" {
		t.Errorf("deviceFlag = %q, want /dev/sdz", got)
	}
}

func TestDeviceFlagDefaultsEmpty(t *testing.T) {
	if got := deviceFlag(Root()); got != "" {
		t.Errorf("deviceFlag on a bare root = %q, want empty", got)
	}
}

// TestStubsStillReportThemselves: the commands that aren't implemented
// must keep saying so rather than exiting 0 and looking like they did
// something.
func TestStubsStillReportThemselves(t *testing.T) {
	stubs := []string{"flash", "install", "update", "recover", "info", "debug", "sim", "test", "release"}
	for _, name := range stubs {
		t.Run(name, func(t *testing.T) {
			_, _, err := runCore(t, name)
			if err == nil {
				t.Fatalf("%s returned nil error; a stub must not look like success", name)
			}
			if !strings.Contains(err.Error(), "not yet implemented") {
				t.Errorf("%s error = %q, want it to say it isn't implemented", name, err)
			}
		})
	}
}

// TestLoadBearingCommandsExist guards the commands that actually work
// today, including the exact invocation core/Makefile's "ipod" target
// depends on.
func TestLoadBearingCommandsExist(t *testing.T) {
	for _, path := range [][]string{
		{"firmware", "pack"},
		{"firmware", "unpack"},
		{"tagcache", "build"},
		{"tagcache", "dump"},
		{"build"},
	} {
		cmd := findCmd(t, path...)
		if cmd.RunE == nil && cmd.Run == nil && !cmd.HasSubCommands() {
			t.Errorf("%v has no implementation", path)
		}
	}
	// core/Makefile: go run ./cmd/core firmware pack <bin> --out <ipod> --force
	pack := findCmd(t, "firmware", "pack")
	for _, flag := range []string{"out", "force"} {
		if pack.Flags().Lookup(flag) == nil {
			t.Errorf("firmware pack lost --%s, which core/Makefile depends on", flag)
		}
	}
}
