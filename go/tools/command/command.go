// Copyright (c) 2025 The Authors. All rights reserved.
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
// Created: 2025/11/19 17:30

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
