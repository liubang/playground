package command

import (
	"bytes"
	"context"
	"errors"
	"os"
	"os/exec"
	"syscall"
)

func CommandOutput(ctx context.Context, dir, commandName string, params []string) (string, error) {
	cmd := exec.Command(commandName, params...)
	var output bytes.Buffer
	cmd.Stdout = &output
	cmd.Stderr = os.Stderr

	if dir == "" {
		cwd, err := os.Getwd()
		if err != nil {
			return "", err
		}
		cmd.Dir = cwd
	} else {
		cmd.Dir = dir
	}

	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	ch := make(chan error, 1)

	go func() {
		if err := cmd.Start(); err != nil {
			ch <- err
			close(ch)
			return
		}
		ch <- cmd.Wait()
		close(ch)
	}()

	select {
	case <-ctx.Done():
		if cmd.Process != nil {
			pid := cmd.Process.Pid
			if pid <= 0 {
				return output.String(), ctx.Err()
			}
			err := syscall.Kill(-pid, syscall.SIGKILL)
			if err != nil {
				return output.String(), errors.New(ctx.Err().Error() + "; " + err.Error())
			}
		}
		return output.String(), ctx.Err()

	case err := <-ch:
		return output.String(), err
	}
}
