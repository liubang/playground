package ssh

import (
	"context"
	"fmt"
	"strings"

	"github.com/playground/go/tools/command"
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
