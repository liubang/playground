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

package ssh

import (
	"context"
	"fmt"
	"strings"

	"github.com/liubang/playground/go/tools/command"
	log "github.com/sirupsen/logrus"
)

func SshExec(ctx context.Context, ip, cmd string) (string, error) {
	shell := fmt.Sprintf(`ssh -q -o StrictHostKeyChecking=no work@%s "%s"`, ip, cmd)
	output, err := command.CommandOutput(ctx, "", "bash", []string{"-c", shell})
	if err != nil && err.Error() != "exit status 1" {
		log.Warnf("cmd: %s", shell)
		return "", err
	}
	ret := strings.TrimSpace(string(output))
	return ret, nil
}

func Scp(ctx context.Context, ip, from, dist string) error {
	shell := fmt.Sprintf("scp -q -o StrictHostKeyChecking=no work@%s:%s %s", ip, from, dist)
	_, err := command.CommandOutput(ctx, "", "bash", []string{"-c", shell})
	if err != nil && err.Error() != "exit status 1" {
		log.Warnf("cmd: %s", shell)
		return err
	}
	return nil
}
