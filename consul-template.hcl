# This denotes the start of the configuration section for Consul. All values
# contained in this section pertain to Consul.
consul {
  # This block specifies the basic authentication information to pass with the
  # request. For more information on authentication, please see the Consul
  # documentation.
  auth {
    enabled  = false
    username = "test"
    password = "test"
  }
  address = "consul:8500"
  token = ""

  retry {
    enabled = true
    attempts = 12
    backoff = "250ms"
    max_backoff = "1m"
  }

  ssl {
    enabled = false
    verify = false
  }
}

reload_signal = "SIGHUP"
kill_signal = "SIGINT"
max_stale = "10m"
log_level = "warn"

wait {
  min = "1s"
  max = "10s"
}

# logging.
syslog {
  enabled = false
  facility = "LOCAL5"
}

deduplicate {
  enabled = false
}

# mode operates and the caveats of this mode.
exec {
  # This is the command to exec as a child process. There can be only one
  # command per Consul Template process.
  # command = "/usr/bin/app"

  # This is a random splay to wait before killing the command. The default
  # value is 0 (no wait), but large clusters should consider setting a splay
  # value to prevent all child processes from reloading at the same time when
  # data changes occur. When this value is set to non-zero, Consul Template
  # will wait a random period of time up to the splay value before reloading
  # or killing the child process. This can be used to prevent the thundering
  # herd problem on applications that do not gracefully reload.
  # splay = "5s"

  env {
    # This specifies if the child process should not inherit the parent
    # process's environment. By default, the child will have full access to the
    # environment variables of the parent. Setting this to true will send only
    # the values specified in `custom_env` to the child process.
    # pristine = false

    # This specifies additional custom environment variables in the form shown
    # below to inject into the child's runtime environment. If a custom
    # environment variable shares its name with a system environment variable,
    # the custom environment variable takes precedence. Even if pristine,
    # whitelist, or blacklist is specified, all values in this option
    # are given to the child process.
    # custom = ["PATH=$PATH:/etc/myapp/bin"]

    # This specifies a list of environment variables to exclusively include in
    # the list of environment variables exposed to the child process. If
    # specified, only those environment variables matching the given patterns
    # are exposed to the child process. These strings are matched using Go's
    # glob function, so wildcards are permitted.
    # whitelist = ["CONSUL_*"]

    # This specifies a list of environment variables to exclusively prohibit in
    # the list of environment variables exposed to the child process. If
    # specified, any environment variables matching the given patterns will not
    # be exposed to the child process, even if they are whitelisted. The values
    # in this option take precedence over the values in the whitelist.
    # These strings are matched using Go's glob function, so wildcards are
    # permitted.
    # blacklist = ["VAULT_*"]
  }

  # This defines the signal that will be sent to the child process when a
  # change occurs in a watched template. The signal will only be sent after the
  # process is started, and the process will only be started after all
  # dependent templates have been rendered at least once. The default value is
  # nil, which tells Consul Template to stop the child process and spawn a new
  # one instead of sending it a signal. This is useful for legacy applications
  # or applications that cannot properly reload their configuration without a
  # full reload.
  reload_signal = ""

  # This defines the signal sent to the child process when Consul Template is
  # gracefully shutting down. The application should begin a graceful cleanup.
  # If the application does not terminate before the `kill_timeout`, it will
  # be terminated (effectively "kill -9"). The default value is "SIGTERM".
  kill_signal = "SIGINT"

  # This defines the amount of time to wait for the child process to gracefully
  # terminate when Consul Template exits. After this specified time, the child
  # process will be force-killed (effectively "kill -9"). The default value is
  # "30s".
  kill_timeout = "2s"
}

# This block defines the configuration for a template. Unlike other blocks,
# this block may be specified multiple times to configure multiple templates.
# It is also possible to configure templates via the CLI directly.
template {
  source = "/opt/app/nginx/conf/vhost/test.iliubang.cn.conf.ctmpl"
  destination = "/opt/app/nginx/conf/vhost/test.iliubang.cn.conf"
  create_dest_dirs = true
  command = "/opt/app/nginx/sbin/nginx -s reload"
  command_timeout = "30s"
  error_on_missing_key = false
  perms = 0600
  backup = true

  left_delimiter  = "{{"
  right_delimiter = "}}"

  wait {
    min = "1s"
    max = "10s"
  }
}
