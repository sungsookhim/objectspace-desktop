# Security

Objectspace runs as the user, with three exceptions worth knowing: the optional change-tracking helper `osd-watchd` runs as root (a systemd system service), the lock screen checks the user's password through PAM (`/etc/pam.d/osd-lock`), and the session's compositor handles every program's input and windows.

## Reporting a problem

Please do not open a public issue for a security problem. Report it privately through GitHub: on the repository's **Security** tab, choose **Report a vulnerability**. Say what the problem is, how to reproduce it, and which version or commit you used.

You will get an answer within a week. A fix goes into the next release, and into a quick release of its own when the problem is serious; the report is credited in the release notes unless you would rather not be named.

## Supported versions

Only the latest release, and the `main` branch, get security fixes.
