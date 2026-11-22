# Security

rx-greeter is a login component. Please report vulnerabilities privately to the
repository owner rather than opening a public issue with exploit details.

## Scope

- Authentication is delegated to greetd and PAM. rx-greeter never reads
  `/etc/shadow` and never implements password checking itself.
- Passwords are held only in memory, cleared after use, and are never logged.
  The opt-in debug status file records only password length.
- The greetd request buffer containing a password is zeroed after it is sent.
- The power helper accepts only `suspend`, `poweroff`, or `reboot`; it does
  not invoke a shell with UI-provided strings.
- Do not install the helper setuid root unless you need that policy. A doas
  rule restricted to the `greetd` user and the fixed helper path is the
  preferred alternative.

## Reporting

Include the version, distribution, greetd version, and reproduction steps.
Avoid including real passwords.
